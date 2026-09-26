/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * The ZFS on-disk format, as far as reading it needs.
 *
 * Written from:
 *
 *   [S]  ZFS On-Disk Specification, Draft, Sun Microsystems, 2006.
 *        Section numbers below are that document's.
 *
 * That document stops at pool version 1, and the pools this has to read
 * are version 5000.  Where it is silent, the second source is named on
 * the spot, so that every constant here can be traced to something:
 *
 *   [Z]  OpenZFS include/sys/, which is the format's only current
 *        written description.
 *
 * Nothing is taken from another implementation's reader.
 */

#ifndef _LIBSA_ZFS_ONDISK_H_
#define	_LIBSA_ZFS_ONDISK_H_

/*
 * A standalone build has no <stddef.h>: libsa brings NULL in through
 * stand.h, which the reader does not include so that the same files can
 * be compiled and tested on a host.  Both are spelled the compiler's own
 * way rather than depending on which header arrived first.
 */
#ifndef NULL
#define	NULL		((void *)0)
#endif
#ifndef offsetof
#define	offsetof(t, m)	__builtin_offsetof(t, m)
#endif

/*
 * [S] defines these in chapter two, but chapter one's uberblock already
 * contains a blkptr, so they come first here.  The commentary on how the
 * fields are packed stays with chapter two, where the accessors are.
 */
#define	SPA_DVAS_PER_BP		3		/* [S] §2.1 */

typedef struct {
	uint64_t	dva_word[2];
} dva_t;

typedef struct {
	uint64_t	zc_word[4];		/* [S] §2.4 */
} zio_cksum_t;

typedef struct {
	dva_t		blk_dva[SPA_DVAS_PER_BP];	/* words 0-5 */
	uint64_t	blk_prop;			/* word 6 */
	uint64_t	blk_pad[3];			/* [S] §2.12 */
	uint64_t	blk_birth;			/* word a, §2.11 */
	uint64_t	blk_fill;			/* word b, §2.10 */
	zio_cksum_t	blk_cksum;			/* words c-f */
} blkptr_t;

/* ------------------------------------------------------------------ */
/* Chapter One: vdev labels						*/
/* ------------------------------------------------------------------ */

/*
 * [S] §1.2: "Each physical vdev within a storage pool contains a 256KB
 * structure called a vdev label."
 */
#define	VDEV_LABEL_SIZE		(256 * 1024)

/*
 * [S] §1.3, Illustration 3: the label is 8KB of blank space, 8KB of boot
 * header, 112KB of name-value pairs and 128KB of uberblocks.
 */
#define	VDEV_LABEL_BLANK_SIZE	(8 * 1024)		/* [S] §1.3.1 */
#define	VDEV_LABEL_BOOT_SIZE	(8 * 1024)		/* [S] §1.3.2 */
#define	VDEV_LABEL_NVLIST_SIZE	(112 * 1024)		/* [S] §1.3.3 */
#define	VDEV_LABEL_UBERBLOCK_SIZE (128 * 1024)		/* [S] §1.3.4 */

#define	VDEV_LABEL_NVLIST_OFF	(VDEV_LABEL_BLANK_SIZE + \
				 VDEV_LABEL_BOOT_SIZE)
#define	VDEV_LABEL_UBERBLOCK_OFF (VDEV_LABEL_NVLIST_OFF + \
				 VDEV_LABEL_NVLIST_SIZE)

/*
 * [S] §1.2.1, Illustration 2: on a device of size N the four copies sit
 * at 0, 256K, N-512K and N-256K.  Two at each end, because corruption is
 * assumed to come in contiguous chunks.
 */
#define	VDEV_LABELS		4

/*
 * [S] §1.3.4 says the uberblock array holds "1K sized uberblock
 * structures", which was true when the smallest allocation was a 512
 * byte sector.
 *
 * [Z] vdev_impl.h: the spacing is now 1 << MAX(ashift, 10), so a pool
 * whose ashift is 12 spaces its uberblocks 4K apart and holds 32 of
 * them rather than 128.  Reading the array at a fixed 1K stride finds
 * garbage on such a pool, so the stride is computed, not assumed.
 */
#define	VDEV_UBERBLOCK_SHIFT_MIN 10

/*
 * [S] §1.3.4, Table 3: the magic reads 0x00bab10c on a big endian
 * machine and 0x0cb1ba00 on a little endian one.  The uberblock is
 * stored in the machine's native endian, so which of the two is seen
 * says which way round the pool was written.
 */
#define	UBERBLOCK_MAGIC		0x00bab10cULL

/* [S] §1.3.4 */
struct uberblock {
	uint64_t	ub_magic;
	uint64_t	ub_version;
	uint64_t	ub_txg;
	uint64_t	ub_guid_sum;
	uint64_t	ub_timestamp;
	blkptr_t	ub_rootbp;
};

/*
 * [S] §1.3.3, Table 1.
 */
#define	POOL_STATE_ACTIVE	0
#define	POOL_STATE_EXPORTED	1
#define	POOL_STATE_DESTROYED	2

/*
 * [S] §1.3.3: the names in the label's nvlist.
 */
#define	ZPOOL_CONFIG_VERSION		"version"
#define	ZPOOL_CONFIG_POOL_NAME		"name"
#define	ZPOOL_CONFIG_POOL_STATE		"state"
#define	ZPOOL_CONFIG_POOL_TXG		"txg"
#define	ZPOOL_CONFIG_POOL_GUID		"pool_guid"
#define	ZPOOL_CONFIG_TOP_GUID		"top_guid"
#define	ZPOOL_CONFIG_GUID		"guid"
#define	ZPOOL_CONFIG_VDEV_TREE		"vdev_tree"

