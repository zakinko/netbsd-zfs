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

読む: raidz1/2/3、全部の子が在るとき、子が一つ欠けたとき、一列が
checksum で合わないとき (P から作り直す)。

断る:

- 拡張した、あるいは拡張中の raidz (label に `raidz_expand_txgs` か
  `raidz_expanding`)。古い block の幅を txg から決める処理を書いていない
- 二列以上が悪いとき。raidz2/3 でも Q と R (GF(2^8) の計算、
  vdev_raidz.c の vdev_raidz_reconstruct_*) を使った作り直しは書いていない
- label の nparity が子の数以上 (data 列が無い)
