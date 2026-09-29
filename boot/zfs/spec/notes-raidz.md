# raidz — 仕様書にあること、ないこと

2026-09-29 に書いた。

## [S] にあること

	§1.3.3 Table 2	vdev の種類として "raidz"
	§2.1		DVA の GRID: "Raid-Z layout information, reserved
			for future use"
	§2.6		asize は raidz の parity を含む。圧縮なしでも
			raidz なら lsize/psize/asize が一致しない

**配置そのもの (どの子のどこに何が在るか) は書かれていない。** 他に
規範的な文書も見つからなかった (2026-09-27 に探した: OpenZFS の文書の
RAIDZ の頁は概念の説明、Max Bruning の 2009 年の blog はコードからの
読み解き、iXsystems の white paper は概観)。user の判断 (2026-09-29):
仕様書にないものは OpenZFS を出典にする。

## [Z] openzfs/zfs 81b19c6 (2026-09-28) から引いたもの

	module/zfs/vdev_raidz.c
	  vdev_raidz_init()		nparity は 1..VDEV_RAIDZ_MAXPARITY、
					label に無ければ 1 (SPA_VERSION_RAIDZ2
					より前の pool)
	  vdev_raidz_map_alloc()	block の子への割り当て: 開始列
					b % dcols、q と r と bc、q == 0 の
					ときの acols/scols、parity は先頭 np
					列、単一 parity は 1MB おきに先頭二列
					を入れ替える
	  vdev_raidz_map_alloc_read()	data 列は np 列目から順に block の
					バイトを持つ
	  vdev_raidz_generate_parity_p()/_pq()
					P は data 列の XOR、短い列は末尾を 0
					とみなす
	  冒頭の block comment		GF(2^8) の定義 (加算は XOR、2 倍は
					(a << 1) ^ (a & 0x80 ? 0x1d : 0)、
					x^8+x^4+x^3+x^2+1)、Q = Σ 2^(n-1-i) D_i、
					R = Σ 4^(n-1-i) D_i。この comment は
					Anvin の "The mathematics of RAID-6" と
					Plank の Reed-Solomon の tutorial を
					方式の出典に挙げている
	  vdev_raidz_combrec(), raidz_reconstruct()
					checksum 不一致のとき、壊れた数の
					少ない組み合わせから試す
	  block comment (expansion)	拡張した raidz の古い block は古い
					幅で置かれ、行ごとに新しい子へ移される
	  vdev_raidz_init()		raidz_expand_txgs (昇順) と
					raidz_expanding から、txg ごとの幅と
					元の幅を決める。拡張中はどの幅も
					もう一つ狭い
	  vdev_raidz_io_start()		block の幅は BP_GET_PHYSICAL_BIRTH の
					txg で決まる: それ以下で最大の txg の
					幅、無ければ元の幅。幅が今の子の数と
					同じなら従来の map、違えば expanded
	  vdev_raidz_map_alloc_expanded()
					一行に一 sector、行の幅は論理幅、
					行の開始 sector b は offset/sec +
					row * 論理幅、子は b % 行の物理幅。
					data sector の置き場 dc * rows + row
					(r 列目以降は一つ少ない)。最後の行は
					全幅として parity を取る。拡張中は
					synced offset を跨ぐ行は古い場所
					(物理幅 - 1)、下の行は新しい場所、
					scratch が有効ならそこから
					VDEV_BOOT_SIZE を引いた番地
	  raidz_simulate_failure()	論理子: 今の子、その前の幅の子、…
					の順に番号を振り、sector
					phys * (offset >> ashift) + devidx を
					各幅で割った余りで列の所属を決める
	  vdev_raidz_combrec()		論理子 1..np 個の組を順に試す
	  raidz_reflow_scratch_sync()	scratch は各子の VDEV_BOOT_OFFSET
					(512K) から VDEV_BOOT_SIZE を sector
					に揃えた長さ
	module/zfs/zio.c
	  zio_vdev_child_io()		leaf への I/O は VDEV_LABEL_START_SIZE
					を足した番地
	  zio_vdev_io_start()		I/O は top-level vdev の 1 << ashift
					に切り上げ、書き込みは 0 で埋める
	include/sys/fs/zfs.h
	  ZPOOL_CONFIG_RAIDZ_EXPANDING, ZPOOL_CONFIG_RAIDZ_EXPAND_TXGS,
	  VDEV_RAIDZ_MAXPARITY
	include/sys/uberblock_impl.h
	  struct uberblock		ub_rootbp の後の六語、最後が
					ub_raidz_reflow_info
	  RRSS_GET_OFFSET/STATE		下位 55 bit が 512 byte 単位の
					offset、上の 9 bit が状態、
					RRSS_SCRATCH_VALID は 1
	include/sys/vdev_impl.h
	  VDEV_BOOT_SIZE		7 << 19 (3.5MB)。[S] は 4MB の内
					label 以外の部分に大きさを与えていない
	module/nvpair/nvpair.c
	  nvs_xdr_nvp_op()		uint64 の配列は RFC 4506 §4.13 の
					可変長配列: 数、続いて 8 byte ずつ
	  i_validate_type_nelem()	boolean は要素 0、値なし