/* [S] §1.3.3: inside vdev_tree. */
#define	ZPOOL_CONFIG_TYPE		"type"
#define	ZPOOL_CONFIG_ID			"id"
#define	ZPOOL_CONFIG_PATH		"path"
#define	ZPOOL_CONFIG_ASHIFT		"ashift"
#define	ZPOOL_CONFIG_ASIZE		"asize"
#define	ZPOOL_CONFIG_CHILDREN		"children"
/*
 * [Z] fs/zfs.h: the number of top-level vdevs, which [S]'s label has
 * no way to say -- each label carries only its own top-level vdev's
 * tree -- and a raidz's parity.
 */
#define	ZPOOL_CONFIG_VDEV_CHILDREN	"vdev_children"
#define	ZPOOL_CONFIG_NPARITY		"nparity"

/* [S] §1.3.3, Table 2. */
#define	VDEV_TYPE_DISK		"disk"
#define	VDEV_TYPE_FILE		"file"
#define	VDEV_TYPE_MIRROR	"mirror"
#define	VDEV_TYPE_RAIDZ		"raidz"
#define	VDEV_TYPE_REPLACING	"replacing"
#define	VDEV_TYPE_ROOT		"root"
#define	VDEV_TYPE_SPARE		"spare"		/* [Z] */

/* ------------------------------------------------------------------ */
/* Chapter Two: block pointers						*/
/* ------------------------------------------------------------------ */

/*
 * [S] Chapter Two: "A block pointer (blkptr_t) is a 128 byte ZFS
 * structure", laid out in Illustration 8 as sixteen 64 bit words.
 *
 * The words are kept whole here and taken apart by the accessors below,
 * because the fields do not line up with anything a C bitfield could
 * describe portably.
 */

/*
 * [S] §2.1: the DVA's two words hold
 *
 *	word 0	vdev (32) | GRID (8) | ASIZE (24)
 *	word 1	G (1) | offset (63)
 *
 * and "The value stored in offset is the offset in terms of sectors
 * (512 byte blocks)", so
 *
 *	physical block address = (offset << 9) + 0x400000 (4MB)
 *
 * where 0x400000 is the two front labels and the boot block.
 */
#define	SPA_MINBLOCKSHIFT	9
/*
 * [S] §2.1 states the sum itself, and it is taken from there rather
 * than added up: the two front labels are 512K, and the rest of the 4MB
 * is a boot block that [S] never gives a size to.  It is not §1.3.2's
 * 8K "boot block header", which sits inside each label and is already
 * counted in the 256K.
 *
 * Adding 2 * 256K + 8K instead reads 3.5MB short of every block on the
 * disk, which is a hole of zeros on a fresh pool and somebody else's
 * data on a full one.
 */
#define	VDEV_LABEL_START_SIZE	0x400000

#define	DVA_GET_VDEV(dva)	((uint32_t)((dva)->dva_word[0] >> 32))
#define	DVA_GET_GRID(dva)	((uint8_t)(((dva)->dva_word[0] >> 24) & 0xff))
#define	DVA_GET_ASIZE(dva)	(((dva)->dva_word[0] & 0xffffffULL) << \
				 SPA_MINBLOCKSHIFT)
#define	DVA_GET_GANG(dva)	((int)((dva)->dva_word[1] >> 63))	/* §2.3 */
#define	DVA_GET_OFFSET(dva)	(((dva)->dva_word[1] & \
				 ((1ULL << 63) - 1)) << SPA_MINBLOCKSHIFT)

/*
 * [S] §2.6: "All sizes are stored as the number of 512 byte sectors
 * (minus one)", so the stored value is one less than the count.
 */
#define	BP_GET_LSIZE(bp)	((((bp)->blk_prop & 0xffffULL) + 1) << \
				 SPA_MINBLOCKSHIFT)
#define	BP_GET_PSIZE(bp)	(((((bp)->blk_prop >> 16) & 0xffffULL) + 1) \
				 << SPA_MINBLOCKSHIFT)

/*
 * [S] Illustration 8, word 6: comp (8) above PSIZE, cksum (8) above
 * that, type (8) above that, then lvl, then E at bit 63.
 *
 * [Z] spa.h narrowed comp to 7 bits and took bit 39 for "embedded
 * data"; the spec has no such bit.  The mask below is therefore 0x7f,
 * not 0xff, and BP_IS_EMBEDDED names the bit the spec does not have.
 */
#define	BP_GET_COMPRESS(bp)	((uint8_t)(((bp)->blk_prop >> 32) & 0x7f))
#define	BP_IS_EMBEDDED(bp)	((int)(((bp)->blk_prop >> 39) & 0x1))
#define	BP_GET_CHECKSUM(bp)	((uint8_t)(((bp)->blk_prop >> 40) & 0xff))
#define	BP_GET_TYPE(bp)		((uint8_t)(((bp)->blk_prop >> 48) & 0xff))
#define	BP_GET_LEVEL(bp)	((uint8_t)(((bp)->blk_prop >> 56) & 0x1f))
#define	BP_GET_BYTEORDER(bp)	((int)(((bp)->blk_prop >> 63) & 0x1))	/* §2.7 */

/*
 * [S] §2.12 reserves words 7, 8 and 9.  [Z] spa.h has since given word 7
 * to blk_prop2 and word 9 to the physical birth txg, the txg the block
 * was actually written in, which differs from word a's logical birth
 * only for blocks that were rewritten, deduplicated or cloned.  Zero
 * means "the same as the logical birth".  A gang header's checksum is
 * keyed on it; see gang_header_valid().
 */
