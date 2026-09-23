# ZFS On-Disk Specification (Sun, 2006, Draft) — 第 5 章の写し

## Chapter Five: ZAP (ZFS Attribute Processor)

> A ZAP object is a DMU object used to store attributes in the form of
> name-value pairs. The name portion ... is a zero-terminated string of
> **up to 256 bytes** (including terminating NULL). The value portion
> ... is an array of integers whose size is only limited by the size of a
> ZAP data block.

Table 13 (ZAP になる object type): OBJECT_DIRECTORY, DSL_DIR_CHILD_MAP,
DSL_DS_SNAP_MAP, DSL_PROPS, DIRECTORY_CONTENTS, MASTER_NODE,
DELETE_QUEUE, ZVOL_PROP。

**Table 13 は Table 10 の名前を使っている** (DSL_DIR_CHILD_MAP,
DSL_DS_SNAP_MAP)。§3.1 で Table 8 と Table 10 が食い違ったとき Table 10 を
採ったが、この表もそちらに付いている。

## micro か fat か

microzap を使うのは**三つとも**満たすとき:

- 全部の対が一ブロックに収まる (最大 128KB、そこに microzap は 2047 個)
- **値が全部 uint64_t**
- **名前が NULL 込みで 50 文字以内**

一つでも外れたら fatzap。

## ブロックの先頭語で種別が分かる

> The **first 64 bit word in each block** of a ZAP object is used to
> identify the type of ZAP contents contained within this block.

Table 14:

	ZBT_MICRO	(1ULL << 63) + 3
	ZBT_HEADER	(1ULL << 63) + 1	fatzap の最初のブロックだけ
	ZBT_LEAF	(1ULL << 63) + 0	fatzap のそれ以外

**読み出しはまずこの語を見て分岐する。**

## §5.1 The Micro Zap

> A microzap object consists of a **single block** containing an array of
> microzap entries.

> the **first 128 bytes** of the block contain a microzap header ...
> a 64 bit ZBT_MICRO value ... Following this value is a **64 bit salt**
> ... The next **42 bytes** of this header is intentionally left blank
> and the **last 64 bytes contain the first microzap entry**.

	offset 0	uint64_t	mz_block_type = ZBT_MICRO
	offset 8	uint64_t	mz_salt
	offset 16	42 バイト	blank
	offset 64	64 バイト	最初の mzap_ent_phys_t
	offset 128..	mzap_ent_phys_t の配列

**8 + 8 + 42 = 58 で 64 に足りない。** 図 (Illustration 15) は
「先頭 128 バイトのうち後ろ 64 バイトが最初の entry」と言っており、
entry が 64 の倍数の境界に来るのはそちらで、42 は 48 の誤りと読める。
実装は **offset 64 から 64 バイト刻み**とし、blank の長さを当てにしない。

	#define MZAP_ENT_LEN	64
	#define MZAP_NAME_LEN	(MZAP_ENT_LEN - 8 - 4 - 2)	/* = 50 */

	typedef struct mzap_ent_phys {
		uint64_t	mze_value;
		uint32_t	mze_cd;		/* collision differentiator */
		uint16_t	mze_pad;
		char		mze_name[MZAP_NAME_LEN];
	} mzap_ent_phys_t;

**microzap には hash が要らない。** entry の数は
(ブロック長 - 64) / 64 で、名前を順に比べればよい。salt も cd も、
書くときの衝突処理のためのもの。

## §5.2 The Fat Zap

> All entries in a fatzap object are arranged based on a **64 bit hash of
> the attribute's name**. The hash is used to index into a **pointer
> table** ... The pointer table entries reference a chain of fatzap
> blocks called **leaf blocks** (zap_leaf_phys). Each leaf block is
> broken up into some number of **chunks**.

### §5.2.1 zap_phys_t — 最初のブロック

	uint64_t	zap_block_type;		/* ZBT_HEADER */
	uint64_t	zap_magic;		/* 0x2F52AB2AB */
	struct zap_table_phys {
		uint64_t	zt_blk;		/* 外付けのときの先頭 blkid */
		uint64_t	zt_numblks;	/* 外付けのときのブロック数 */
		uint64_t	zt_shift;	/* hash の上位何 bit を使うか */
		uint64_t	zt_nextblk;
		uint64_t	zt_blks_copied;
	} zap_ptrtbl;
	uint64_t	zap_freeblk;
	uint64_t	zap_num_leafs;
	uint64_t	zap_num_entries;
	uint64_t	zap_salt;
	uint64_t	zap_pad[8181];
	uint64_t	zap_leafs[8192];

	zap_magic = 0x2F52AB2AB		("zfs-zap-zap")

