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
#include "sha512.h"
#include "skein.h"
#include "edonr.h"
#include "blake3.h"
#include "fletcher.h"
#include "lz4.h"
#include "gzip.h"
#include "zle.h"
#include "zstd.h"
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
block_checksum_ok(const struct zfs_pool *pool, const blkptr_t *bp,
    const void *buf, size_t psize)
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
	(void)pool; (void)bp; (void)buf; (void)psize; (void)got; (void)i;
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
	case ZIO_CHECKSUM_SHA512:
		sha512_256(buf, psize, NULL, got);
		break;
	case ZIO_CHECKSUM_SKEIN:
	case ZIO_CHECKSUM_EDONR:
	case ZIO_CHECKSUM_BLAKE3:
		/*
		 * [Z] zio_checksum.c: these three are keyed with the
		 * pool's salt.  Without it there is nothing to check
		 * against, and a block is not read unchecked.
		 */
		if (!pool->pool_salt_ok)
			return (0);
		if (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_SKEIN)
			skein_zfs(pool->pool_salt, buf, psize, got);
		else if (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_EDONR)
			edonr_zfs(pool->pool_salt, buf, psize, got);
		else
			blake3_keyed(pool->pool_salt, buf, psize, NULL, got);
		break;
	default:
		/*
		 * Anything past [Z] zio_checksum.h's blake3 is refused
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
	case ZIO_COMPRESS_ZSTD:
		return (zfs_zstd_decompress(in, out, psize, lsize));
	default:
		/*
		 * [S] §2.5, Table 6 has only lzjb, which nothing has
		 * written by default for many years.  It is refused
		 * rather than guessed at.
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

/*
 * raidz.
 *
 * [S] names the vdev type and says a DVA's asize includes its parity,
 * and nothing more; everything below is from OpenZFS, file and function
 * named at each step, all at openzfs/zfs 81b19c6.
 */
struct raidz_col {
	uint32_t	rc_devidx;	/* which child */
	uint64_t	rc_offset;	/* on that child, labels not counted */
	size_t		rc_size;	/* bytes, 0 for a column past the end */
};

/*
 * One row of a block's map: its columns, parity first, and for each
 * data column where its bytes belong in the block.  [Z] vdev_raidz.c
 * calls it raidz_row_t.  A block on a raidz that has never been widened
 * is one row whose columns are many sectors tall; one written at an
 * older width of a widened raidz is many rows of one sector each.
 */
struct raidz_row {
	uint64_t	rr_cols;
	struct raidz_col rr_col[ZFS_MAX_CHILDREN];
	size_t		rr_off[ZFS_MAX_CHILDREN];
};

/* What it takes to work out any row of a block's map. */
struct raidz_map {
	uint64_t	rm_ashift;
	uint64_t	rm_np;
	uint64_t	rm_offset;	/* the DVA's, into the raidz */
	uint64_t	rm_s;		/* data sectors */
	uint64_t	rm_q, rm_r, rm_bc;
	uint64_t	rm_rows;
	size_t		rm_psize;	/* bytes in one parity column */
	int		rm_expanded;
	uint64_t	rm_dcols, rm_acols;		/* not expanded */
	uint64_t	rm_cols, rm_logical, rm_physical;	/* expanded */
	uint64_t	rm_synced;
	int		rm_scratch;
};

/* Read column c into buf, from a child that was found. */
static int
raidz_col_read(const struct zfs_top *tv, const struct raidz_col *rc,
    void *buf)
{
	const struct zfs_leaf *lf = &tv->tv_child[rc->rc_devidx];

	if (lf->lf_read == NULL)
		return (ENXIO);
	/*
	 * [Z] module/zfs/zio.c, zio_vdev_child_io(): an I/O to a leaf
	 * vdev has VDEV_LABEL_START_SIZE added to its offset -- the same
	 * 4MB [S] §2.1 adds for a disk that is its own top-level vdev.
	 * An offset into the scratch area below it has wrapped, and
	 * comes back round here, as it does in [Z].
	 */
	return (lf->lf_read(lf->lf_cookie, rc->rc_offset +
	    VDEV_LABEL_START_SIZE, buf, rc->rc_size));
}

/*
 * [Z] vdev_raidz.c, vdev_raidz_io_start() and vdev_raidz_init(): the
 * width a block was written at is the one in force at its physical
 * birth txg -- that of the last widening at or before it, or else the
 * width the raidz was made with.  raidz_expand_txgs is ascending, its
 * last entry is the current width, each one before it one narrower, and
 * while a widening is under way every one of them is one narrower
 * again, since the new child does not yet count.
 */