#define	BP_GET_PHYSICAL_BIRTH(bp)	\
	((bp)->blk_pad[2] != 0 ? (bp)->blk_pad[2] : (bp)->blk_birth)

/* [S] §2.7, Table 7. */
#define	ZFS_HOST_BYTEORDER_BIG		0
#define	ZFS_HOST_BYTEORDER_LITTLE	1


/*
 * Embedded block pointers.
 *
 * [S] has no such thing: in its version a block pointer always points
 * somewhere.  [Z] spa.h lets a pointer carry up to 112 bytes of payload
 * in place of its three DVAs, for data small enough that a whole sector
 * would be waste, and marks it with bit 39 of blk_prop.  A pool NetBSD
 * writes uses it for small ZAPs -- a dataset's child map, among others --
 * so a reader that skips it cannot reach a filesystem.
 *
 * The payload lies in every word except word 6, which holds the fields
 * below, and word 0xa, which stays the birth txg: words 0-5, 7-9 and
 * 0xb-0xf, fourteen in all, in that order.
 *
 * LSIZE and PSIZE here are in bytes, not in sectors as [S] §2.6 has
 * them, and both are stored one less than the true value.  There is no
 * checksum: there is no block to check.
 */
#define	BPE_GET_LSIZE(bp)	((size_t)(((bp)->blk_prop & 0x1ffffffULL) + 1))
#define	BPE_GET_PSIZE(bp)	\
	((size_t)((((bp)->blk_prop >> 25) & 0x7fULL) + 1))
#define	BPE_GET_ETYPE(bp)	((uint8_t)(((bp)->blk_prop >> 40) & 0xff))

#define	BP_EMBEDDED_TYPE_DATA	0	/* [Z] spa.h */

#define	BPE_NUM_WORDS		14
#define	BPE_PAYLOAD_SIZE	(BPE_NUM_WORDS * 8)

/* [S] §2.4, Table 5. */
#define	ZIO_CHECKSUM_INHERIT	0
#define	ZIO_CHECKSUM_ON		1	/* fletcher2 */
#define	ZIO_CHECKSUM_OFF	2	/* none */
#define	ZIO_CHECKSUM_LABEL	3	/* SHA-256 */
#define	ZIO_CHECKSUM_GANG_HEADER 4	/* SHA-256 */
#define	ZIO_CHECKSUM_ZILOG	5	/* fletcher2 */
#define	ZIO_CHECKSUM_FLETCHER_2	6
#define	ZIO_CHECKSUM_FLETCHER_4	7
#define	ZIO_CHECKSUM_SHA256	8

/*
 * [Z] zio.h continues the table past what [S] §2.4 records.  Only the
 * ones this can verify are named; the rest are refused by number.
 */
#define	ZIO_CHECKSUM_NOPARITY	10
#define	ZIO_CHECKSUM_SHA512	11	/* SHA-512/256; see sha512.c */
#define	ZIO_CHECKSUM_SKEIN	12	/* salted; see skein.c */
#define	ZIO_CHECKSUM_EDONR	13	/* salted; see edonr.c */
#define	ZIO_CHECKSUM_BLAKE3	14	/* salted; see blake3.c */

/* [S] §2.5, Table 6. */
#define	ZIO_COMPRESS_INHERIT	0
#define	ZIO_COMPRESS_ON		1	/* lzjb */
#define	ZIO_COMPRESS_OFF	2	/* none */
#define	ZIO_COMPRESS_LZJB	3

/*
 * [Z] zio_compress.h continues the table.  A pool made by NetBSD 11
 * compresses with lz4 by default, so reading one needs at least this
 * much beyond [S].
 */
#define	ZIO_COMPRESS_EMPTY	4
#define	ZIO_COMPRESS_GZIP_1	5
#define	ZIO_COMPRESS_GZIP_9	13
#define	ZIO_COMPRESS_ZLE	14
#define	ZIO_COMPRESS_LZ4	15
#define	ZIO_COMPRESS_ZSTD	16	/* [Z] zio_compress.h */

/* [S] §2.3: the gang block's tail. */
#define	ZBT_MAGIC		0x210da7ab10c7a11ULL
#define	SPA_GANGBLOCKSIZE	512

/*
 * [S] §2.3: "Gang blocks are 512 byte sized, self checksumming blocks",
 * and what makes them self checksumming is a zio_block_tail_t at the end
 * of the block holding a magic and a checksum.
 *
 *	zbt_magic = 0x210da7ab10c7a11	("zio-data-bloc-tail")
 *
 * [S] gives the structure only for the gang header, but the same tail
 * ends every block whose checksum cannot live in a block pointer,
 * because nothing points at it yet.  The uberblock slots of a label are
 * the case this reader meets first.
 *
 * [Z] zio.h calls the structure zio_eck_t and the magic ZEC_MAGIC.  The
 * value is unchanged, so this keeps [S]'s.
 */
#define	ZEC_MAGIC		0x210da7ab10c7a11ULL	/* [S] §2.3 */

struct zio_eck {
	uint64_t	zec_magic;
	uint64_t	zec_cksum[4];
};

/*
 * [S] §2.3: "Gang blocks are 512 byte sized, self checksumming blocks.
 * A gang block contains up to 3 block pointers followed by a 32 byte
 * checksum."  Three pointers are 384 bytes and the tail is 40, so 88
 * bytes of filler sit between them.
 *
 * [Z] zio.h no longer fixes the size.  With the dynamic_gang_header
 * feature active a header is as large as the smallest allocation of
 * the top-level vdev, 1 << ashift, and holds as many pointers as fit
 * ahead of the tail; the layout is otherwise [S]'s.  The feature only
 * governs headers written after it was enabled, so a pool can hold
 * both kinds, and [Z] zio_checksum.c tells them apart by trying the
 * large size and then [S]'s.  This reader does the same, which spares
 * it reading the pool's feature list.
 */