> If the pointer table is contained within the zap_phys, **zt_shift will
> be 13**.

> zap_leafs[8192]: ... If the pointer table has fewer than 2^13 entries,
> the pointer table will be stored here. If not, this field is unused.

**zt_blk が 0 なら表は zap_leafs[] の中、そうでなければ blkid zt_blk から
zt_numblks ブロック。**

語数は 2 + 5 + 4 + 8181 + 8192 = 16384 語 = 128KB で、本文の
「128KB zap_phys_t」と合う。**ただしこれは block size が 128KB のとき。**
zap_leafs[] の実際の要素数は (ブロック長 / 2 / 8) で、
内蔵の表はブロックの後ろ半分に在ると読むのが整合する
(zt_shift = 13 は 8192 個 = 64KB = 128KB の半分)。
**実装は zt_shift から数えるので、8192 を直接書かない。**

### §5.2.2 Pointer Table

> The value used to index into the pointer table is called the **prefix**
> and is the **zt_shift high order bits** of the 64 bit computed hash.

各要素は 64 bit で、**level 0 の blkid**。

### §5.2.3 zap_leaf_phys_t

	struct zap_leaf_header {		/* 2 個分の 24 バイト chunk */
		uint64_t	lh_block_type;	/* ZBT_LEAF */
		uint64_t	lh_next;	/* 次の leaf の blkid */
		uint64_t	lh_prefix;
		uint32_t	lh_magic;	/* 0x2AB1EAF */
		uint16_t	lh_nfree;
		uint16_t	lh_nentries;
		uint16_t	lh_prefix_len;
		uint16_t	lh_freelist;
		uint8_t		lh_pad2[12];
	} l_hdr;
	uint16_t		l_hash[ZAP_LEAF_HASH_NUMENTRIES];
	union zap_leaf_chunk	l_chunk[ZAP_LEAF_NUMCHUNKS];

**仕様書は同じ欄を `lhr_` と `lh_` の二通りで書いている**
(構造体定義は lhr_block_type, 説明文の途中から lh_freelist)。同じもの。

> Each leaf (or chain of leafs) stores the ZAP entries whose **first
> lh_prefix_len bits** of their hash value equals lh_prefix.

> lh_prefix_len can be **equal to or less than zt_shift** ... in which
> case multiple pointer table buckets reference the same leaf.

#### leaf hash

> The next **8KB** of the zap_leaf_phys_t is the zap leaf hash table.
> **Twelve bits** (the twelve following the lh_prefix_len used to
> uniquely identify this block) of the attribute's hash value are used to
> index into this table.

> Each bucket in the table contains a **16 bit integer which is the index
> into the zap_leaf_chunk array**.

12 bit = 4096 要素、16 bit なので 8KB。**これも 128KB ブロック前提**で、
現行は (ブロック長 / 32) 要素。実装はブロック長から数える。

### §5.2.4 zap_leaf_chunk

chunk は 24 バイトの共用体で、三種:

	ZAP_LEAF_ENTRY	252
	ZAP_LEAF_ARRAY	251
	ZAP_LEAF_FREE	253

	struct zap_leaf_entry {
		uint8_t		le_type;	/* 252 */
		uint8_t		le_int_size;	/* 値の 1 要素のバイト数 */
		uint16_t	le_next;	/* 鎖の次、末尾は 0xffff */
		uint16_t	le_name_chunk;
		uint16_t	le_name_length;	/* NULL を含む */
		uint16_t	le_value_chunk;
		uint16_t	le_value_length;/* le_int_size 単位の個数 */
		uint16_t	le_cd;
		uint8_t		le_pad[2];
		uint64_t	le_hash;
	} l_entry;

	struct zap_leaf_array {
		uint8_t		la_type;	/* 251 */
		uint8_t		la_array[ZAP_LEAF_ARRAY_BYTES];	/* 21 */
		uint16_t	la_next;	/* 末尾は 0xffff */
	} l_array;

	struct zap_leaf_free {
		uint8_t		lf_type;	/* 253 */
		uint8_t		lf_pad[ZAP_LEAF_ARRAY_BYTES];
		uint16_t	lf_next;
	} l_free;

