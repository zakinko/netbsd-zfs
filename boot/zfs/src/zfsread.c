/*	$NetBSD$	*/

/*
 * Finding the pool: vdev labels and the uberblock.
 *
 * [S] chapter one.  Nothing here is taken from another implementation's
 * reader; where [S] no longer describes what is on disk, the second
 * source is named on the spot, as in zfs_ondisk.h.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "zfs_ondisk.h"
#include "zfsread.h"
#include "sha256.h"
#include "fletcher.h"
#include "lz4.h"
#include "gzip.h"
#include "zle.h"
#include "nvlist.h"
#include "scratch.h"

/*
 * [S] §1.2.1, Illustration 2: two labels at the front of the device and
 * two at the back, on a device of size N at 0, 256K, N-512K and N-256K.
 */
static uint64_t
label_offset(int l, uint64_t vdev_size)
{

	if (l < 2)
		return ((uint64_t)l * VDEV_LABEL_SIZE);
	return (vdev_size - (uint64_t)(4 - l) * VDEV_LABEL_SIZE);
}

/*
 * The uberblock's checksum.
 *
 * [S] §1.3.4 says the active uberblock is "the uberblock with the
 * highest transaction group number and valid SHA-256 checksum", and
 * [S] §2.4 fixes SHA-256 for labels, but neither says where in the
 * uberblock's slot the checksum sits or what is hashed.
 *
 * [Z] zio.h and zio_checksum.c: a self-checksumming block ends with a
 * zio_eck_t -- [S] §2.3's zio_block_tail_t, same magic -- and the hash
 * is taken over the whole slot with the checksum field temporarily
 * holding a "verifier" instead.  For a label that verifier is the
 * block's byte offset on the vdev followed by three zeros.
 *
 * So the slot cannot simply be hashed as it lies: the field has to be
 * substituted first, which is why this copies the tail rather than
 * hashing in place.
 */
static int
uberblock_valid(const void *slot, size_t slotsize, uint64_t offset)
{
	uint8_t tmp[sizeof(struct zio_eck)];
	uint64_t want[4], got[4];
	const struct zio_eck *eck;
	struct zio_eck sub;
	uint8_t *p;
	size_t i;

	if (slotsize < sizeof(struct zio_eck))
		return (0);

	eck = (const struct zio_eck *)((const uint8_t *)slot + slotsize -
	    sizeof(struct zio_eck));

	/*
	 * [Z] zio_checksum.c reads the tail's magic to decide whether the
	 * block was written by a machine of the other endianness.  A
	 * bootloader that cannot byte swap should say so here rather than
	 * fail the checksum and call the pool corrupt.
	 */
	if (eck->zec_magic != ZEC_MAGIC)
		return (0);

	for (i = 0; i < 4; i++)
		want[i] = eck->zec_cksum[i];

	sub.zec_magic = ZEC_MAGIC;
	sub.zec_cksum[0] = offset;
	sub.zec_cksum[1] = 0;
	sub.zec_cksum[2] = 0;
	sub.zec_cksum[3] = 0;

	p = (uint8_t *)(uintptr_t)(const uint8_t *)eck;
	for (i = 0; i < sizeof(tmp); i++) {
		tmp[i] = p[i];
		p[i] = ((const uint8_t *)&sub)[i];
	}

	sha256(slot, slotsize, got);

	for (i = 0; i < sizeof(tmp); i++)
		p[i] = tmp[i];

	for (i = 0; i < 4; i++)
		if (got[i] != want[i])
			return (0);
	return (1);
}

/*
 * [S] §1.3.4: walk the uberblock array of one label and keep the
 * highest txg that checks out.
 *
 * [S] gives the array a fixed 1K stride, which held while 512 bytes was
 * the smallest allocation.  [Z] vdev_impl.h spaces the slots by
 * 1 << MAX(ashift, 10), so the stride is a parameter here.  The caller
 * learns the ashift from the label's nvlist; when it does not know it
 * yet it can pass the minimum, because a larger stride's slots are a
 * subset of the 1K ones and their magic still lands on a 1K boundary --
 * only the checksum then fails, which is how a wrong stride shows up.
 */
static int
label_scan(struct zfs_pool *pool, int l, uint32_t ashift,
    struct uberblock *best, uint64_t *best_off)
{
	uint8_t *slot;
	uint64_t base, off;
	size_t slotsize;
	int found = 0, err;