#define	SPA_GANGBLOCKSIZE	512			/* [S] §2.3 */
#define	GBH_NBLKPTRS(size)	\
	(((size) - sizeof(struct zio_eck)) / sizeof(blkptr_t))

/*
 * How deep a gang block may nest: a gang member may itself be a gang
 * block.  [S] and [Z] set no limit -- each level only has to be smaller
 * than the one above -- so this is the reader's, sized so that the
 * header buffers for every level fit in the scratch arena.  A pool
 * nested deeper is refused; see scratch.h for the arithmetic.
 */
#define	ZFS_GANG_MAXDEPTH	4

/*
 * Object types.
 *
 * Table 8 (2.8) and Table 10 (3.1) both number these, and they
 * disagree: Table 8 calls 12 DMU_OT_DSL_DATASET and 16
 * DMU_OT_DSL_OBJSET, while Table 10 calls 12 DMU_OT_DSL_DIR and 16
 * DMU_OT_DSL_DATASET.  The two cannot both be right.
 *
 * [Z] dmu.h settles it in Table 10's favour: counted from zero, its
 * dmu_object_type_t gives 12 DSL_DIR and 16 DSL_DATASET.  The numbering
 * agrees with Table 8 everywhere else, and the names that differ were
 * changed later without moving anything (5 and 6 BPLIST to BPOBJ, 22
 * DELETE_QUEUE to UNLINKED_SET).
 *
 * Only the types this reader acts on are named; the rest stay numbers.
 */
#define	DMU_OT_NONE			0
#define	DMU_OT_OBJECT_DIRECTORY		1	/* a ZAP */
#define	DMU_OT_OBJECT_ARRAY		2
#define	DMU_OT_PACKED_NVLIST		3
#define	DMU_OT_PACKED_NVLIST_SIZE	4
#define	DMU_OT_SPACE_MAP_HEADER		7
#define	DMU_OT_SPACE_MAP		8
#define	DMU_OT_INTENT_LOG		9
#define	DMU_OT_DNODE			10
#define	DMU_OT_OBJSET			11
#define	DMU_OT_DSL_DIR			12	/* Table 10, not Table 8 */
#define	DMU_OT_DSL_DIR_CHILD_MAP	13
#define	DMU_OT_DSL_DS_SNAP_MAP		14
#define	DMU_OT_DSL_PROPS		15
#define	DMU_OT_DSL_DATASET		16	/* Table 10, not Table 8 */
#define	DMU_OT_ZNODE			17
#define	DMU_OT_PLAIN_FILE_CONTENTS	19
#define	DMU_OT_DIRECTORY_CONTENTS	20
#define	DMU_OT_MASTER_NODE		21

/* 3.2, Table 11: the kinds of object set. */
#define	DMU_OST_NONE			0
#define	DMU_OST_META			1	/* DSL, chapter 4 */
#define	DMU_OST_ZFS			2	/* ZPL, chapter 6 */
#define	DMU_OST_ZVOL			3	/* chapter 8 */

/*
 * 3.1: "Objects are defined by 512 bytes structures called dnodes."
 *
 * [Z] dnode.h lets a dnode span several of those slots (dn_extra_slots,
 * the large_dnode feature).  A pool that uses it is refused rather than
 * misread, because the specification describes no such thing.
 */
#define	DNODE_SIZE		512
#define	DNODE_SHIFT		9

/*
 * [S] §3.1: "ZFS supports variable data and indirect block sizes ranging
 * from 512 bytes to 128 Kbytes", and "ZFS provides up to six levels of
 * indirection".  Both are limits on fields read off the disk that are
 * then used as shift counts and loop bounds, so they are named here and
 * checked before use rather than trusted.
 */
#define	SPA_MAXBLOCKSHIFT	17
#define	DN_MAX_LEVELS		6

/* [S] §3.1: the bonus buffer is "between 64 and 320 bytes". */
#define	DN_MAX_BONUSLEN		320

/* 3.1, Illustration 9. */
typedef struct {
	uint8_t		dn_type;
	uint8_t		dn_indblkshift;		/* log2 of the indirect size */
	uint8_t		dn_nlevels;
	uint8_t		dn_nblkptr;		/* between one and three */
	uint8_t		dn_bonustype;
	uint8_t		dn_checksum;
	uint8_t		dn_compress;
	uint8_t		dn_pad[1];
	uint16_t	dn_datablkszsec;	/* data size / 512, 1..256 */
	uint16_t	dn_bonuslen;
	uint8_t		dn_pad2[4];
	uint64_t	dn_maxblkid;		/* largest level 0 blkid */
	uint64_t	dn_secphys;
	uint64_t	dn_pad3[4];
	/*
	 * [S] §3.1: "dn_blkptr is a variable length field that can
	 * contain between one and three block pointers", and the bonus
	 * buffer begins where they end.  Three are declared so that the
	 * structure can be indexed, but only dn_nblkptr of them are
	 * there, and the bonus buffer is found with DN_BONUS rather than
	 * by taking the address of dn_blkptr[3].
	 */
	blkptr_t	dn_blkptr[3];
	/*
	 * [S] §3.1 fixes the whole dnode at 512 bytes, and the bonus
	 * buffer is whatever is left after the block pointers.  The
	 * padding is here so that sizeof matches the disk: a copy of a
	 * dnode then contains every byte the dnode has, and an offset
	 * computed from a field inside it -- a bonus buffer offset, say
	 * -- cannot leave the copy even when the field is nonsense.
	 */
	uint8_t		dn_bonus[DNODE_SIZE - 64 - 3 * 128];
} dnode_phys_t;