1 + 21 + 2 = 24 バイト。**chunk の大きさは 24。**
本文の「l_hdr は 2 個分の 24 バイト chunk」= 48 バイトと合う。

> la_array: 21 byte array containing the name or value's value.
> **Values of type "integer" are always stored in big endian format,
> regardless of the machine's native endianness.**

**これは blkptr や dnode と逆。** ZAP の値だけは常に big endian。

#### 引き方

	hash = zap_hash(salt, name)
	prefix = hash >> (64 - zt_shift)
	blkid  = pointer_table[prefix]
	leaf   = その blkid の level 0 ブロック
	h      = (hash >> (64 - lh_prefix_len - 12)) & 0xfff
	chunk  = l_hash[h]
	while chunk != 0xffff:
		e = l_chunk[chunk].l_entry
		if e.le_hash == hash and 名前が一致: 見つかった
		chunk = e.le_next
	名前と値は le_name_chunk / le_value_chunk から la_next を辿って集める

---

## この版で足りない所

	zap_hash	**仕様書は hash 関数そのものを書いていない。**
			「64 bit hash of the attribute's name」「salt を
			混ぜる」としか言わない。これが無いと fatzap は
			引けない。

			2026-09-23 に openzfs/zfs master を引いて確かめた
			(module/zfs/zap_impl.c の zap_hash、
			 include/sys/dmu.h の ZFS_CRC64_POLY):

			  h = zap_salt
			  名前の各バイト c について
			      h = (h >> 8) ^ crc64[(h ^ c) & 0xff]
			  h &= ~((1 << (64 - hashbits)) - 1)

			多項式は ECMA-182 の reflected form
			0xC96C5795D7870F42。表は reflected の作り方で、
			**table[128] == 多項式** になる
			(zap_hash の ASSERT がそれを見ている)。
			これは組んだ表が正しいかの検算に使える。

			hashbits は ZAP_FLAG_HASH64 が立っていれば 48、
			でなければ **28**。**下位 bit を 0 にするのは
			collision differentiator の場所を空けるため**で、
			hash の上位から bucket を選ぶのはそのため。

			**終端の NULL は hash に入れない。** 原文の注:
			"We previously stored the terminating null on disk,
			 but didn't hash it, so we need to continue to not
			 hash it."  これを間違えると全部 ENOENT になる。

			表は走らせる前に作ること。前に carry した実装で
			表が全部 0 のまま引いていて、ZAP の引きが全部
			ENOENT になった
	ZAP_LEAF の数	仕様書に式が無い。openzfs/zfs include/sys/zap_leaf.h
			(同じ日に引いた) は block shift bs から

			  ZAP_LEAF_CHUNKSIZE		24
			  ZAP_LEAF_ARRAY_BYTES		24 - 3 = 21
			  ZAP_LEAF_HASH_NUMENTRIES	1 << (bs - 5)
			  ZAP_LEAF_NUMCHUNKS
			      ((1 << bs) - 2 * NUMENTRIES) / 24 - 2

			と数える。**- 2 は header が chunk 二つ分を
			食っているから**で、§5.2.3 の「2 24-byte chunks」と
			合う。hash 表が 1 要素 2 バイトなので
			2 * NUMENTRIES = ブロック長 / 16。
			bs = 17 (128KB) を入れると NUMENTRIES = 4096、
			hash 表 8KB で、§5.2.3 の「次の 8KB」と一致する
	ブロック長依存	§5.2 の 8192 / 8KB / 12 bit は全部 128KB ブロック
			前提の数。実装は dn_datablkszsec から数える
	microzap の 42	上記のとおり 48 と読むべき所
	le_cd の幅	mzap_ent_phys_t の mze_cd は 32 bit、
			zap_leaf_entry の le_cd は 16 bit。図
			(Illustration 18) は le_cd を uint32_t と書いており、
			**構造体定義と図が食い違う**。構造体定義のほうが
			1+1+2*6+2+8 = 24 バイトに収まるので、そちらを採る
