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
	  block comment (expansion) と vdev_raidz_map_alloc_expanded()
					拡張した raidz の古い block は古い
					幅で置かれている
	module/zfs/zio.c
	  zio_vdev_child_io()		leaf への I/O は VDEV_LABEL_START_SIZE
					を足した番地
	  zio_vdev_io_start()		I/O は top-level vdev の 1 << ashift
					に切り上げ、書き込みは 0 で埋める
	include/sys/fs/zfs.h
	  ZPOOL_CONFIG_RAIDZ_EXPANDING, ZPOOL_CONFIG_RAIDZ_EXPAND_TXGS,
	  VDEV_RAIDZ_MAXPARITY

## 読むもの、読まないもの

読む: raidz1/2/3、全部の子が在るとき、parity の数までの列が欠けたか
checksum で合わないとき。欠けた列は必ず含め、checksum 不一致なら残りを
少ない順に組み合わせ、読める parity から同じ数を選んで GF(2^8) の連立
方程式を解く。OpenZFS の解き方 (matrix の作り方や特殊化) は写していない。
power と log の表は 2 倍の定義から作る。

断る:

- 拡張した、あるいは拡張中の raidz (label に `raidz_expand_txgs` か
  `raidz_expanding`)。古い block の幅を txg から決める処理を書いていない
- parity の数より多い列が悪いとき
- label の nparity が子の数以上 (data 列が無い)

## 測ったもの (2026-09-30)

raidz3 を 7 台で作り、3 台を外して、どの parity で何列を解いたかを
数えた。P、Q、R 単独、PQ、PR、QR、PQR (三列を一度に) が全部使われ、
4 MB の file はどの組でも ZFS と一致した。5 台の raidz3 では block の
data 列は 2 本までなので、三列の場合は出ない。openzfs.sh は 7 台の場合も
回す。