#define	DN_BONUS(dn)	((void *)((uint8_t *)(dn) + \
			    offsetof(dnode_phys_t, dn_blkptr) + \
			    (dn)->dn_nblkptr * sizeof(blkptr_t)))

/*
 * 3.1: a blkptr is 128 bytes, so an indirect block holds
 * (1 << dn_indblkshift) / 128 of them; the largest, 128KB, holds 1024.
 */
#define	SPA_BLKPTRSHIFT		7

/*
 * 3.1 gives the way up the tree with a worked example, and the
 * example's formula is wrong:
 *
 *	"Given a level 0 block id of 16360 ... block 15 of level 1
 *	 contains the block pointer for level 0 blkid 16360.
 *	  level 1 blkid = 16360%1024 = 15"
 *
 * 16360 % 1024 is 1000.  It is the division that gives 15, and division
 * is what finding a parent means, so that is what is done here.
 */

/*
 * [S] §3.2, Illustration 11 and 12: an objset_phys_t is 1K, holding the
 * metadnode, a zil_header_t, os_type, and 376 bytes of padding.
 *
 * Those numbers do not add up, in this version or now.  Illustration 12's
 * 376 bytes of padding leaves 648 for what comes before it, but §7.1's
 * zil_header_t is a blkptr and two words, which with the 512 byte
 * metadnode and os_type makes 664.
 *
 * [Z] zil.h has since grown the header to 192 bytes (four more words and
 * three of padding), and dmu_objset.h has grown the object set itself to
 * 2K and then 4K for the accounting dnodes.  So os_type is neither at
 * 648 nor at 664 but at 704, and it is the only field past the metadnode
 * this reader wants.
 *
 * The offset is therefore taken from the structure, not counted by hand.
 */
#define	ZIL_HEADER_SIZE		192		/* [Z] zil.h */

typedef struct {
	uint64_t	zh_claim_txg;
	uint64_t	zh_replay_seq;
	blkptr_t	zh_log;
	uint64_t	zh_claim_blk_seq;	/* [Z] */
	uint64_t	zh_flags;		/* [Z] */
	uint64_t	zh_claim_lr_seq;	/* [Z] */
	uint64_t	zh_pad[3];		/* [Z] */
} zil_header_t;

typedef struct {
	/*
	 * [S] §3.1 fixes a dnode at 512 bytes, but dnode_phys_t above
	 * stops after one block pointer, because the count is in the
	 * dnode.  Everything that follows a dnode in a larger structure
	 * is therefore placed by the 512, not by sizeof: taking the C
	 * structure's length here puts os_type 320 bytes early, on a
	 * word that happens to be zero.
	 */
	union {
		dnode_phys_t	osd_dnode;
		uint8_t		osd_space[DNODE_SIZE];
	} os_meta;
	zil_header_t	os_zil_header;
	uint64_t	os_type;		/* DMU_OST_*, [S] Table 11 */
	/* [Z] os_flags and the accounting dnodes follow; not read here */
} objset_phys_t;

#define	os_meta_dnode	os_meta.osd_dnode

/* ------------------------------------------------------------------ */
/* Chapter Four: the DSL						*/
/* ------------------------------------------------------------------ */

/*
 * [S] §4.2: "The DSL is implemented as an object set of type
 * DMU_OST_META ... often called the Meta Object Set, or MOS.  There is
 * only one MOS per pool and the uberblock points to it directly."
 *
 * "There is a single distinguished object in the Meta Object Set ...
 * called the object directory and is always located in the second
 * element of the dnode array (index 1)."
 *
 * [S] §6.1 gives the ZPL object set the same rule: its master node is
 * also object 1.  So one constant serves both.
 */
#define	MASTER_NODE_OBJ		1

/* [S] §4.2: the three attributes of the object directory. */
#define	DMU_POOL_ROOT_DATASET	"root_dataset"
#define	DMU_POOL_CONFIG		"config"
#define	DMU_POOL_SYNC_BPLIST	"sync_bplist"

/*
 * [S] §4.2 lists only those three.  [Z] dmu.h has many more; this is the
 * one a loader needs, a ZAP of the pool's own properties, in which
 * spa.c keeps bootfs as the object number of a DSL dataset.
 */
#define	DMU_POOL_PROPS		"pool_props"	/* [Z] dmu.h */
#define	DMU_POOL_CHECKSUM_SALT	"org.illumos:checksum_salt" /* [Z] dmu.h */
#define	ZPOOL_PROP_BOOTFS	"bootfs"	/* [Z] spa.c */

/*
 * [S] §4.4: the DSL directory's bonus buffer.
 *
 * [Z] dsl_dir.h has added dd_deleg_zapobj, dd_flags, dd_used_breakdown[]
 * and dd_clones after dd_props_zapobj, and renamed dd_clone_parent_obj
 * to dd_origin_obj.  Everything this reader wants is before that, at the
 * offsets [S] gives.
 */
typedef struct {
	uint64_t	dd_creation_time;
	uint64_t	dd_head_dataset_obj;	/* the active dataset */
	uint64_t	dd_parent_obj;
	uint64_t	dd_clone_parent_obj;	/* [Z] dd_origin_obj */
	uint64_t	dd_child_dir_zapobj;	/* name -> child directory */
	uint64_t	dd_used_bytes;
	uint64_t	dd_compressed_bytes;
	uint64_t	dd_uncompressed_bytes;
	uint64_t	dd_quota;
	uint64_t	dd_reserved;
	uint64_t	dd_props_zapobj;
} dsl_dir_phys_t;