	/*
	 * ashift comes out of the label's name-value pairs, which is to
	 * say off the disk, and it is about to be a shift count.  A
	 * label saying 200 is undefined behaviour, and asking whether
	 * "1 << ashift" is too large is undefined in the asking.  So the
	 * value is checked, not the result.
	 */
	if (ashift < SPA_MINBLOCKSHIFT || ashift > SPA_MAXBLOCKSHIFT)
		return (0);
	slotsize = (size_t)1 <<
	    (ashift > VDEV_UBERBLOCK_SHIFT_MIN ? ashift :
	    VDEV_UBERBLOCK_SHIFT_MIN);
	if ((slot = zfs_scratch_get(slotsize)) == NULL)
		return (0);

	base = label_offset(l, pool->pool_size) + VDEV_LABEL_UBERBLOCK_OFF;

	for (off = 0; off + slotsize <= VDEV_LABEL_UBERBLOCK_SIZE;
	    off += slotsize) {
		const struct uberblock *ub = (const void *)slot;

		err = pool->pool_read(pool->pool_cookie, base + off, slot,
		    slotsize);
		if (err != 0)
			continue;

		if (ub->ub_magic != UBERBLOCK_MAGIC)
			continue;
		if (!uberblock_valid(slot, slotsize, base + off))
			continue;
		if (found && ub->ub_txg <= best->ub_txg)
			continue;

		*best = *ub;
		*best_off = base + off;
		found = 1;
	}
	zfs_scratch_put(slot, slotsize);
	return (found);
}

/*
 * [S] §1.2.2: the four labels are written in two stages, so at least one
 * is always intact.  All four are read for that reason, and the newest
 * valid uberblock across all of them wins.
 */
int
zfs_uberblock_find(struct zfs_pool *pool)
{
	struct uberblock ub, best;
	uint64_t off;
	int l, found = 0;

	for (l = 0; l < VDEV_LABELS; l++) {
		if (!label_scan(pool, l, pool->pool_ashift, &ub, &off))
			continue;
		if (found && ub.ub_txg <= best.ub_txg)
			continue;
		best = ub;
		pool->pool_label = l;
		found = 1;
	}
	if (!found)
		return (EINVAL);

	pool->pool_ub = best;
	return (0);
}

/*
 * Reading what a block pointer points at.
 *
 * [S] chapter two describes the pointer; what it does not describe is
 * how to check the block once it is read, because [S] §2.4 names the
 * checksum functions without defining them.  See fletcher.c.
 */

/*
 * [S] §2.1: "The value stored in offset is the offset in terms of
 * sectors (512 byte blocks)" counted "after the vdev labels (L0 and L1)
 * and boot block", and gives the sum:
 *
 *	physical block address = (offset << 9) + 0x400000
 *
 * DVA_GET_OFFSET has already shifted, so only the 4MB is added here.
 */
static uint64_t
dva_offset(const dva_t *dva)
{

	return (DVA_GET_OFFSET(dva) + VDEV_LABEL_START_SIZE);
}

static int
block_checksum_ok(const blkptr_t *bp, const void *buf, size_t psize)
{
	uint64_t got[4];
	int i;

	switch (BP_GET_CHECKSUM(bp)) {
	case ZIO_CHECKSUM_OFF:
		/*
		 * [S] §2.4: "If the cksum value is 2 (off), a checksum
		 * will not be computed and checksum[0..3] will be zero."
		 */
		return (1);
	case ZIO_CHECKSUM_ON:
	case ZIO_CHECKSUM_ZILOG:
	case ZIO_CHECKSUM_FLETCHER_2:
		fletcher2(buf, psize, got);
		break;
	case ZIO_CHECKSUM_FLETCHER_4:
		fletcher4(buf, psize, got);
		break;
	case ZIO_CHECKSUM_LABEL:
	case ZIO_CHECKSUM_GANG_HEADER:
	case ZIO_CHECKSUM_SHA256:
		sha256(buf, psize, got);
		break;
	default:
		/*
		 * [Z] zio_checksum.h has added sha512, skein, edonr and
		 * blake3 since [S].  A pool using one of them is refused
		 * rather than read unchecked: a bootloader that skips the
		 * checksum turns a bad disk into a kernel that crashes
		 * somewhere else.
		 */
		return (0);
	}

	for (i = 0; i < 4; i++)
		if (got[i] != bp->blk_cksum.zc_word[i])
			return (0);
	return (1);
}