static uint64_t
raidz_width(const struct zfs_top *tv, uint64_t birth)
{
	uint64_t w = tv->tv_nchildren - (uint64_t)tv->tv_expanding;
	uint32_t i;

	for (i = tv->tv_nexpand; i-- > 0; w--)
		if (tv->tv_expand_txg[i] <= birth)
			return (w);
	return (w);
}

static uint64_t
raidz_original_width(const struct zfs_top *tv)
{

	return (tv->tv_nchildren - tv->tv_nexpand -
	    (uint64_t)tv->tv_expanding);
}

static void
raidz_map_init(struct raidz_map *rm, const struct zfs_pool *pool,
    const struct zfs_top *tv, uint64_t offset, size_t len, uint64_t width)
{
	uint64_t np = tv->tv_nparity, sec = (uint64_t)1 << tv->tv_ashift;

	rm->rm_ashift = tv->tv_ashift;
	rm->rm_np = np;
	rm->rm_offset = offset;
	/*
	 * [Z] zio.c, zio_vdev_io_start(): the I/O is rounded up to the
	 * raidz's sectors, and a write padded with zeros.
	 */
	rm->rm_s = (len + sec - 1) >> tv->tv_ashift;
	rm->rm_q = rm->rm_s / (width - np);
	rm->rm_r = rm->rm_s - rm->rm_q * (width - np);
	rm->rm_bc = (rm->rm_r == 0 ? 0 : rm->rm_r + np);

	if (width == tv->tv_nchildren) {
		/*
		 * [Z] vdev_raidz.c, vdev_raidz_map_alloc(): one row.  Each
		 * of the first bc columns holds q + 1 sectors and the rest
		 * q; the first np columns are parity.  A block too small
		 * to fill a row uses only the columns it needs.  The skip
		 * sectors that pad a block's allocation to a multiple of
		 * np + 1 are after its last column and never read.
		 */
		rm->rm_expanded = 0;
		rm->rm_dcols = width;
		rm->rm_acols = (rm->rm_q == 0 ? rm->rm_bc : width);
		rm->rm_rows = 1;
		rm->rm_psize = (size_t)(rm->rm_q + (rm->rm_r != 0)) <<
		    tv->tv_ashift;
		return;
	}

	/*
	 * [Z] vdev_raidz.c, vdev_raidz_map_alloc_expanded(): a block
	 * written at a width narrower than the raidz is now is laid out
	 * a sector per column, in rows of that width.
	 */
	rm->rm_expanded = 1;
	rm->rm_logical = width;
	rm->rm_physical = tv->tv_nchildren;
	{
		uint64_t tot = rm->rm_s +
		    np * (rm->rm_q + (rm->rm_r == 0 ? 0 : 1));

		rm->rm_rows = (tot + width - 1) / width;
		rm->rm_cols = (tot < width ? tot : width);
	}
	rm->rm_psize = (size_t)sec;
	/*
	 * [Z] vdev_raidz_io_start(): while a raidz is being widened, the
	 * uberblock says how far the move has got and whether the rows
	 * below that are still only in the scratch area.  Otherwise
	 * every row has been moved, which [Z] writes as UINT64_MAX.
	 */
	if (tv->tv_expanding) {
		rm->rm_synced = RRSS_GET_OFFSET(&pool->pool_ub);
		rm->rm_scratch = (RRSS_GET_STATE(&pool->pool_ub) ==
		    RRSS_SCRATCH_VALID);
	} else {
		rm->rm_synced = ~(uint64_t)0;
		rm->rm_scratch = 0;
	}
}

/*
 * [Z] vdev_raidz_map_alloc() and vdev_raidz_map_alloc_expanded(),
 * restricted to one row and to reading.
 */