/*
 * [S] §4.3: the dataset's bonus buffer.  ds_bp is what this reader is
 * after: it points at the object set the dataset represents.
 *
 * [Z] dsl_dataset.h renamed ds_restoring to ds_flags and ds_used_bytes
 * to ds_referenced_bytes, and added three object numbers and padding
 * after ds_bp.  Nothing was inserted before it, so ds_bp is still the
 * seventeenth field.
 */
typedef struct {
	uint64_t	ds_dir_obj;
	uint64_t	ds_prev_snap_obj;
	uint64_t	ds_prev_snap_txg;
	uint64_t	ds_next_snap_obj;
	uint64_t	ds_snapnames_zapobj;
	uint64_t	ds_num_children;
	uint64_t	ds_creation_time;
	uint64_t	ds_creation_txg;
	uint64_t	ds_deadlist_obj;
	uint64_t	ds_used_bytes;
	uint64_t	ds_compressed_bytes;
	uint64_t	ds_uncompressed_bytes;
	uint64_t	ds_unique_bytes;
	uint64_t	ds_fsid_guid;
	uint64_t	ds_guid;
	uint64_t	ds_restoring;		/* [Z] ds_flags */
	blkptr_t	ds_bp;
} dsl_dataset_phys_t;

/* ------------------------------------------------------------------ */
/* Chapter Five: the ZAP						*/
/* ------------------------------------------------------------------ */

/*
 * [S] chapter 5: "The first 64 bit word in each block of a ZAP object is
 * used to identify the type of ZAP contents contained within this
 * block."  Table 14.
 */
/*
 * [S] chapter 5: "The name portion of the attribute is a zero-terminated
 * string of up to 256 bytes (including terminating NULL)."
 */
#define	ZAP_MAXNAMELEN		256

#define	ZBT_MICRO		((1ULL << 63) + 3)
#define	ZBT_HEADER		((1ULL << 63) + 1)
#define	ZBT_LEAF		((1ULL << 63) + 0)

/*
 * [S] §5.1: a microzap is one block.  Its first 128 bytes are the
 * header, of which the last 64 hold the first entry; entries follow at
 * 64 bytes each.
 *
 * The header's own account of itself does not close: 8 bytes of block
 * type, 8 of salt and "the next 42 bytes ... left blank" reach only 58,
 * where the first entry starts at 64.  The blank run is 48, so the entry
 * array is addressed from offset 64 and the padding is not counted on.
 */
#define	MZAP_ENT_LEN		64
#define	MZAP_NAME_LEN		(MZAP_ENT_LEN - 8 - 4 - 2)	/* 50 */
#define	MZAP_HDR_LEN		128

typedef struct {
	uint64_t	mze_value;
	uint32_t	mze_cd;			/* collision differentiator */
	uint16_t	mze_pad;
	char		mze_name[MZAP_NAME_LEN];
} mzap_ent_phys_t;

typedef struct {
	uint64_t	mz_block_type;		/* ZBT_MICRO */
	uint64_t	mz_salt;
	uint64_t	mz_pad[6];
	mzap_ent_phys_t	mz_chunk[1];	/* the block's length says how many */
} mzap_phys_t;

/* [S] §5.2.1: the first block of a fatzap. */
#define	ZAP_MAGIC		0x2F52AB2ABULL

typedef struct {
	uint64_t	zt_blk;			/* blkid, if the table is external */
	uint64_t	zt_numblks;		/* zero if it is not */
	uint64_t	zt_shift;		/* hash bits used to index it */
	uint64_t	zt_nextblk;
	uint64_t	zt_blks_copied;
} zap_table_phys_t;

typedef struct {
	uint64_t		zap_block_type;	/* ZBT_HEADER */
	uint64_t		zap_magic;
	zap_table_phys_t	zap_ptrtbl;
	uint64_t		zap_freeblk;
	uint64_t		zap_num_leafs;
	uint64_t		zap_num_entries;
	uint64_t		zap_salt;
	/*
	 * [S] §5.2.1 ends the structure with zap_pad[8181] and
	 * zap_leafs[8192], which describes a 128K block: the embedded
	 * pointer table is the block's second half, and zt_shift is 13
	 * because 8192 entries is what fits there.
	 *
	 * A smaller block has a smaller table, so the two arrays are not
	 * declared.  The table is read at half the block length, and its
	 * size comes from zt_shift.
	 */
} zap_phys_t;

/*
 * [S] §5.2.3: the leaf.  Its header is "2 24-byte chunks", then the
 * hash table, then the chunks themselves.
 *
 * [S] spells the header's fields lhr_ in the structure and lh_ in the
 * prose; they are the same fields.
 */
#define	ZAP_LEAF_MAGIC		0x2AB1EAF

typedef struct {
	uint64_t	lh_block_type;		/* ZBT_LEAF */
	uint64_t	lh_next;		/* blkid of the next leaf in the chain */
	uint64_t	lh_prefix;
	uint32_t	lh_magic;
	uint16_t	lh_nfree;
	uint16_t	lh_nentries;
	uint16_t	lh_prefix_len;
	uint16_t	lh_freelist;
	uint8_t		lh_pad2[12];
} zap_leaf_header_t;

/*
 * [S] §5.2.4 gives the three chunk types and their sizes, but not how
 * many of each a leaf holds, nor how large the hash table is: §5.2.3's
 * "the next 8KB" and "twelve bits" are again the 128K block.
 *
 * [Z] zap_leaf.h derives all three from the block shift bs:
 *
 *	ZAP_LEAF_CHUNKSIZE		24
 *	ZAP_LEAF_ARRAY_BYTES		24 - 3 = 21
 *	ZAP_LEAF_HASH_NUMENTRIES	1 << (bs - 5)
 *	ZAP_LEAF_NUMCHUNKS		((1 << bs) - 2 * NUMENTRIES) / 24 - 2
 *
 * The final - 2 is the header, which [S] already describes as two
 * chunks, and bs = 17 gives 4096 hash entries in 8KB, which is [S]'s
 * figure.  The two accounts agree; only [Z]'s is general.
 */
