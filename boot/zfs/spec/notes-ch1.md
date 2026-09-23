# ZFS On-Disk Specification (Sun, 2006, Draft) — 第 1 章の写し

出典: *ZFS On-Disk Specification*, Draft, Sun Microsystems, 2006.
実装に定数を書くときは、この節番号をコメントに添える。

## §1.1 Virtual Devices

vdev は木。葉が physical vdev (書き込める block device)、内部が logical
vdev。root vdev が木を根で束ね、その直下が top-level vdev。

## §1.2 Vdev Labels

> Each physical vdev within a storage pool contains a **256KB** structure
> called a vdev label.

label は「pool の中身への入口」と「整合性の確認」の二役。

### §1.2.1 Label Redundancy

> Four copies of the vdev label are written to each physical vdev.
> ZFS places two labels at the front of the device and two labels at the
> back.

サイズ N のデバイス上の配置 (Illustration 2):

	L0	0
	L1	256K
	L2	N - 512K
	L3	N - 256K

**前後に分けるのは、破損が連続した塊で起きるという前提から。**

### §1.2.2 Transactional Two Staged Label Update

label は copy-on-write ではなく上書き。**偶数 (L0, L2) を先に書き、
安定してから奇数 (L1, L3) を書く。** どの時点で落ちても片方は有効。

## §1.3 Vdev Technical Details

label 256KB の内訳 (Illustration 3):

	0     .. 8K	blank space		§1.3.1
	8K    .. 16K	boot block header	§1.3.2
	16K   .. 128K	name/value pairs	§1.3.3  (112KB)
	128K  .. 256K	uberblock array		§1.3.4  (128KB, 1K ごと)

### §1.3.1 Blank Space

> VTOC labels must be written to the first 8K of slice 0. Thus, to
> support VTOC labels, the first 8k of the vdev_label is left empty.

### §1.3.2 Boot Block Header

8K、予約。「将来の appendix で述べる」とあり、この版には記述が無い。

### §1.3.3 Name-Value Pair List

> All name-value pairs are stored in **XDR encoded nvlists**.

label 直下に在る対:

	version		UINT64	on-disk format version。この版では "1"
	name		STRING	pool 名
	state		UINT64	Table 1 参照
	txg		UINT64	この label を書いた transaction group
	pool_guid	UINT64	pool の guid
	top_guid	UINT64	この subtree の top-level vdev の guid
	guid		UINT64	この vdev の guid
	vdev_tree	NVLIST	subtree の記述 (再帰)

Table 1 (pool state):

	POOL_STATE_ACTIVE	0
	POOL_STATE_EXPORTED	1
	POOL_STATE_DESTROYED	2

vdev_tree の要素 (すべての type に全部が在るわけではない):

	type		STRING	Table 2
	id		UINT64	親の children 配列における添字
	guid		UINT64
	path		STRING	葉のみ
	devid		STRING	type=disk のみ
	metaslab_array	UINT64	space map の object 番号の配列を持つ object
	metaslab_shift	UINT64	metaslab サイズの log2
	ashift		UINT64	この top-level vdev の最小割り当て単位の log2。
				**「RAIDz では 10、それ以外は 9」** (この版の記述)
	asize		UINT64	この top-level vdev から割り当てられる量
	children	NVLIST_ARRAY

Table 2 (vdev type):

	"disk"		葉: block storage
	"file"		葉: file storage
	"mirror"	内部: mirror
	"raidz"		内部: raidz
	"replacing"	内部: mirror の変種、置換中に使う
	"root"		内部: 木の根

### §1.3.4 The Uberblock

> Immediately following the nvpair lists in the vdev label is an array of
> uberblocks. ... **The uberblock with the highest transaction group
> number and valid SHA-256 checksum is the active uberblock.**

> To ensure constant access to the active uberblock, the active uberblock
> is never overwritten. Instead, all updates to an uberblock are done by
> writing a modified uberblock to another element of the uberblock array.

uberblock は**machine の native endian** で置かれる。

	ub_magic	uint64_t	0x00bab10c ("oo-ba-block")
	ub_version	uint64_t	この版では 0x1
	ub_txg		uint64_t
	ub_guid_sum	uint64_t
	ub_timestamp	uint64_t
	ub_rootbp	blkptr_t

Table 3 (ディスク上で見える ub_magic):

	Big Endian	0x00bab10c
	Little Endian	0x0cb1ba00

**endian の見分けは magic の並びで付く。** これが判定の根拠になる。

---

## この版で足りない所 (二つ目の文書が要る)

**仕様書は 2006 年の Draft で version 1 まで。** NetBSD 11 が作る pool は
version 5000 (feature flags) なので、以下はこの文書からは読めない。
実装に入れる前に、それぞれ出典を別に立てること。

	uberblock 配列の間隔	この版は「1K ごと」。現行は
				VDEV_UBERBLOCK_SHIFT = MAX(ashift, 10) で、
				ashift=12 の pool では 4K 間隔になる。
				→ OpenZFS include/sys/vdev_impl.h で確かめる
	version 5000		feature flags。§1.3.3 の version の意味が
				変わる
	lz4 / zstd		§2.5 の圧縮表に無い
	embedded blkptr		§2 の blkptr に無い (BP_IS_EMBEDDED)
	ashift の既定		この版の「raidz は 10、他は 9」は現行と違う