static int
raidz_map_row(const struct raidz_map *rm, uint64_t row, struct raidz_row *rr)
{
	uint64_t ashift = rm->rm_ashift, np = rm->rm_np, c;
	uint64_t sec = (uint64_t)1 << ashift;

	if (!rm->rm_expanded) {
		uint64_t dcols = rm->rm_dcols, b = rm->rm_offset >> ashift;
		uint64_t f = b % dcols, o = (b / dcols) << ashift;
		size_t d = 0;

		rr->rr_cols = rm->rm_acols;
		for (c = 0; c < rr->rr_cols; c++) {
			uint64_t cc = f + c, coff = o;

			if (cc >= dcols) {
				cc -= dcols;
				coff += sec;
			}
			rr->rr_col[c].rc_devidx = (uint32_t)cc;
			rr->rr_col[c].rc_offset = coff;
			rr->rr_col[c].rc_size = (size_t)(rm->rm_q +
			    (c < rm->rm_bc)) << ashift;
			/*
			 * [Z] vdev_raidz_map_alloc_read(): the data columns
			 * hold the block's bytes in order.
			 */
			if (c >= np) {
				rr->rr_off[c] = d;
				d += rr->rr_col[c].rc_size;
			}
		}
		if (d != rm->rm_s << ashift)
			return (EINVAL);
	} else {
		uint64_t rows = rm->rm_rows, r = rm->rm_r;
		uint64_t b = (rm->rm_offset >> ashift) + row * rm->rm_logical;
		uint64_t phys = rm->rm_physical, child, coff;
		int scratch = 0;

		/*
		 * [Z] vdev_raidz_map_alloc_expanded(): a row not wholly
		 * below the offset the move has reached is where it was,
		 * on the children there were before; one that is, is at
		 * its new place -- or, while the scratch area is valid, at
		 * that place less VDEV_BOOT_SIZE.  [Z] takes the row to be
		 * the full cols wide for this, the last row included.
		 */
		if (b + rm->rm_cols > rm->rm_synced >> ashift)
			phys--;
		else if (rm->rm_scratch)
			scratch = 1;
		child = b % phys;
		coff = (b / phys) << ashift;

		/*
		 * [Z] the same function: rr_cols is the whole width even
		 * where the last row is short, so that Q and R are taken
		 * over zeros there; data sector off of the block is dc *
		 * rows + row in the first r data columns, which have a
		 * sector in every row, and the rest have one fewer.
		 */
		rr->rr_cols = rm->rm_cols;
		for (c = 0; c < rr->rr_cols; c++, child++) {
			struct raidz_col *rc = &rr->rr_col[c];
			uint64_t dc = c - np, off;

			if (child >= phys) {
				child -= phys;
				coff += sec;
			}
			rc->rc_devidx = (uint32_t)child;
			rc->rc_offset = coff;
			if (scratch)
				rc->rc_offset -= VDEV_BOOT_SIZE;
			if (c < np)
				rc->rc_size = (size_t)sec;
			else if (row == rows - 1 && rm->rm_bc != 0 &&
			    c >= rm->rm_bc)
				rc->rc_size = 0;
			else {
				if (c < rm->rm_bc || r == 0)
					off = dc * rows + row;
				else
					off = r * rows + (dc - r) *
					    (rows - 1) + row;
				if (off >= rm->rm_s)
					return (EINVAL);
				rc->rc_size = (size_t)sec;
				rr->rr_off[c] = (size_t)off << ashift;
			}
		}
	}

	/*
	 * [Z] both functions: single parity trades its first two columns
	 * in every other megabyte of the vdev, a layout choice the
	 * comment in vdev_raidz_map_alloc() calls "an implicit on-disk
	 * format requirement that we need to support for all eternity,
	 * but only for single-parity RAID-Z".  Only where they are, not
	 * their sizes, which are the same.
	 */
	if (np == 1 && rr->rr_cols > 1 && (rm->rm_offset & (1ULL << 20))) {
		struct raidz_col t = rr->rr_col[0];

		rr->rr_col[0].rc_devidx = rr->rr_col[1].rc_devidx;
		rr->rr_col[0].rc_offset = rr->rr_col[1].rc_offset;
		rr->rr_col[1].rc_devidx = t.rc_devidx;
		rr->rr_col[1].rc_offset = t.rc_offset;
	}
	return (0);
}

/*
 * The caller's buffer holds only the block, and the I/O is whole
 * sectors, so the last sector of the block -- the only one with
 * padding in it -- is read into tail instead.  These address byte i of
 * data column c of a row wherever it is kept.
 */
struct raidz_data {
	uint8_t		*rd_buf;	/* the block, rd_len bytes */
	size_t		rd_len;
	uint8_t		*rd_tail;	/* the block's last sector */
	size_t		rd_tailpos;	/* where that sector starts in it */
};