#define	ZAP_LEAF_CHUNKSIZE	24
#define	ZAP_LEAF_ARRAY_BYTES	(ZAP_LEAF_CHUNKSIZE - 3)
#define	ZAP_LEAF_HASH_SHIFT(bs)	((bs) - 5)
#define	ZAP_LEAF_HASH_NUMENTRIES(bs) (1U << ZAP_LEAF_HASH_SHIFT(bs))
#define	ZAP_LEAF_NUMCHUNKS(bs)	\
	(((1U << (bs)) - 2 * ZAP_LEAF_HASH_NUMENTRIES(bs)) / \
	 ZAP_LEAF_CHUNKSIZE - 2)

#define	ZAP_LEAF_ENTRY		252	/* [S] §5.2.4 */
#define	ZAP_LEAF_ARRAY		251
#define	ZAP_LEAF_FREE		253
#define	ZAP_LEAF_CHAIN_END	0xffff

/*
 * [S] §5.2.4.  Illustration 18 draws le_cd as a uint32_t where the
 * structure declares a uint16_t and two bytes of padding; the structure
 * is what fits the 24 byte chunk, so it is what is used.
 */
typedef struct {
	uint8_t		le_type;	/* ZAP_LEAF_ENTRY */
	uint8_t		le_int_size;	/* bytes per value element */
	uint16_t	le_next;
	uint16_t	le_name_chunk;
	uint16_t	le_name_length;	/* including the NUL */
	uint16_t	le_value_chunk;
	uint16_t	le_value_length;/* in le_int_size units */
	uint16_t	le_cd;
	uint8_t		le_pad[2];
	uint64_t	le_hash;
} zap_leaf_entry_t;

/*
 * [S] §5.2.4: "Values of type 'integer' are always stored in big
 * endian format, regardless of the machine's native endianness."
 *
 * This is the one place in the format where that is true; blkptrs,
 * dnodes and uberblocks are all written in the writing machine's endian.
 */
typedef struct {
	uint8_t		la_type;	/* ZAP_LEAF_ARRAY */
	uint8_t		la_array[ZAP_LEAF_ARRAY_BYTES];
	uint16_t	la_next;
} zap_leaf_array_t;

/*
 * The hash.
 *
 * [S] says only that entries are "arranged based on a 64 bit hash of the
 * attribute's name" with the ZAP's salt "stirred into" it.  It never
 * gives the function, so a fatzap cannot be read from [S] alone.
 *
 * [Z] zap_impl.c: starting from the salt, each byte of the name updates
 * a reflected CRC-64,
 *
 *	h = (h >> 8) ^ crc64[(h ^ c) & 0xff]
 *
 * over ECMA-182's polynomial in reflected form, and the low
 * 64 - hashbits bits are then cleared to leave room for the collision
 * differentiator.  hashbits is 28 unless the ZAP sets ZAP_FLAG_HASH64.
 *
 * Two details decide whether this works at all.  The terminating NUL is
 * *not* hashed, although it is stored.  And the table has to be built
 * before the first lookup: a table of zeros makes every name hash to the
 * salt, and every lookup returns ENOENT rather than failing loudly.
 *
 * zap_impl.c asserts crc64[128] == the polynomial, which is a property
 * of the reflected construction and so serves as a check on the table
 * built here.
 */
#define	ZFS_CRC64_POLY		0xC96C5795D7870F42ULL	/* [Z] dmu.h */
#define	ZAP_HASHBITS		28			/* [Z] zap_impl.c */

/* ------------------------------------------------------------------ */
/* Chapter Six: the ZPL							*/
/* ------------------------------------------------------------------ */

/*
 * [S] §6.1: the master node is object 1 of a DMU_OST_ZFS object set and
 * holds three attributes.
 */
#define	ZFS_ROOT_OBJ		"ROOT"
#define	ZPL_VERSION_STR		"VERSION"
#define	ZFS_DELETE_QUEUE	"DELETE_QUEUE"

/*
 * [S] §6.2: directories are ZAP objects whose values are object
 * numbers.  The prose calls the type DMU_OT_DIRECTORY; Tables 8 and 13
 * call it DMU_OT_DIRECTORY_CONTENTS.
 *
 * [S] §6.2's znode_phys_t is what a filesystem object keeps in its
 * bonus buffer.  Only two of its fields matter to a reader.
 */
typedef struct {
	uint64_t	zp_atime[2];		/* seconds, nanoseconds */
	uint64_t	zp_mtime[2];
	uint64_t	zp_ctime[2];
	uint64_t	zp_crtime[2];
	uint64_t	zp_gen;
	uint64_t	zp_mode;
	uint64_t	zp_size;
	uint64_t	zp_parent;
	uint64_t	zp_links;
	uint64_t	zp_xattr;
	uint64_t	zp_rdev;
	uint64_t	zp_flags;
	uint64_t	zp_uid;
	uint64_t	zp_gid;
	uint64_t	zp_pad[4];
	/* [S] §6.3's zfs_znode_acl_t follows; a reader does not need it */
} znode_phys_t;