static int
block_decompress(const blkptr_t *bp, const void *in, size_t psize,
    void *out, size_t lsize)
{
	const uint8_t *p = in;
	size_t i;

	switch (BP_GET_COMPRESS(bp)) {
	case ZIO_COMPRESS_OFF:
		/*
		 * [S] §2.6: with compression off and no raid-z, "lsize,
		 * asize, and psize will all be equal".
		 */
		if (psize != lsize)
			return (EINVAL);
		for (i = 0; i < lsize; i++)
			((uint8_t *)out)[i] = p[i];
		return (0);
	case ZIO_COMPRESS_LZ4:
		return (lz4_decompress(in, out, psize, lsize));
	case ZIO_COMPRESS_GZIP_1:
	case ZIO_COMPRESS_GZIP_1 + 1:
	case ZIO_COMPRESS_GZIP_1 + 2:
	case ZIO_COMPRESS_GZIP_1 + 3:
	case ZIO_COMPRESS_GZIP_1 + 4:
	case ZIO_COMPRESS_GZIP_1 + 5:
	case ZIO_COMPRESS_GZIP_1 + 6:
	case ZIO_COMPRESS_GZIP_1 + 7:
	case ZIO_COMPRESS_GZIP_9:
		return (gzip_decompress(in, out, psize, lsize));
	case ZIO_COMPRESS_ZLE:
		return (zle_decompress(in, out, psize, lsize));
	default:
		/*
		 * [S] §2.5, Table 6 has only lzjb, which nothing has
		 * written by default for many years; zstd is the other
		 * one [Z] zio_compress.h lists and is not carried here,
		 * because it would be a library rather than a function.
		 * Both are refused rather than guessed at.
		 */
		return (ENOTSUP);
	}
}

/*
 * Read one block, check it, and expand it.  buf is the logical block and
 * must be BP_GET_LSIZE(bp) bytes.
 *
 * [S] §2.1 allows up to three copies ("wideness"); each is tried in turn,
 * which is the whole of what a reader has to do about mirroring.
 */