static uint8_t *
raidz_byte(const struct raidz_data *rd, const struct raidz_row *rr,
    uint64_t c, size_t i)
{
	size_t pos = rr->rr_off[c] + i;

	if (pos >= rd->rd_tailpos)
		return (&rd->rd_tail[pos - rd->rd_tailpos]);
	return (&rd->rd_buf[pos]);
}

/* The part of the tail that belongs to the block, into the block. */
static void
raidz_tail_copy(const struct raidz_data *rd)
{
	size_t i;

	for (i = 0; rd->rd_tailpos + i < rd->rd_len; i++)
		rd->rd_buf[rd->rd_tailpos + i] = rd->rd_tail[i];
}

/*
 * Read data column c of a row into its place.  Columns are whole
 * sectors, so one that reaches the tail ends with it.
 */
static int
raidz_data_read(const struct zfs_top *tv, const struct raidz_row *rr,
    const struct raidz_data *rd, uint64_t c)
{
	const struct raidz_col *rc = &rr->rr_col[c];
	struct raidz_col part = *rc;
	size_t off = rr->rr_off[c];
	int err;

	if (rc->rc_size == 0)
		return (0);
	if (off + rc->rc_size <= rd->rd_tailpos)
		return (raidz_col_read(tv, rc, rd->rd_buf + off));
	part.rc_size = rd->rd_tailpos - off;
	if (part.rc_size > 0 &&
	    (err = raidz_col_read(tv, &part, rd->rd_buf + off)) != 0)
		return (err);
	part.rc_offset = rc->rc_offset + part.rc_size;
	part.rc_size = rc->rc_size - part.rc_size;
	if ((err = raidz_col_read(tv, &part, rd->rd_tail)) != 0)
		return (err);
	raidz_tail_copy(rd);
	return (0);
}

/*
 * GF(2^8) for raidz's Q and R, as the block comment at the top of [Z]
 * module/zfs/vdev_raidz.c defines it: addition is XOR, and
 * multiplication by 2 is ((a << 1) ^ ((a & 0x80) ? 0x1d : 0)), from the
 * primitive polynomial x^8 + x^4 + x^3 + x^2 + 1.  2 generates the
 * field, so the tables of its powers and logs are built from that one
 * definition rather than carried.
 */
static uint8_t gf_pow2[255], gf_log2[256];

static void
gf_init(void)
{
	unsigned i, a = 1;

	if (gf_pow2[0] != 0)
		return;
	for (i = 0; i < 255; i++) {
		gf_pow2[i] = (uint8_t)a;
		gf_log2[a] = (uint8_t)i;
		a = ((a << 1) ^ ((a & 0x80) ? 0x1d : 0)) & 0xff;
	}
}

static uint8_t
gf_mul(uint8_t a, uint8_t b)
{

	if (a == 0 || b == 0)
		return (0);
	return (gf_pow2[(gf_log2[a] + gf_log2[b]) % 255]);
}

static uint8_t
gf_inv(uint8_t a)		/* a != 0 */
{

	return (gf_pow2[(255 - gf_log2[a]) % 255]);
}

/*
 * [Z] the same comment: over data columns D_0 .. D_n-1,
 *	P = D_0 + ... + D_n-1
 *	Q = 2^(n-1) D_0 + ... + 2^0 D_n-1
 *	R = 4^(n-1) D_0 + ... + 4^0 D_n-1
 * so parity p (0, 1, 2) weighs data column i by 2^(p * (n - 1 - i)).
 */
static uint8_t
raidz_coef(unsigned p, uint64_t ndata, uint64_t i)
{

	return (gf_pow2[(p * (ndata - 1 - i)) % 255]);
}

/*
 * Invert a k x k matrix over GF(2^8) in place, k <= 3.  Returns 0 if it
 * is singular, which for these coefficients it is not ([Z] the same
 * comment on why 1, 2 and 4 were chosen), but a disk is not a proof.
 */