/*
 * [S] §6.2 says of zp_mode that "The lower 8 bits ... contain the
 * access mode bits, for example 755", that "The 9th bit is the sticky
 * bit", and that "Bits 13-16 are used to designate the file type".
 *
 * Those three sentences cannot all hold: 755 is octal and needs nine
 * bits, which leaves the sticky bit nowhere to sit, and the type values
 * of Table 15 are POSIX's S_IFMT shifted right by twelve, not thirteen.
 * Table 15's values are the part that is checkable, so the shift is
 * twelve and the prose's bit numbering is not used.
 */
#define	ZFS_MODE_FMT_SHIFT	12
#define	ZFS_MODE_FMT(m)		(((m) >> ZFS_MODE_FMT_SHIFT) & 0xf)

#define	ZFS_IFIFO		0x1	/* [S] Table 15 */
#define	ZFS_IFCHR		0x2
#define	ZFS_IFDIR		0x4
#define	ZFS_IFBLK		0x6
#define	ZFS_IFREG		0x8
#define	ZFS_IFLNK		0xA
#define	ZFS_IFSOCK		0xC

/*
 * System attributes.
 *
 * [S] has no notion of these: in its version every filesystem object
 * carries the znode_phys_t above.  A pool written by anything current
 * does not.  From ZPL version 5 the bonus buffer instead holds an
 * sa_hdr_phys_t and a packed run of attributes whose order is named by a
 * layout number, and laying znode_phys_t over that reads nonsense
 * without reporting an error -- which is the reason this is here rather
 * than in the list of things left for later.
 *
 * [Z] sa_impl.h and zfs_sa.h.  The header is
 *
 *	uint32_t sa_magic;		0x2F505A
 *	uint16_t sa_layout_info;	bits 0-9 layout, 10-15 size / 8
 *	uint16_t sa_lengths[];		for the variable length attributes
 *
 * so the first word of the bonus buffer says which of the two shapes is
 * there, and the master node's VERSION says so too.
 *
 * The layout's meaning proper is a ZAP under the MOS ("LAYOUTS"), but
 * zfs_sa.h also fixes where the fields land in the layout everything
 * uses, counted from the end of the header.  This reader wants mode and
 * size; whether it trusts these offsets or reads LAYOUTS is decided when
 * the code is written, not here.
 */
#define	SA_MAGIC		0x2F505A		/* [Z] sa_impl.h */

typedef struct {
	uint32_t	sa_magic;
	uint16_t	sa_layout_info;
	uint16_t	sa_lengths[1];	/* one per variable length attribute */
} sa_hdr_phys_t;

#define	SA_HDR_LAYOUT(x)	((x) & 0x3ff)
#define	SA_HDR_SIZE(x)		((((x) >> 10) & 0x3f) * 8)

#define	SA_MODE_OFFSET		0		/* [Z] zfs_sa.h */
#define	SA_SIZE_OFFSET		8
#define	SA_GEN_OFFSET		16
#define	SA_UID_OFFSET		24
#define	SA_GID_OFFSET		32
#define	SA_PARENT_OFFSET	40
#define	SA_FLAGS_OFFSET		48

/* ------------------------------------------------------------------ */
/* Sizes the format fixes						*/
/* ------------------------------------------------------------------ */

/*
 * Where a source states a structure's size in bytes, it is checked here
 * rather than trusted, so that a field added or mistyped above fails the
 * build instead of reading the wrong offset off a disk.
 *
 * dnode_phys_t is not among them: [S] §3.1 gives it 512 bytes, but that
 * counts a block pointer array and a bonus buffer whose lengths the
 * dnode itself declares, so the C structure is deliberately shorter and
 * DNODE_SIZE is what the reader steps by.
 */
#ifndef CTASSERT
#define	CTASSERT(x)	_Static_assert((x), #x)
#endif

CTASSERT(sizeof(blkptr_t) == 128);		/* [S] chapter 2 */
CTASSERT(sizeof(struct uberblock) == 5 * 8 + 128);  /* [S] §1.3.4 */
CTASSERT(sizeof(mzap_ent_phys_t) == MZAP_ENT_LEN);  /* [S] §5.1 */
CTASSERT(MZAP_NAME_LEN == 50);			/* [S] chapter 5 */
CTASSERT(sizeof(zap_leaf_entry_t) == ZAP_LEAF_CHUNKSIZE);
CTASSERT(sizeof(zap_leaf_array_t) == ZAP_LEAF_CHUNKSIZE);
CTASSERT(sizeof(zap_leaf_header_t) == 2 * ZAP_LEAF_CHUNKSIZE); /* [S] §5.2.3 */
CTASSERT(sizeof(zil_header_t) == ZIL_HEADER_SIZE);  /* [Z] zil.h */

/*
 * The one offset worth pinning: os_type is what says whether an object
 * set is the MOS or a filesystem, and it is the field a miscounted
 * metadnode or zil header moves.  704 was read off a pool.
 */
CTASSERT(offsetof(objset_phys_t, os_type) == 704);

/*
 * [S] §3.1: the dnode is 512 bytes.  The C structure has to be that
 * size too, or a field read at an offset taken from the dnode itself
 * can land outside the copy while still being inside the dnode.
 */
CTASSERT(sizeof(dnode_phys_t) == DNODE_SIZE);

/*
 * [Z] zfs_sa.h calls the old bonus buffer 0x108 bytes; the difference
 * from this structure is [S] §6.3's 88 byte zfs_znode_acl_t, which is
 * not declared above.
 */
CTASSERT(sizeof(znode_phys_t) + 88 == 0x108);

/* [S] §5.2.3: bs = 17 is the block the chapter's figures describe. */
CTASSERT(ZAP_LEAF_HASH_NUMENTRIES(17) * 2 == 8 * 1024);

#endif	/* _LIBSA_ZFS_ONDISK_H_ */