int
zfs_read_block(struct zfs_pool *pool, const blkptr_t *bp, void *buf,
    size_t buflen)
{
	uint8_t *raw;
	size_t lsize, psize;
	int i, err = EIO;

	if (BP_IS_EMBEDDED(bp)) {
		/*
		 * [Z] spa.h: the payload is in the pointer, in every word
		 * but 6 and 0xa.  Nothing was read, so nothing is
		 * checked; the pointer was itself covered by the checksum
		 * of the block it came out of.
		 */
		static const int word[BPE_NUM_WORDS] = {
			0, 1, 2, 3, 4, 5, 7, 8, 9, 11, 12, 13, 14, 15
		};
		uint8_t payload[BPE_PAYLOAD_SIZE];
		const uint64_t *w = (const void *)bp;
		size_t i;

		if (BPE_GET_ETYPE(bp) != BP_EMBEDDED_TYPE_DATA)
			return (ENOTSUP);
		lsize = BPE_GET_LSIZE(bp);
		psize = BPE_GET_PSIZE(bp);
		if (buflen < lsize || psize > sizeof(payload))
			return (EINVAL);

		for (i = 0; i < BPE_NUM_WORDS; i++)
			((uint64_t *)(void *)payload)[i] = w[word[i]];

		return (block_decompress(bp, payload, psize, buf, lsize));
	}

	lsize = BP_GET_LSIZE(bp);
	psize = BP_GET_PSIZE(bp);
	if (buflen < lsize || psize > ZFS_MAXBLOCKSIZE)
		return (EINVAL);
	if ((raw = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL)
		return (ENOMEM);

	for (i = 0; i < SPA_DVAS_PER_BP; i++) {
		const dva_t *dva = &bp->blk_dva[i];

		if (dva->dva_word[0] == 0 && dva->dva_word[1] == 0)
			continue;
		if (DVA_GET_GANG(dva)) {
			/*
			 * [S] §2.3's gang block: 512 bytes holding up to
			 * three pointers and a self checksummed tail.
			 * Not yet; the next copy may be plain.
			 */
			err = ENOTSUP;
			continue;
		}
		if (DVA_GET_VDEV(dva) != 0) {
			/* One top-level vdev only, so far. */
			err = ENOTSUP;
			continue;
		}

		err = pool->pool_read(pool->pool_cookie, dva_offset(dva),
		    raw, psize);
		if (err != 0)
			continue;
		if (!block_checksum_ok(bp, raw, psize)) {
			err = EINVAL;
			continue;
		}
		err = block_decompress(bp, raw, psize, buf, lsize);
		if (err == 0)
			break;
	}
	zfs_scratch_put(raw, ZFS_MAXBLOCKSIZE);
	return (err);
}

/*
 * Objects.
 *
 * [S] §3.1: an object is a dnode, and a dnode addresses its data
 * through up to three block pointers and up to six levels of indirect
 * blocks.  Level 0 is the data; a block at level n holds pointers to
 * blocks at level n-1.
 */

/*
 * [S] §3.1: "The number of block pointers that an indirect block can
 * hold ... can be calculated by dividing the indirect block size by the
 * size of a blkptr (128 bytes)."
 */
static uint64_t
indirect_span(const dnode_phys_t *dn)
{

	return ((uint64_t)1 << (dn->dn_indblkshift - SPA_BLKPTRSHIFT));
}

/*
 * Is this dnode one the reader can use?
 *
 * Everything below is [S] §3.1: data blocks are 512 bytes to 128KB, so
 * dn_datablkszsec is 1 to 256 and dn_indblkshift is 9 to 17; there are
 * "between one and three" block pointers; and there are at most six
 * levels of indirection.
 *
 * The check is here rather than at each use because the fields are read
 * off a disk and every one of them is a shift count or an index:
 *
 *   - dn_indblkshift is shifted by, so 71 or more is undefined and
 *     anything under 7 makes indirect_span shift by a negative number,
 *     which is undefined as well.  The guard that used to stand in
 *     dnode_read_block computed "1 << dn_indblkshift" in order to
 *     compare it, so the check was itself the undefined operation.
 *   - dn_nblkptr indexes dn_blkptr[], which is three long.  A dnode
 *     claiming two hundred sends the reader off the end of the
 *     structure, which is a read of the caller's stack.
 *   - dn_nlevels bounds a loop whose product is a divisor.
 *
 * A bad dnode here means a corrupt or hostile pool, not a bug, so it is
 * refused rather than fixed up.
 */
static int
dnode_valid(const dnode_phys_t *dn)
{

	if (dn->dn_datablkszsec == 0 ||
	    dn->dn_datablkszsec > ZFS_MAXBLOCKSIZE / 512)
		return (0);
	if (dn->dn_nblkptr < 1 || dn->dn_nblkptr > SPA_DVAS_PER_BP)
		return (0);
	if (dn->dn_nlevels < 1 || dn->dn_nlevels > DN_MAX_LEVELS)
		return (0);
	if (dn->dn_nlevels > 1 &&
	    (dn->dn_indblkshift < SPA_MINBLOCKSHIFT ||
	    dn->dn_indblkshift > SPA_MAXBLOCKSHIFT))
		return (0);
	return (1);
}

/*
 * Read level 0 block blkid of an object.
 *
 * [S] §3.1 works its example the wrong way round: for 128K indirect
 * blocks holding 1024 pointers it says the parent of level 0 block
 * 16360 is "level 1 blkid = 16360%1024 = 15", but 16360 % 1024 is 1000.
 * 16360 / 1024 is 15, and the sentence before it -- "block 15 of level 1
 * contains the block pointer for level 0 blkid 16360" -- is what the
 * arithmetic has to produce.  Descending, the index at each level is
 * therefore the quotient by the span of the level below, and the
 * remainder is what the next step down uses.
 */
/*
 * A hole.
 *
 * [S] §2.10 says a block pointer's fill count is the number of non-zero
 * block pointers under it, which is zero for a hole, but a hole is
 * recognised here by having no DVA at all: nothing was allocated, so
 * there is nowhere to read from.
 *
 * A hole can stand at any level, not only at level 0.  A file written
 * as nothing but zeros has holes all the way up -- the dnode's own
 * block pointer is empty -- and descending into one reads a block
 * pointer full of zeros as if it addressed sector zero.  That is how
 * this was found: 4MB of zeros came back EIO instead of zeros.
 */
static int
bp_is_hole(const blkptr_t *bp)
{
	int i;

	if (BP_IS_EMBEDDED(bp))
		return (0);
	for (i = 0; i < SPA_DVAS_PER_BP; i++)
		if (bp->blk_dva[i].dva_word[0] != 0 ||
		    bp->blk_dva[i].dva_word[1] != 0)
			return (0);
	return (1);
}

static void
zero_block(void *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		((uint8_t *)buf)[i] = 0;
}

static int
dnode_read_block(struct zfs_pool *pool, const dnode_phys_t *dn,
    uint64_t blkid, void *buf, size_t buflen)
{
	uint8_t *ind = NULL;		/* one indirect block at a time */
	blkptr_t bp;
	uint64_t span, idx;
	size_t datasize;
	int level, err;

	if (!dnode_valid(dn))
		return (EINVAL);
	datasize = (size_t)dn->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (buflen < datasize)
		return (EINVAL);
	if (blkid > dn->dn_maxblkid)
		return (EINVAL);

	/*
	 * The top level lives in the dnode itself: dn_nblkptr pointers,
	 * each covering span^(nlevels-1) level 0 blocks.
	 */
	span = 1;
	for (level = 1; level < dn->dn_nlevels; level++)
		span *= indirect_span(dn);

	idx = blkid / span;
	if (idx >= dn->dn_nblkptr)
		return (EINVAL);
	bp = dn->dn_blkptr[idx];
	blkid %= span;

	if (dn->dn_nlevels > 1 &&
	    (ind = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL)
		return (ENOMEM);

	for (level = dn->dn_nlevels - 1; level > 0; level--) {
		if (bp_is_hole(&bp)) {
			zero_block(buf, datasize);
			zfs_scratch_put(ind, ZFS_MAXBLOCKSIZE);
			return (0);
		}
		err = zfs_read_block(pool, &bp, ind, ZFS_MAXBLOCKSIZE);
		if (err != 0) {
			zfs_scratch_put(ind, ZFS_MAXBLOCKSIZE);
			return (err);
		}

		span /= indirect_span(dn);
		idx = blkid / span;
		blkid %= span;
		bp = ((const blkptr_t *)(const void *)ind)[idx];
	}
	if (ind != NULL)
		zfs_scratch_put(ind, ZFS_MAXBLOCKSIZE);

	if (bp_is_hole(&bp)) {
		zero_block(buf, datasize);
		return (0);
	}

	return (zfs_read_block(pool, &bp, buf, buflen));
}

/*
 * [S] §3.2: "Each object within an object set is uniquely identified by
 * a 64 bit integer called an object number.  An object's object number
 * identifies the array element, in the dnode array, containing this
 * object's dnode_phys_t."
 *
 * So the dnode array is an ordinary object -- the metadnode's -- and
 * reading object N means reading 512 bytes at N * 512 of it.
 */
int
zfs_read_dnode(struct zfs_pool *pool, const dnode_phys_t *metadnode,
    uint64_t obj, dnode_phys_t *out)
{
	uint8_t *blk;
	size_t datasize, per;
	uint64_t off;
	int err;

	if (!dnode_valid(metadnode))
		return (EINVAL);
	datasize = (size_t)metadnode->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	per = datasize / DNODE_SIZE;

	if ((blk = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL)
		return (ENOMEM);
	err = dnode_read_block(pool, metadnode, obj / per, blk,
	    ZFS_MAXBLOCKSIZE);
	if (err != 0) {
		zfs_scratch_put(blk, ZFS_MAXBLOCKSIZE);
		return (err);
	}

	off = (obj % per) * DNODE_SIZE;
	*out = *(const dnode_phys_t *)(const void *)(blk + off);

	zfs_scratch_put(blk, ZFS_MAXBLOCKSIZE);

	/*
	 * Checked on the way out as well as on the way in, so that a
	 * dnode read off the disk is refused here and not wherever its
	 * shift counts are first used.
	 */
	return (dnode_valid(out) ? 0 : EINVAL);
}

int
zfs_read_object_block(struct zfs_pool *pool, const dnode_phys_t *dn,
    uint64_t blkid, void *buf, size_t buflen)
{

	return (dnode_read_block(pool, dn, blkid, buf, buflen));
}

/* A string valued pair, compared without copying it out. */
static int
nv_streq(const struct nvpair_value *v, const char *s)
{
	uint32_t i;

	for (i = 0; i < v->nv_strlen; i++)
		if (v->nv_string[i] != s[i] || s[i] == '\0')
			return (0);
	return (s[v->nv_strlen] == '\0');
}

/*
 * Opening a pool: read a label's nvlist, take from it what is needed to
 * read the rest, and then find the active uberblock.
 *
 * [S] §1.3.3 lists what is in the label: version, name, state, txg,
 * pool_guid, top_guid, guid and the vdev_tree, whose ashift says how
 * the uberblock array is spaced.
 */
int
zfs_pool_open(struct zfs_pool *pool, char *name, size_t namelen)
{
	uint8_t *nv;
	struct nvpair_value v, tree;
	int l, err = EINVAL;

	if ((nv = zfs_scratch_get(VDEV_LABEL_NVLIST_SIZE)) == NULL)
		return (ENOMEM);

	for (l = 0; l < VDEV_LABELS; l++) {
		uint64_t off = label_offset(l, pool->pool_size) +
		    VDEV_LABEL_NVLIST_OFF;

		if (pool->pool_read(pool->pool_cookie, off, nv,
		    VDEV_LABEL_NVLIST_SIZE) != 0)
			continue;

		/*
		 * [S] §1.3.3, Table 1 gives three states.  Which of them
		 * a reader will accept is the operating system's choice,
		 * not the format's: DESTROYED is refused, and EXPORTED is
		 * read, because a pool built on another machine and
		 * exported is exactly what an install image is.
		 */
		if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE, ZPOOL_CONFIG_POOL_STATE,
		    NV_WANT_UINT64, &v) != 0)
			continue;
		if (v.nv_u64 != POOL_STATE_ACTIVE &&
		    v.nv_u64 != POOL_STATE_EXPORTED)
			continue;
		if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE, ZPOOL_CONFIG_POOL_GUID,
		    NV_WANT_UINT64, &v) != 0)
			continue;
		pool->pool_guid = v.nv_u64;

		if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE, ZPOOL_CONFIG_VDEV_TREE,
		    NV_WANT_NVLIST, &tree) != 0)
			continue;
		/*
		 * [S] §1.3.3, Table 2 names the kinds of vdev, and which
		 * of them this can read follows from [S] §2.1: a DVA's
		 * offset is an offset into the *top-level* vdev.
		 *
		 * Under a mirror that offset means the same place on
		 * every child, because the children hold the same bytes,
		 * so one leaf is a complete copy of the pool and nothing
		 * has to be enumerated.  Under a raidz it does not: the
		 * data is spread across the children with parity, and an
		 * offset lands somewhere else on each of them.
		 *
		 * So mirrors are read and raidz is refused, and it is
		 * refused here, where the reason can be given.  Left to
		 * itself the reader would open the pool, follow the
		 * uberblock, read a block from the wrong place and fail
		 * its checksum -- reporting an I/O error on a disk that
		 * is perfectly sound.
		 */
		if (nvlist_find_nested(tree.nv_list, tree.nv_listlen,
		    ZPOOL_CONFIG_TYPE, NV_WANT_STRING, &v) != 0)
			continue;
		if (!nv_streq(&v, VDEV_TYPE_DISK) &&
		    !nv_streq(&v, VDEV_TYPE_FILE) &&
		    !nv_streq(&v, VDEV_TYPE_MIRROR) &&
		    !nv_streq(&v, VDEV_TYPE_REPLACING)) {
			err = ENOTSUP;
			break;
		}

		/*
		 * [S] §1.3.3: ashift belongs to the top-level vdev.  On a
		 * single disk the vdev_tree is that vdev and carries it;
		 * under a mirror or raidz the tree is the interior vdev
		 * and its children[0] does.  Only the value is wanted
		 * here -- reading from more than one disk is a separate
		 * matter -- so the first child answers either way.
		 */
		if (nvlist_find_nested(tree.nv_list, tree.nv_listlen,
		    ZPOOL_CONFIG_ASHIFT, NV_WANT_UINT64, &v) != 0) {
			struct nvpair_value ch;

			if (nvlist_find_nested(tree.nv_list, tree.nv_listlen,
			    ZPOOL_CONFIG_CHILDREN, NV_WANT_NVLIST, &ch) != 0)
				continue;
			if (nvlist_find_nested(ch.nv_list, ch.nv_listlen,
			    ZPOOL_CONFIG_ASHIFT, NV_WANT_UINT64, &v) != 0)
				continue;
		}
		if (v.nv_u64 < SPA_MINBLOCKSHIFT ||
		    v.nv_u64 > SPA_MAXBLOCKSHIFT)
			continue;
		pool->pool_ashift = (uint32_t)v.nv_u64;

		if (name != NULL && namelen > 0 &&
		    nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE, ZPOOL_CONFIG_POOL_NAME,
		    NV_WANT_STRING, &v) == 0) {
			size_t i, n = v.nv_strlen;

			if (n > namelen - 1)
				n = namelen - 1;
			for (i = 0; i < n; i++)
				name[i] = v.nv_string[i];
			name[n] = '\0';
		}

		err = 0;
		break;
	}
	zfs_scratch_put(nv, VDEV_LABEL_NVLIST_SIZE);
	if (err != 0)
		return (err);

	return (zfs_uberblock_find(pool));
}