static int
gf_invert(uint8_t m[3][3], int k, uint8_t inv[3][3])
{
	int i, j, r;

	for (i = 0; i < k; i++)
		for (j = 0; j < k; j++)
			inv[i][j] = (i == j);
	for (i = 0; i < k; i++) {
		uint8_t piv, t;

		for (r = i; r < k && m[r][i] == 0; r++)
			continue;
		if (r == k)
			return (0);
		for (j = 0; j < k; j++) {
			t = m[i][j]; m[i][j] = m[r][j]; m[r][j] = t;
			t = inv[i][j]; inv[i][j] = inv[r][j]; inv[r][j] = t;
		}
		piv = gf_inv(m[i][i]);
		for (j = 0; j < k; j++) {
			m[i][j] = gf_mul(m[i][j], piv);
			inv[i][j] = gf_mul(inv[i][j], piv);
		}
		for (r = 0; r < k; r++) {
			uint8_t f = m[r][i];

			if (r == i || f == 0)
				continue;
			for (j = 0; j < k; j++) {
				m[r][j] ^= gf_mul(f, m[i][j]);
				inv[r][j] ^= gf_mul(f, inv[i][j]);
			}
		}
	}
	return (1);
}

/*
 * Not popcount32: NetBSD's <string.h> declares one returning unsigned
 * int, and the test drivers include this file after it.
 */
static int
raidz_popcount(uint32_t v)
{
	int n = 0;

	for (; v != 0; v &= v - 1)
		n++;
	return (n);
}

/*
 * Rebuild the data columns of a row in tset from its parity columns in
 * pset, one byte position at a time: for each parity p in pset its
 * syndrome is the parity byte less every data column not in tset,
 * weighted, and the bytes of tset solve those equations.  A short
 * column is zero past its end, both where it is read and where it is
 * solved for.
 */
static void
raidz_solve(const struct raidz_row *rr, uint64_t np,
    const struct raidz_data *rd, const uint8_t *par, size_t psize,
    uint32_t tset, unsigned pset)
{
	const struct raidz_col *col = rr->rr_col;
	uint8_t m[3][3], inv[3][3], syn[3];
	uint64_t ndata = rr->rr_cols - np, t[3], c;
	unsigned pl[3];
	size_t j;
	int k = 0, pk = 0, a, bb;

	for (c = np; c < rr->rr_cols; c++)
		if (tset & ((uint32_t)1 << c))
			t[k++] = c;
	for (a = 0; a < 3; a++)
		if (pset & (1u << a))
			pl[pk++] = (unsigned)a;
	for (a = 0; a < k; a++)
		for (bb = 0; bb < k; bb++)
			m[a][bb] = raidz_coef(pl[a], ndata, t[bb] - np);
	if (!gf_invert(m, k, inv))
		return;

	for (j = 0; j < psize; j++) {
		for (a = 0; a < k; a++) {
			uint8_t v = par[pl[a] * psize + j];

			for (c = np; c < rr->rr_cols; c++) {
				if ((tset & ((uint32_t)1 << c)) ||
				    j >= col[c].rc_size)
					continue;
				v ^= gf_mul(raidz_coef(pl[a], ndata, c - np),
				    *raidz_byte(rd, rr, c, j));
			}
			syn[a] = v;
		}
		for (bb = 0; bb < k; bb++) {
			uint8_t x = 0;

			if (j >= col[t[bb]].rc_size)
				continue;
			for (a = 0; a < k; a++)
				x ^= gf_mul(inv[bb][a], syn[a]);
			*raidz_byte(rd, rr, t[bb], j) = x;
		}
	}
}

/*
 * [Z] vdev_raidz.c, raidz_simulate_failure(): whether a column is on
 * logical child i.  The first physical-width values of i are today's
 * children; the next physical-width - 1 are the children as they were
 * before the last widening, and so on back to the original width.  A
 * child that returned wrong data before a widening had it moved,
 * unchecked, diagonally across the new children, and this is where it
 * went.  On a raidz never widened it is just rc_devidx == i.
 */
static int
raidz_on_child(uint64_t phys, uint64_t orig, uint64_t ashift, uint64_t i,
    const struct raidz_col *rc)
{
	uint64_t sector = phys * (rc->rc_offset >> ashift) + rc->rc_devidx;
	uint64_t w;

	for (w = phys; w >= orig && w > 0; w--) {
		if (i < w)
			return (sector % w == i);
		i -= w;
	}
	return (0);
}

/*
 * The columns of a row taken to be bad: those that could not be read,
 * and those on any of the k logical children in ids.
 */
static uint32_t
raidz_row_bad(const struct zfs_top *tv, const struct raidz_row *rr,
    uint32_t miss, const uint64_t *ids, int k)
{
	uint64_t orig = raidz_original_width(tv), c;
	uint32_t bad = miss;
	int t;

	for (c = 0; c < rr->rr_cols; c++) {
		if (rr->rr_col[c].rc_size == 0)
			continue;
		for (t = 0; t < k; t++)
			if (raidz_on_child(tv->tv_nchildren, orig,
			    tv->tv_ashift, ids[t], &rr->rr_col[c])) {
				bad |= (uint32_t)1 << c;
				break;
			}
	}
	return (bad);
}

