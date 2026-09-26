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
 *
 * N is not quite the device's size.  [Z] vdev.c (vdev_open) rounds the
 * size down to a whole number of labels before placing them, and
 * vdev_label.c asserts that it has.  A partition whose size is not a
 * multiple of 256K -- most of them -- has its last two labels up to
 * 256K short of the end, and read at the unrounded size they were
 * never found; a pool whose first two labels were damaged could not
 * be opened although the other two were intact.
 */
static uint64_t
label_offset(int l, uint64_t vdev_size)
{

	vdev_size &= ~(uint64_t)(VDEV_LABEL_SIZE - 1);
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
eck_valid(const void *slot, size_t slotsize, const uint64_t verifier[4])
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
	for (i = 0; i < 4; i++)
		sub.zec_cksum[i] = verifier[i];

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

static int
uberblock_valid(const void *slot, size_t slotsize, uint64_t offset)
{
	const uint64_t verifier[4] = { offset, 0, 0, 0 };

	return (eck_valid(slot, slotsize, verifier));
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
label_scan(const struct zfs_leaf *lf, int l, uint32_t ashift,
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

	base = label_offset(l, lf->lf_size) + VDEV_LABEL_UBERBLOCK_OFF;

	for (off = 0; off + slotsize <= VDEV_LABEL_UBERBLOCK_SIZE;
	    off += slotsize) {
		const struct uberblock *ub = (const void *)slot;

		err = lf->lf_read(lf->lf_cookie, base + off, slot, slotsize);
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
 * A pool that was not opened with zfs_pool_open -- the test drivers
 * that walk a pool by hand do this -- has an empty table of vdevs.  The
 * device it was given is then the whole of it.
 */
static void
pool_single(struct zfs_pool *pool)
{
	struct zfs_top *tv = &pool->pool_top[0];

	pool->pool_ntops = 1;
	tv->tv_type = ZFS_VT_LEAF;
	tv->tv_ashift = pool->pool_ashift;
	tv->tv_nchildren = 1;
	tv->tv_child[0].lf_read = pool->pool_read;
	tv->tv_child[0].lf_cookie = pool->pool_cookie;
	tv->tv_child[0].lf_size = pool->pool_size;
}

/*
 * [S] §1.2.2: the four labels are written in two stages, so at least one
 * is always intact.  All four are read for that reason, and the newest
 * valid uberblock across all of them wins.
 *
 * [S] §1.3.4 speaks of one device.  Every leaf of a pool carries the
 * same uberblocks, and a leaf that missed some transaction groups --
 * one half of a mirror that was away -- carries older ones, so every
 * leaf found is read and the newest across all of them is taken.  This
 * is also what [Z] vdev_label.c (vdev_uberblock_load) does.
 */
int
zfs_uberblock_find(struct zfs_pool *pool)
{
	struct uberblock ub, best;
	uint64_t off;
	uint32_t t, c;
	int l, found = 0;

	if (pool->pool_ntops == 0)
		pool_single(pool);

	for (t = 0; t < pool->pool_ntops; t++) {
		const struct zfs_top *tv = &pool->pool_top[t];

		for (c = 0; c < tv->tv_nchildren; c++) {
			const struct zfs_leaf *lf = &tv->tv_child[c];

			if (lf->lf_read == NULL)
				continue;
			for (l = 0; l < VDEV_LABELS; l++) {
				if (!label_scan(lf, l, tv->tv_ashift, &ub,
				    &off))
					continue;
				if (found && ub.ub_txg <= best.ub_txg)
					continue;
				best = ub;
				found = 1;
			}
		}
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

#ifdef ZFS_FUZZ_NO_CKSUM
	/*
	 * For the fuzzer only, and never for a build that boots
	 * anything.  The threat this reader is hardened against is a
	 * disk written on purpose, and whoever writes one recomputes
	 * the checksums -- so a fuzzer that has to get past them is
	 * testing fletcher4 rather than the parsing underneath it.
	 */
	(void)bp; (void)buf; (void)psize; (void)got; (void)i;
	return (1);
#else

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
#endif
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

static int	bp_is_hole(const blkptr_t *);
static int	dva_read(struct zfs_pool *, const blkptr_t *, int, uint8_t *,
		    size_t, int);

/*
 * Read len bytes of a DVA from its top-level vdev, and hand them to
 * verify; the first copy it accepts is the answer.
 *
 * [S] §2.1: a DVA's vdev field is "the top-level vdev", and its offset
 * is an offset into that vdev.  A disk has one copy of the bytes.  A
 * mirror's children hold the same bytes at the same offsets, so any
 * one of them will do -- and when one fails its checksum, the next may
 * not, which is the other half of what a mirror is for.  The verify
 * function is how this layer knows a copy is bad without knowing what
 * the bytes are.
 */
typedef int (*zfs_verifyfn_t)(void *, const void *, size_t);

static int
vdev_read(struct zfs_pool *pool, const dva_t *dva, void *buf, size_t len,
    zfs_verifyfn_t verify, void *arg)
{
	const struct zfs_top *tv;
	uint64_t id = DVA_GET_VDEV(dva);
	uint32_t c;
	int err;

	if (id >= pool->pool_ntops)
		return (ENXIO);
	tv = &pool->pool_top[id];

	switch (tv->tv_type) {
	case ZFS_VT_LEAF:
	case ZFS_VT_MIRROR:
		err = ENXIO;
		for (c = 0; c < tv->tv_nchildren; c++) {
			const struct zfs_leaf *lf = &tv->tv_child[c];

			if (lf->lf_read == NULL)
				continue;
			err = lf->lf_read(lf->lf_cookie, dva_offset(dva), buf,
			    len);
			if (err != 0)
				continue;
			if (verify(arg, buf, len))
				return (0);
			err = EINVAL;
		}
		return (err);
	case ZFS_VT_NONE:
		/* On a device that was not found. */
		return (ENXIO);
	default:
		return (ENOTSUP);
	}
}

static int
bp_verify(void *arg, const void *buf, size_t len)
{

	return (block_checksum_ok(arg, buf, len));
}

/*
 * [S] §2.3 calls a gang header "self checksumming" and says no more.
 * [Z] zio_checksum.c: the tail's checksum is SHA-256 over the whole
 * header with the checksum field holding a verifier while it is taken,
 * as for the uberblock, and the verifier is the vdev and byte offset of
 * DVA[0] with the block's physical birth txg.  DVA[0] even when the copy
 * being read is DVA[1]'s: the verifier names the block, not the copy.
 */
static int
gang_header_valid(const blkptr_t *bp, const void *hdr, size_t size)
{
	const dva_t *dva = &bp->blk_dva[0];
	const uint64_t verifier[4] = {
		DVA_GET_VDEV(dva), DVA_GET_OFFSET(dva),
		BP_GET_PHYSICAL_BIRTH(bp), 0
	};

#ifdef ZFS_FUZZ_NO_CKSUM
	/*
	 * As in block_checksum_ok(), and for the same reason.  The tail's
	 * magic is still required, so that the header size is chosen as
	 * it would be with the checksum in place.
	 */
	(void)verifier;
	return (((const struct zio_eck *)(const void *)((const uint8_t *)hdr +
	    size - sizeof(struct zio_eck)))->zec_magic == ZEC_MAGIC);
#else
	return (eck_valid(hdr, size, verifier));
#endif
}

/*
 * Assemble the data of a gang block from DVA d of bp into out, which is
 * BP_GET_PSIZE(bp) bytes.
 *
 * [S] §2.3 describes the header; what it leaves to be inferred is how
 * the members make up the block.  [Z] zio.c (zio_gang_tree_issue) lays
 * them end to end in the order the header lists them, skipping holes,
 * each contributing its own psize, and requires that they add up to the
 * gang block's psize exactly.  A member is an ordinary block pointer
 * with its own checksum, over its own piece, and may itself be a gang
 * block.  The member pieces are never compressed on their own: the
 * gang block's compression applies to the assembled whole, and so does
 * its checksum -- [S] §2.4: "The computed checksum is always of the
 * data, even if this is a gang block."  The caller checks that.
 */
/*
 * Try the header at the vdev's allocation size and then at [S]'s 512
 * bytes; see SPA_GANGBLOCKSIZE.  Reading the larger size is always in
 * bounds, because it is what the header was given on the disk whichever
 * kind it is.
 */
struct gang_verify {
	const blkptr_t	*gv_bp;
	size_t		gv_hdrsize;	/* out: which size checked out */
};

static int
gang_verify(void *arg, const void *hdr, size_t bufsize)
{
	struct gang_verify *gv = arg;

	if (gang_header_valid(gv->gv_bp, hdr, bufsize)) {
		gv->gv_hdrsize = bufsize;
		return (1);
	}
	if (bufsize > SPA_GANGBLOCKSIZE &&
	    gang_header_valid(gv->gv_bp, hdr, SPA_GANGBLOCKSIZE)) {
		gv->gv_hdrsize = SPA_GANGBLOCKSIZE;
		return (1);
	}
	return (0);
}

static int
gang_read(struct zfs_pool *pool, const blkptr_t *bp, int d, uint8_t *out,
    size_t psize, int depth)
{
	struct gang_verify gv;
	uint8_t *hdr;
	uint64_t id = DVA_GET_VDEV(&bp->blk_dva[d]);
	size_t bufsize, hdrsize, off, g;
	int j, err;

	if (depth >= ZFS_GANG_MAXDEPTH)
		return (ENOTSUP);
	if (id >= pool->pool_ntops)
		return (ENXIO);

	bufsize = (size_t)1 << pool->pool_top[id].tv_ashift;
	if (bufsize < SPA_GANGBLOCKSIZE)
		bufsize = SPA_GANGBLOCKSIZE;
	if ((hdr = zfs_scratch_get(bufsize)) == NULL)
		return (ENOMEM);

	gv.gv_bp = bp;
	err = vdev_read(pool, &bp->blk_dva[d], hdr, bufsize, gang_verify,
	    &gv);
	if (err != 0)
		goto out;
	hdrsize = gv.gv_hdrsize;

	off = 0;
	for (g = 0; g < GBH_NBLKPTRS(hdrsize); g++) {
		const blkptr_t *gbp = &((const blkptr_t *)(const void *)hdr)[g];
		size_t mpsize;

		if (bp_is_hole(gbp))
			continue;
		/*
		 * Everything below comes out of a header that checked
		 * out, but a checksum says the header is the one that
		 * was written, not that its writer meant well.
		 */
		if (BP_IS_EMBEDDED(gbp)) {
			err = EINVAL;
			goto out;
		}
		mpsize = BP_GET_PSIZE(gbp);
		if (mpsize > psize - off) {
			err = EINVAL;
			goto out;
		}
		err = EIO;
		for (j = 0; j < SPA_DVAS_PER_BP; j++) {
			if (gbp->blk_dva[j].dva_word[0] == 0 &&
			    gbp->blk_dva[j].dva_word[1] == 0)
				continue;
			err = dva_read(pool, gbp, j, out + off, mpsize,
			    depth + 1);
			if (err == 0)
				break;
		}
		if (err != 0)
			goto out;
		off += mpsize;
	}
	if (off != psize)
		err = EINVAL;
out:
	zfs_scratch_put(hdr, bufsize);
	return (err);
}

/*
 * Read psize bytes of copy d of bp into out, and check them against
 * bp's checksum.  Nothing is expanded.
 */
static int
dva_read(struct zfs_pool *pool, const blkptr_t *bp, int d, uint8_t *out,
    size_t psize, int depth)
{
	const dva_t *dva = &bp->blk_dva[d];
	int err;

	if (!DVA_GET_GANG(dva))
		return (vdev_read(pool, dva, out, psize, bp_verify,
		    (void *)(uintptr_t)bp));

	/*
	 * A gang block's members were each checked as they were read,
	 * and each from whichever copy passed; the whole is checked
	 * once it is assembled.
	 */
	err = gang_read(pool, bp, d, out, psize, depth);
	if (err != 0)
		return (err);
	if (!block_checksum_ok(bp, out, psize))
		return (EINVAL);
	return (0);
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
		err = dva_read(pool, bp, i, raw, psize, 0);
		if (err != 0)
			continue;
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
	/*
	 * [S] §3.1: the bonus buffer "can range between 64 and 320
	 * bytes", and it begins where the block pointers end, so a
	 * length that does not fit in the 512 byte dnode is a lie.
	 */
	if (dn->dn_bonuslen > DN_MAX_BONUSLEN ||
	    offsetof(dnode_phys_t, dn_blkptr) +
	    (size_t)dn->dn_nblkptr * sizeof(blkptr_t) +
	    dn->dn_bonuslen > DNODE_SIZE)
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
 * [S] §1.3.3, Table 2 names the kinds of vdev, and which of them this
 * can read follows from [S] §2.1: a DVA's offset is an offset into the
 * *top-level* vdev.
 *
 * Under a mirror that offset means the same place on every child,
 * because the children hold the same bytes.  So do the children of a
 * replacing vdev, while one disk is being copied to another, and of a
 * spare that has taken over; [Z] vdev_replacing_ops and vdev_spare_ops
 * read them as a mirror, and so does this.
 *
 * Under a raidz it does not: the data is spread across the children
 * with parity, and an offset lands somewhere else on each of them.  So
 * raidz is refused, and it is refused when the pool is opened, where
 * the reason can be given.  Left to itself the reader would follow the
 * uberblock, read a block from the wrong place and fail its checksum
 * -- reporting an I/O error on a disk that is perfectly sound.
 */
static int
vdev_type(const struct nvpair_value *v)
{

	if (nv_streq(v, VDEV_TYPE_DISK) || nv_streq(v, VDEV_TYPE_FILE))
		return (ZFS_VT_LEAF);
	if (nv_streq(v, VDEV_TYPE_MIRROR) ||
	    nv_streq(v, VDEV_TYPE_REPLACING) ||
	    nv_streq(v, VDEV_TYPE_SPARE))
		return (ZFS_VT_MIRROR);
	return (ZFS_VT_NONE);
}

static int
nv_guid(const struct nvpair_value *list, uint64_t *guid)
{
	struct nvpair_value v;

	if (nvlist_find_nested(list->nv_list, list->nv_listlen,
	    ZPOOL_CONFIG_GUID, NV_WANT_UINT64, &v) != 0)
		return (EINVAL);
	*guid = v.nv_u64;
	return (0);
}

/*
 * Enter a device in the pool's table: its top-level vdev, as the
 * label's vdev_tree describes it, and the device itself as the leaf of
 * that vdev whose guid it carries.
 *
 * [S] §1.3.3: each label holds the tree of its own top-level vdev and
 * nothing of the others, so the table fills in as devices are found.
 * The first to name a top-level vdev describes it; any later one must
 * agree.
 */
static int
top_enter(struct zfs_pool *pool, const struct nvpair_value *tree,
    uint64_t guid, const struct zfs_leaf *dev, uint32_t *ashiftp)
{
	struct nvpair_value v, ch, el;
	struct zfs_top *tv;
	uint64_t id, cguid;
	uint32_t i, ashift;
	int type;

	if (nvlist_find_nested(tree->nv_list, tree->nv_listlen,
	    ZPOOL_CONFIG_ID, NV_WANT_UINT64, &v) != 0)
		return (EINVAL);
	id = v.nv_u64;
	if (id >= pool->pool_ntops)
		return (EINVAL);
	tv = &pool->pool_top[id];

	if (nvlist_find_nested(tree->nv_list, tree->nv_listlen,
	    ZPOOL_CONFIG_TYPE, NV_WANT_STRING, &v) != 0)
		return (EINVAL);
	if ((type = vdev_type(&v)) == ZFS_VT_NONE)
		return (ENOTSUP);

	/*
	 * [S] §1.3.3: ashift belongs to the top-level vdev.  A disk's
	 * tree is that vdev and carries it; under a mirror the tree is
	 * the interior vdev, and older pools put it on children[0].
	 */
	if (nvlist_find_nested(tree->nv_list, tree->nv_listlen,
	    ZPOOL_CONFIG_CHILDREN, NV_WANT_NVLIST, &ch) != 0)
		ch.nv_nelem = 0;
	if (nvlist_find_nested(tree->nv_list, tree->nv_listlen,
	    ZPOOL_CONFIG_ASHIFT, NV_WANT_UINT64, &v) != 0) {
		if (ch.nv_nelem == 0 ||
		    nvlist_array_elem(&ch, 0, &el) != 0 ||
		    nvlist_find_nested(el.nv_list, el.nv_listlen,
		    ZPOOL_CONFIG_ASHIFT, NV_WANT_UINT64, &v) != 0)
			return (EINVAL);
	}
	/*
	 * ashift comes off the disk and is about to be shifted by, so
	 * it is the value that is checked, not the result.
	 */
	if (v.nv_u64 < SPA_MINBLOCKSHIFT || v.nv_u64 > SPA_MAXBLOCKSHIFT)
		return (EINVAL);
	ashift = (uint32_t)v.nv_u64;

	if (tv->tv_type == ZFS_VT_NONE) {
		tv->tv_ashift = ashift;
		tv->tv_nparity = 0;
		tv->tv_nchildren = 0;
		if (type == ZFS_VT_LEAF) {
			if (nv_guid(tree, &cguid) != 0)
				return (EINVAL);
			tv->tv_child[0].lf_guid = cguid;
			tv->tv_child[0].lf_read = NULL;
			tv->tv_nchildren = 1;
		} else {
			if (ch.nv_nelem == 0 ||
			    ch.nv_nelem > ZFS_MAX_CHILDREN)
				return (ENOTSUP);
			for (i = 0; i < ch.nv_nelem; i++) {
				if (nvlist_array_elem(&ch, i, &el) != 0 ||
				    nv_guid(&el, &cguid) != 0)
					return (EINVAL);
				/*
				 * A child may itself be interior -- a
				 * mirror's disk being replaced.  Its
				 * guid is then no device's, so it is
				 * never found and the other children,
				 * each a whole copy, are read instead.
				 */
				tv->tv_child[i].lf_guid = cguid;
				tv->tv_child[i].lf_read = NULL;
			}
			tv->tv_nchildren = ch.nv_nelem;
		}
		tv->tv_type = type;
	} else if (tv->tv_type != type || tv->tv_ashift != ashift)
		return (EINVAL);

	*ashiftp = ashift;
	for (i = 0; i < tv->tv_nchildren; i++) {
		struct zfs_leaf *lf = &tv->tv_child[i];

		if (lf->lf_guid != guid)
			continue;
		if (lf->lf_read != NULL)
			return (EEXIST);	/* offered twice */
		lf->lf_read = dev->lf_read;
		lf->lf_cookie = dev->lf_cookie;
		lf->lf_size = dev->lf_size;
		return (0);
	}
	/* Once part of this pool, and since detached from it. */
	return (ENOENT);
}

/*
 * Read one device's label and, if it belongs to the pool, enter it.
 * The first device decides which pool that is: the one the loader was
 * pointed at.  Returns zero only if the device was entered.
 *
 * [S] §1.3.3 lists what is in the label: version, name, state, txg,
 * pool_guid, top_guid, guid and the vdev_tree.
 */
static int
label_enter(struct zfs_pool *pool, const struct zfs_leaf *dev, uint8_t *nv,
    int first, char *name, size_t namelen)
{
	struct nvpair_value v, tree;
	uint64_t guid;
	uint32_t ashift;
	int l, err = EINVAL;

	for (l = 0; l < VDEV_LABELS; l++) {
		uint64_t off = label_offset(l, dev->lf_size) +
		    VDEV_LABEL_NVLIST_OFF;

		if (dev->lf_read(dev->lf_cookie, off, nv,
		    VDEV_LABEL_NVLIST_SIZE) != 0)
			continue;

		/*
		 * [S] §1.3.3, Table 1 gives three states.  Which of them
		 * a reader will accept is the operating system's choice,
		 * not the format's: DESTROYED is refused, and EXPORTED is
		 * read, because a pool built on another machine and
		 * exported is exactly what an install image is.
		 */
		if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE,
		    ZPOOL_CONFIG_POOL_STATE, NV_WANT_UINT64, &v) != 0)
			continue;
		if (v.nv_u64 != POOL_STATE_ACTIVE &&
		    v.nv_u64 != POOL_STATE_EXPORTED)
			continue;
		if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE,
		    ZPOOL_CONFIG_POOL_GUID, NV_WANT_UINT64, &v) != 0)
			continue;
		if (first)
			pool->pool_guid = v.nv_u64;
		else if (v.nv_u64 != pool->pool_guid)
			return (ENOENT);	/* another pool's */

		if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE, ZPOOL_CONFIG_GUID,
		    NV_WANT_UINT64, &v) != 0)
			continue;
		guid = v.nv_u64;
		if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE,
		    ZPOOL_CONFIG_VDEV_TREE, NV_WANT_NVLIST, &tree) != 0)
			continue;

		if (first) {
			/*
			 * [S]'s label cannot say how many top-level
			 * vdevs there are; [Z] added vdev_children for
			 * it.  A label without one is from a pool that
			 * predates it, and such a pool had one.
			 */
			if (nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE,
			    ZPOOL_CONFIG_VDEV_CHILDREN, NV_WANT_UINT64,
			    &v) != 0)
				v.nv_u64 = 1;
			if (v.nv_u64 == 0 || v.nv_u64 > ZFS_MAX_TOPS)
				return (ENOTSUP);
			pool->pool_ntops = (uint32_t)v.nv_u64;
		}

		err = top_enter(pool, &tree, guid, dev, &ashift);
		if (err == EINVAL)
			continue;	/* try the label's other copies */
		if (err != 0)
			return (err);

		if (first) {
			pool->pool_ashift = ashift;
			if (name != NULL && namelen > 0 &&
			    nvlist_find(nv, VDEV_LABEL_NVLIST_SIZE,
			    ZPOOL_CONFIG_POOL_NAME, NV_WANT_STRING, &v) == 0) {
				size_t i, n = v.nv_strlen;

				if (n > namelen - 1)
					n = namelen - 1;
				for (i = 0; i < n; i++)
					name[i] = v.nv_string[i];
				name[n] = '\0';
			}
		}
		return (0);
	}
	return (err);
}

