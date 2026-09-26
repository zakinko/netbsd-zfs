# ZFS On-Disk Specification (Sun, 2006, Draft) — 第 2 章の写し

## Chapter Two: Block Pointers and Indirect Blocks

> A block pointer (blkptr_t) is a **128 byte** ZFS structure used to
> physically locate, verify, and describe blocks of data on disk.

Illustration 8 の並び (64 bit word 単位、word 内は bit 63 が左):

	word 0	vdev1 (32) | GRID (8) | ASIZE (24)
	word 1	G (1) | offset1 (63)
	word 2	vdev2 (32) | GRID (8) | ASIZE (24)
	word 3	G (1) | offset2 (63)
	word 4	vdev3 (32) | GRID (8) | ASIZE (24)
	word 5	G (1) | offset3 (63)
	word 6	E (1) | lvl | type (8) | cksum (8) | comp (8) |
		PSIZE (16) | LSIZE (16)
	word 7	padding
	word 8	padding
	word 9	padding
	word a	birth txg
	word b	fill count
	word c	checksum[0]
	word d	checksum[1]
	word e	checksum[2]
	word f	checksum[3]

## §2.1 DVA — Data Virtual Address

> The *vdev* portion of each DVA is a 32 bit integer which uniquely
> identifies the vdev ID containing this block. The *offset* portion of
> the DVA is a 63 bit integer holding the offset (**starting after the
> vdev labels (L0 and L1) and boot block**) within that device.

> The value stored in *offset* is the offset in terms of sectors (512
> byte blocks).

**変換式 (§2.1 に式として書かれている):**

	physical block address = (offset << 9) + 0x400000 (4MB)

0x400000 は「二つの vdev label と boot block の大きさ」。
DVA は最大三つ (dva1/dva2/dva3) で、使う数を "wideness" と呼ぶ。

## §2.2 GRID

> Raid-Z layout information, reserved for future use.

## §2.3 GANG

G bit (Table 4): 0 = non-gang、1 = gang block。

> Gang blocks are **512 byte** sized, self checksumming blocks. A gang
> block contains **up to 3 block pointers** followed by a 32 byte
> checksum.

	struct zio_gbh {
		blkptr_t	zg_blkptr[SPA_GBH_NBLKPTRS];
		uint64_t	zg_filler[SPA_GBH_FILLER];
		zio_block_tail_t zg_tail;
	};

	struct zio_block_tail {
		uint64_t	zbt_magic;	/* 0x210da7ab10c7a11 */
		zio_cksum_t	zbt_cksum;
	};

	zbt_magic = 0x210da7ab10c7a11	("zio-data-bloc-tail")

## §2.4 Checksum

Table 5:

	on		1	fletcher2
	off		2	none
	label		3	SHA-256
	gang header	4	SHA-256
	zilog		5	fletcher2
	fletcher2	6	fletcher2
	fletcher4	7	fletcher4
	SHA-256		8	SHA-256

> If the cksum value is 2 (off), a checksum will not be computed and
> checksum[0..3] will be zero.

> **Note: The computed checksum is always of the data, even if this is a
> gang block.** Gang blocks and zilog blocks are self checksumming.

## §2.5 Compression

Table 6:

	on	1	lzjb
	off	2	none
	lzjb	3	lzjb

## §2.6 Block Size

	lsize	論理サイズ。圧縮・raidz・gang の overhead を含まない
	psize	圧縮後のディスク上のサイズ
	asize	このデータを置くために割り当てた総量。gang header や
		raidz の parity を含む

> If compression is turned off and ZFS is not on Raid-Z storage, lsize,
> asize, and psize will all be equal.

> **All sizes are stored as the number of 512 byte sectors (minus one)**
> needed to represent the size of this block.

**「minus one」が肝。** 格納値 + 1 が sector 数。

## §2.7 Endian

Table 7:

	Little Endian	1
	Big Endian	0

> Blocks are always written out in the machine's native endian format.
> If a pool is moved to a machine with a different endian format, the
> contents of the block are byte swapped on read.

## §2.8 Type

Table 8 (object types):

	DMU_OT_NONE			0
	DMU_OT_OBJECT_DIRECTORY		1
	DMU_OT_OBJECT_ARRAY		2
	DMU_OT_PACKED_NVLIST		3
	DMU_OT_NVLIST_SIZE		4
	DMU_OT_BPLIST			5
	DMU_OT_BPLIST_HDR		6
	DMU_OT_SPACE_MAP_HEADER		7
	DMU_OT_SPACE_MAP		8
	DMU_OT_INTENT_LOG		9
	DMU_OT_DNODE			10
	DMU_OT_OBJSET			11
	DMU_OT_DSL_DATASET		12
	DMU_OT_DSL_DATASET_CHILD_MAP	13
	DMU_OT_OBJSET_SNAP_MAP		14
	DMU_OT_DSL_PROPS		15
	DMU_OT_DSL_OBJSET		16
	DMU_OT_ZNODE			17
	DMU_OT_ACL			18
	DMU_OT_PLAIN_FILE_CONTENTS	19
	DMU_OT_DIRECTORY_CONTENTS	20
	DMU_OT_MASTER_NODE		21
	DMU_OT_DELETE_QUEUE		22
	DMU_OT_ZVOL			23
	DMU_OT_ZVOL_PROP		24

## §2.9 Level

> the number of levels (number of block pointers which need to be
> traversed to arrive at this data)

## §2.10 Fill

> The fill count describes the number of non-zero block pointers under
> this block pointer. The fill count for a data block pointer is 1.

DMU_OT_DNODE のときだけ意味が違い、**その下にある空き dnode の数**。

## §2.11 Birth Transaction

birth txg = この blkptr を割り当てた transaction group。

## §2.12 Padding

word 7,8,9 は予約。

---

## この版で足りない所 (二つ目の文書が要る)

	compression	§2.5 の表は 3 までしかない。現行は
			gzip-1..9 (5..13)、zle (14)、lz4 (15)、zstd (16)。
			**NetBSD 11 の pool は既定で lz4** なので必須。
			→ OpenZFS include/sys/zio_compress.h で確かめる
	checksum	§2.4 の表は 8 まで。現行は noparity(10)、
			sha512(11)、skein(12)、edonr(13)、blake3(14)。
			label と gang header は今も SHA-256
	embedded BP	§2 の blkptr に無い。現行は word 6 の一 bit を
			BP_EMBEDDED に使い、そのとき DVA 領域にデータを
			直接置く。**これを知らないと壊れた blkptr に見える**
	word 7,8,9	§2.12 は「予約」。現行は word 7 が blk_prop2、
			word 8 が blk_pad、word 9 が physical birth txg
			(blk_birth_word[0]、0 なら word a の logical birth
			と同じ)。gang header の verifier がこれを使う
			(2026-09-26 訂正: 前は「word 7 が physical birth」
			と書いていた。[Z] spa.h の blkptr_t で数え直した)
	dedup / encryption
			この版に無い