/* The next k-subset of 0 .. n - 1 in order; 0 when there is none. */
static int
raidz_next_ids(uint64_t *ids, int k, uint64_t n)
{
	int i, j;

	for (i = k - 1; i >= 0 && ids[i] == n - (uint64_t)(k - i); i--)
		continue;
	if (i < 0)
		return (0);
	ids[i]++;
	for (j = i + 1; j < k; j++)
		ids[j] = ids[j - 1] + 1;
	return (1);
}

/* The data columns of a row, as a mask. */
static uint32_t
raidz_dmask(const struct raidz_row *rr, uint64_t np)
{

	return ((((uint32_t)1 << rr->rr_cols) - 1) &
	    ~(((uint32_t)1 << np) - 1));
}

/*
 * Some columns could not be read, or the block failed its checksum and
 * any of them may be wrong.  [Z] vdev_raidz.c, vdev_raidz_combrec() and
 * raidz_reconstruct(): take 1, then 2, up to np logical children to
 * have failed, every combination in turn, together with every column
 * that could not be read; a row with more bad columns than parity
 * cannot be rebuilt under that guess.  Every row's bad data columns are
 * rebuilt from as many of its good parity columns, and the whole block
 * checked.  How [Z] solves a row is its own business; this solves the
 * equations of the comment above directly.
 *
 * The first guess is that nothing failed but what could not be read,
 * which is the whole story when a disk is simply missing; [Z] does that
 * first too (vdev_raidz_io_done_reconstruct_known_missing()).  A later
 * guess that marks nothing beyond the unreadable columns, in any row
 * with a data column to rebuild, would rebuild the same block again,
 * and is not checked twice.  A parity column it marks does count: the
 * row is then solved from other parity, which is the whole point when
 * the bad column is P on a disk that reads but returns the wrong thing.
 */
static int
raidz_rebuild(const struct zfs_top *tv, const struct raidz_map *rm,
    const struct raidz_data *rd, uint32_t *miss, zfs_verifyfn_t verify,
    void *arg)
{
	struct raidz_row rr;
	uint64_t np = rm->rm_np, row, n, w, ids[3], c;
	size_t psize = rm->rm_psize, plen;
	uint8_t *par;
	uint32_t bad, dmask;
	unsigned pset;
	int k, t, err = EIO, fits, fresh;

	gf_init();
	for (n = 0, w = tv->tv_nchildren; w >= raidz_original_width(tv) &&
	    w > 0; w--)
		n += w;

	plen = (size_t)(rm->rm_rows * np) * psize;
	if ((par = zfs_scratch_get(plen)) == NULL)
		return (ENOMEM);
	for (row = 0; row < rm->rm_rows; row++) {
		if (raidz_map_row(rm, row, &rr) != 0) {
			err = EINVAL;
			goto out;
		}
		for (c = 0; c < np; c++)
			if (raidz_col_read(tv, &rr.rr_col[c],
			    par + (row * np + c) * psize) != 0)
				miss[row] |= (uint32_t)1 << c;
		if (raidz_popcount(miss[row]) > (int)np)
			goto out;
	}

	for (k = 0; k <= (int)np && (uint64_t)k <= n; k++) {
		for (t = 0; t < k; t++)
			ids[t] = (uint64_t)t;
		do {
			/* Can every row be rebuilt, and is it new? */
			fits = 1;
			fresh = 0;
			for (row = 0; row < rm->rm_rows && fits; row++) {
				(void)raidz_map_row(rm, row, &rr);
				dmask = raidz_dmask(&rr, np);
				bad = raidz_row_bad(tv, &rr, miss[row], ids, k);
				if (raidz_popcount(bad) > (int)np)
					fits = 0;
				else if ((bad & dmask) != 0 &&
				    (k == 0 || (bad & ~miss[row]) != 0))
					fresh = 1;
			}
			if (!fits || !fresh)
				continue;

			for (row = 0; row < rm->rm_rows; row++) {
				(void)raidz_map_row(rm, row, &rr);
				dmask = raidz_dmask(&rr, np);
				bad = raidz_row_bad(tv, &rr, miss[row], ids, k);
				if ((bad & dmask) == 0)
					continue;
				/* As many good parity columns, P first. */
				pset = 0;
				for (c = 0; c < np; c++)
					if (!(bad & ((uint32_t)1 << c)) &&
					    raidz_popcount(pset) <
					    raidz_popcount(bad & dmask))
						pset |= 1u << c;
				raidz_solve(&rr, np, rd,
				    par + row * np * psize, psize,
				    bad & dmask, pset);
			}
			raidz_tail_copy(rd);
			if (verify(arg, rd->rd_buf, rd->rd_len)) {
				err = 0;
				goto out;
			}
			err = EINVAL;
			/* Put back what was read, for the next guess. */
			for (row = 0; row < rm->rm_rows; row++) {
				(void)raidz_map_row(rm, row, &rr);
				bad = raidz_row_bad(tv, &rr, miss[row], ids,
				    k) & raidz_dmask(&rr, np) & ~miss[row];
				for (c = np; c < rr.rr_cols; c++)
					if (bad & ((uint32_t)1 << c))
						(void)raidz_data_read(tv, &rr,
						    rd, c);
			}
		} while (k > 0 && raidz_next_ids(ids, k, n));
	}
out:
	zfs_scratch_put(par, plen);
	return (err);
}