## 読むもの、読まないもの

読む: raidz1/2/3、全部の子が在るとき、parity の数までの列が欠けたか
checksum で合わないとき。欠けた列は必ず含め、checksum 不一致なら論理子
を少ない順に組み合わせ、各行で読める parity から同じ数を選んで GF(2^8)
の連立方程式を解く。OpenZFS の解き方 (matrix の作り方や特殊化) は写して
いない。power と log の表は 2 倍の定義から作る。

拡張した raidz と拡張中の raidz も読む (2026-09-30)。拡張中は、選んだ
uberblock の ub_raidz_reflow_info が示す位置で新旧の場所を分け、scratch
が有効なら boot area から読む。

断る:

- parity の数より多い列が悪いとき
- label の nparity が元の幅 (子の数 - raidz_expand_txgs の数 - 拡張中
  なら 1) 以上のとき (data 列が無い)
- raidz_expand_txgs が昇順でないか、16 より多いとき (OpenZFS が書かない
  label)

## 測ったもの (2026-09-30)

raidz3 を 7 台で作り、3 台を外して、どの parity で何列を解いたかを
数えた。P、Q、R 単独、PQ、PR、QR、PQR (三列を一度に) が全部使われ、
4 MB の file はどの組でも ZFS と一致した。5 台の raidz3 では block の
data 列は 2 本までなので、三列の場合は出ない。openzfs.sh は 7 台の場合も
回す。

## 拡張を測ったもの (2026-09-30)

OpenZFS 2.4.1 で作った pool。`test/openzfs.sh` の raidzx 以下。

	raidzx1		raidz1 3 台を 4 台に。拡張の前と後の file、全部、
			一台欠け、一台 0 埋め。二台悪いと読めない
	raidzx2		raidz2 4 台を 5 台、6 台と二度。三つの幅の
			file、二台欠け、一台 0 埋めと一台欠け
	raidzxp		raidz1 3 台に 40MB 書いて 4 台目を付け、20MB で
			止めて export。uberblock の reflow offset は
			160MB 前後で、その上と下と跨ぐ行が在る
	raidzxs		scratch が有効な状態 (下記)

scratch が有効な状態は ztest の pause point でしか止められず、Linux の
module parameter には無い。そこで `raidz_expand_max_reflow_bytes=1` で
止め、各子の新しい場所の先頭 VDEV_BOOT_SIZE を boot area に写して元を
0 で埋め、最新 txg の uberblock を RRSS_SCRATCH_VALID、offset は scratch
の論理長 (14MB) に書き換えて SHA-256 を付け直した。zdb -lu がその
uberblock を state=1 と読むことで書き換えを確かめた。書き換え前の印では
5 file 中 4 つが読めず、書き換え後は全部読める。

1 で止めても move は scratch の先まで進む (34MB 前後)。作り替えた状態では
その間の行は古い場所から読まれ、block がそこに無い限り正しい。全 file が
SHA-256 で一致することが、その前提が成り立っている証拠になっている。

変異 (各一行を殺して同じ試験を回した):

	txg から幅を引かない (常に今の幅)	32 件失敗
	reflow offset を見ない		23 件失敗 (raidzxp は mount できない)
	scratch を見ない			raidzxs の 4 file 失敗

試験を作っていないもの: 論理子のうち拡張前の幅の分、つまり拡張の前に
黙って壊れた値を返していた子の組み合わせ。OpenZFS の
raidz_simulate_failure() と同じ式で書いたが、その場合を作る手段を
用意していない。