/*
 * Opening a pool: read the label of the device the pool was found on,
 * then those of every other device the host can offer, keep the ones
 * that belong, and find the active uberblock across all of them.
 */
int
zfs_pool_open(struct zfs_pool *pool, char *name, size_t namelen)
{
	struct zfs_leaf dev;
	uint8_t *nv;
	uint32_t t;
	int i, err;

	pool->pool_ntops = 0;
	for (t = 0; t < ZFS_MAX_TOPS; t++) {
		pool->pool_top[t].tv_type = ZFS_VT_NONE;
		pool->pool_top[t].tv_nchildren = 0;
	}

	if ((nv = zfs_scratch_get(VDEV_LABEL_NVLIST_SIZE)) == NULL)
		return (ENOMEM);

	dev.lf_guid = 0;
	dev.lf_read = pool->pool_read;
	dev.lf_cookie = pool->pool_cookie;
	dev.lf_size = pool->pool_size;
	err = label_enter(pool, &dev, nv, 1, name, namelen);

	/*
	 * The host answers ENOENT once it has offered everything; any
	 * other error is a device it could not open, and is passed over.
	 * The count is bounded all the same, since the host's answer is
	 * not the reader's to trust.
	 */
	for (i = 0; err == 0 && pool->pool_probe != NULL &&
	    i < ZFS_MAX_PROBE; i++) {
		int perr;

		dev.lf_read = NULL;
		perr = pool->pool_probe(pool->pool_probe_cookie, i,
		    &dev.lf_read, &dev.lf_cookie, &dev.lf_size);
		if (perr == ENOENT)
			break;
		if (perr != 0 || dev.lf_read == NULL)
			continue;
		if (label_enter(pool, &dev, nv, 0, NULL, 0) != 0 &&
		    pool->pool_release != NULL)
			pool->pool_release(pool->pool_probe_cookie,
			    dev.lf_cookie);
	}
	zfs_scratch_put(nv, VDEV_LABEL_NVLIST_SIZE);
	if (err != 0)
		return (err);

	return (zfs_uberblock_find(pool));
}