static int
raidz_read(struct zfs_pool *pool, const struct zfs_top *tv, const dva_t *dva,
    uint64_t birth, uint8_t *buf, size_t len, zfs_verifyfn_t verify,
    void *arg)
{
	struct raidz_map rm;
	struct raidz_row rr;
	struct raidz_data rd;
	uint64_t width, row, c;
	size_t sec = (size_t)1 << tv->tv_ashift, mlen;
	uint32_t *miss;
	uint8_t *tail;
	int bad = 0, err;

	width = raidz_width(tv, birth);
	if (width <= tv->tv_nparity || width > tv->tv_nchildren || len == 0)
		return (EINVAL);
	raidz_map_init(&rm, pool, tv, DVA_GET_OFFSET(dva), len, width);
	if (rm.rm_rows > SPA_LARGE_MAXBLOCKSIZE >> SPA_MINBLOCKSHIFT)
		return (EINVAL);

	if ((tail = zfs_scratch_get(sec)) == NULL)
		return (ENOMEM);
	mlen = (size_t)rm.rm_rows * sizeof(*miss);
	if ((miss = zfs_scratch_get(mlen)) == NULL) {
		zfs_scratch_put(tail, sec);
		return (ENOMEM);
	}
	rd.rd_buf = buf;
	rd.rd_len = len;
	rd.rd_tail = tail;
	rd.rd_tailpos = (size_t)(rm.rm_s - 1) << tv->tv_ashift;

	/* Every data column as it is on the disks. */
	for (row = 0; row < rm.rm_rows; row++) {
		miss[row] = 0;
		if (raidz_map_row(&rm, row, &rr) != 0) {
			err = EINVAL;
			goto out;
		}
		for (c = tv->tv_nparity; c < rr.rr_cols; c++)
			if (raidz_data_read(tv, &rr, &rd, c) != 0) {
				miss[row] |= (uint32_t)1 << c;
				bad = 1;
			}
	}
	if (!bad && verify(arg, buf, len)) {
		err = 0;
		goto out;
	}
	err = raidz_rebuild(tv, &rm, &rd, miss, verify, arg);
out:
	zfs_scratch_put(miss, mlen);
	zfs_scratch_put(tail, sec);
	return (err);
}

static int
vdev_read(struct zfs_pool *pool, const dva_t *dva, uint64_t birth, void *buf,
    size_t len, zfs_verifyfn_t verify, void *arg)
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
	case ZFS_VT_RAIDZ:
		return (raidz_read(pool, tv, dva, birth, buf, len, verify,
		    arg));
	case ZFS_VT_NONE:
		/* On a device that was not found. */
		return (ENXIO);
	default:
		return (ENOTSUP);
	}
}

struct bp_verify {
	const struct zfs_pool	*bv_pool;
	const blkptr_t		*bv_bp;
};

