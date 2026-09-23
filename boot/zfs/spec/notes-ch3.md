# ZFS On-Disk Specification (Sun, 2006, Draft) — 第 3 章の写し

## Chapter Three: Data Management Unit

> With the exception of a small amount of infrastructure, described in
> chapters 1 and 2, **everything in ZFS is an object**.

## §3.1 Objects

> Objects are defined by **512 bytes structures called dnodes**.

Illustration 9 (dnode_phys_t):

	uint8_t		dn_type;
	uint8_t		dn_indblkshift;
	uint8_t		dn_nlevels;
	uint8_t		dn_nblkptr;
	uint8_t		dn_bonustype;
	uint8_t		dn_checksum;
	uint8_t		dn_compress;
	uint8_t		dn_pad[1];
	uint16_t	dn_datablkszsec;
	uint16_t	dn_bonuslen;
	uint8_t		dn_pad2[4];
	uint64_t	dn_maxblkid;
	uint64_t	dn_secphys;
	uint64_t	dn_pad3[4];
	blkptr_t	dn_blkptr[N];		/* 可変長 */
	uint8_t		dn_bonus[BONUSLEN];	/* 可変長 */

### dn_indblkshift と dn_datablkszsec

> ZFS supports variable data and indirect block sizes ranging from
> **512 bytes to 128 Kbytes**.

	dn_indblkshift	間接ブロックの大きさ (バイト) の log2
	dn_datablkszsec	データブロックの大きさ (バイト) **を 512 で割った値**。
			1 (512 バイト) から 256 (128 Kバイト) まで

### dn_nblkptr と dn_blkptr

> dn_blkptr is a variable length field that can contains **between one
> and three** block pointers. The number ... is set at object allocation
> time and remains constant throughout the life of the dnode.

### dn_nlevels — 間接

> For a dnode using the largest data block size (128KB) and containing
> the maximum number of block pointers (3), the largest object size it
> can represent (without indirection) is **384 KB**.

> The number of block pointers that an indirect block can hold ... can be
> calculated by dividing the indirect block size by the size of a blkptr
> (**128 bytes**). The largest indirect block (128KB) can hold up to
> **1024 block pointers**.

> ZFS provides up to **six levels** of indirection to support files up to
> 2^64 bytes long.

### dn_maxblkid と block id

> The dn_maxblkid field in the dnode is set to the value of the largest
> data (level zero) block id for this object.

仕様書の例 (p.25):

> take an object which has 128KB sized indirect blocks. An indirect
> block of this size can hold 1024 block pointers. Given a level 0
> block id of 16360, it can be determined that block 15 (block id 15) of
> level 1 contains the block pointer for level 0 blkid 16360.
>
>	level 1 blkid = 16360%1024 = 15

**この式は誤り。** 16360 % 1024 = 1000 であって 15 ではない。
15 になるのは **除算** のほう: 16360 / 1024 = 15.97... → 15。
本文も "block 15 of level 1 contains the block pointer for ..." と
言っており、親を求めるのだから除算が正しい。

	level n-1 の blkid = level n の blkid / (間接ブロック 1 個が
			     持てる blkptr の数)

**実装では除算を使い、この誤植をコメントに残す。**

### dn_secphys

> The sum of all *asize* values for all block pointers (data and
> indirect) for this object.

### dn_bonus, dn_bonuslen, dn_bonustype

> The bonus buffer (dn_bonus) is defined as the space following a
> dnode's block pointer array (dn_blkptr). The amount of space is
> dependent on object type and can range between **64 and 320 bytes**.

Table 10 (bonus buffer types):

	DMU_OT_PACKED_NVLIST_SIZE	uint64_t		4
	DMU_OT_SPACE_MAP_HEADER		space_map_obj_t		7
	DMU_OT_DSL_DIR			dsl_dir_phys_t		12
	DMU_OT_DSL_DATASET		dsl_dataset_phys_t	16
	DMU_OT_ZNODE			znode_phys_t		17

### **Table 8 と Table 10 が食い違っている**

	Table 8 (§2.8)		Table 10 (§3.1)
	12  DMU_OT_DSL_DATASET	12  DMU_OT_DSL_DIR
	16  DMU_OT_DSL_OBJSET	16  DMU_OT_DSL_DATASET

同じ番号に別の名前が当たっている。**どちらかが誤り。**

二つ目の出典で決める: OpenZFS `include/sys/dmu.h` の `dmu_object_type_t`
は

	12  DMU_OT_DSL_DIR
	13  DMU_OT_DSL_DIR_CHILD_MAP
	14  DMU_OT_DSL_DS_SNAP_MAP
	15  DMU_OT_DSL_PROPS
	16  DMU_OT_DSL_DATASET

**Table 10 が正しく、Table 8 の 12 と 16 が誤り。** Table 8 の 13/14 も
名前が違う (CHILD_MAP / SNAP_MAP の綴り)。実装は Table 10 に従い、
食い違い自体をコメントに残す。

## §3.2 Object Sets

> Object sets are represented by a **1K byte objset_phys_t** structure.

Illustration 11 / 12:

	dnode_phys_t	metadnode;
	zil_header_t	os_zil_header;		/* 第 7 章 */
	uint64_t	os_type;
	char		os_pad[376];		/* Illustration 12 */

Table 11 (os_type):

	DMU_OST_NONE	0	未初期化
	DMU_OST_META	1	DSL object set (第 4 章)
	DMU_OST_ZFS	2	ZPL object set (第 6 章)
	DMU_OST_ZVOL	3	ZVOL object set (第 8 章)

### metadnode

> The collection of dnode_phys_t structures describing the objects in
> this object set are stored as an object pointed to by the metadnode.
> The data contained within this object is formatted as an **array of
> dnode_phys_t structures** (one for each object within the object set).

> Each object within an object set is uniquely identified by a 64 bit
> integer called an **object number**. An object's "object number"
> identifies the array element, in the dnode array, containing this
> object's dnode_phys_t.

**つまり object N を読むとは、metadnode が指すオブジェクトの
offset N * 512 から 512 バイト読むこと。**

---

## この版で足りない所

	dn_used の単位	§3.1 の dn_secphys は「asize の総和」。現行は
			dnode に DNODE_FLAG_USED_BYTES があり、バイト単位
			のこともある。読み出しには使わないので当面不要
	dnode の大きさ	§3.1 は 512 バイト固定。現行は large_dnode 機能で
			512 の倍数になりうる (dn_extra_slots)。
			→ OpenZFS include/sys/dnode.h
	os_pad		Illustration 12 の 376 バイトのうち、現行は
			os_flags と user/group used object を使う