static int
bp_verify(void *arg, const void *buf, size_t len)
{
	const struct bp_verify *bv = arg;

	return (block_checksum_ok(bv->bv_pool, bv->bv_bp, buf, len));
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
	err = vdev_read(pool, &bp->blk_dva[d], BP_GET_PHYSICAL_BIRTH(bp), hdr,
	    bufsize, gang_verify, &gv);
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

	if (!DVA_GET_GANG(dva)) {
		struct bp_verify bv = { pool, bp };

		return (vdev_read(pool, dva, BP_GET_PHYSICAL_BIRTH(bp), out,
		    psize, bp_verify, &bv));
	}

	/*
	 * A gang block's members were each checked as they were read,
	 * and each from whichever copy passed; the whole is checked
	 * once it is assembled.
	 */
	err = gang_read(pool, bp, d, out, psize, depth);
	if (err != 0)
		return (err);
	if (!block_checksum_ok(pool, bp, out, psize))
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
	if (buflen < lsize || psize > SPA_LARGE_MAXBLOCKSIZE)
		return (EINVAL);
	if ((raw = zfs_scratch_get(psize)) == NULL)
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
	zfs_scratch_put(raw, psize);
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
	    dn->dn_datablkszsec > SPA_LARGE_MAXBLOCKSIZE / 512)
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
 * with parity, and an offset lands somewhere else on each of them.
 * [S] names raidz (§1.3.3, Table 2), counts its parity in a DVA's asize
 * (§2.6) and reserves the DVA's GRID field for "Raid-Z layout
 * information" (§2.1), but never says what the layout is; raidz_read()
 * takes it from OpenZFS.
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
	if (nv_streq(v, VDEV_TYPE_RAIDZ))
		return (ZFS_VT_RAIDZ);
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
	int type, err;

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
		if (type == ZFS_VT_RAIDZ) {
			/*
			 * [Z] module/zfs/vdev_raidz.c, vdev_raidz_init()
			 * (openzfs/zfs 81b19c6): nparity is 1 to
			 * VDEV_RAIDZ_MAXPARITY, and a label without one
			 * is from a pool older than SPA_VERSION_RAIDZ2,
			 * which had single parity only.
			 */
			if (nvlist_find_nested(tree->nv_list,
			    tree->nv_listlen, ZPOOL_CONFIG_NPARITY,
			    NV_WANT_UINT64, &v) != 0)
				v.nv_u64 = 1;
			if (v.nv_u64 < 1 || v.nv_u64 > VDEV_RAIDZ_MAXPARITY)
				return (EINVAL);
			tv->tv_nparity = (uint32_t)v.nv_u64;
			/*
			 * [Z] vdev_raidz.c, vdev_raidz_init(): a raidz
			 * that has been widened lists in raidz_expand_txgs
			 * the txg each widening finished in, and one being
			 * widened carries the boolean raidz_expanding.
			 * [Z] vdev_raidz_config_generate() writes the txgs
			 * in ascending order, out of an AVL tree that
			 * would not hold two the same; anything else is
			 * not a label OpenZFS wrote.
			 */
			tv->tv_nexpand = 0;
			tv->tv_expanding = 0;
			err = nvlist_find_nested(tree->nv_list,
			    tree->nv_listlen, ZPOOL_CONFIG_RAIDZ_EXPAND_TXGS,
			    NV_WANT_UINT64_ARRAY, &v);
			if (err == 0) {
				if (v.nv_nelem > ZFS_MAX_CHILDREN)
					return (ENOTSUP);
				for (i = 0; i < v.nv_nelem; i++) {
					tv->tv_expand_txg[i] =
					    nvlist_u64_elem(&v, i);
					if (i > 0 && tv->tv_expand_txg[i] <=
					    tv->tv_expand_txg[i - 1])
						return (EINVAL);
				}
				tv->tv_nexpand = v.nv_nelem;
			} else if (err != ENOENT)
				return (EINVAL);
			err = nvlist_find_nested(tree->nv_list,
			    tree->nv_listlen, ZPOOL_CONFIG_RAIDZ_EXPANDING,
			    NV_WANT_BOOLEAN, &v);
			if (err == 0)
				tv->tv_expanding = 1;
			else if (err != ENOENT)
				return (EINVAL);
		}
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
			/*
			 * A raidz needs a data column beside its parity,
			 * at every width it has had; raidz_read() divides
			 * by the difference.  The narrowest is the width
			 * it was made with, [Z] vdev_raidz_init()'s
			 * vd_original_width.
			 */
			if (type == ZFS_VT_RAIDZ &&
			    ch.nv_nelem <= tv->tv_nparity + tv->tv_nexpand +
			    (uint32_t)tv->tv_expanding)
				return (EINVAL);
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
