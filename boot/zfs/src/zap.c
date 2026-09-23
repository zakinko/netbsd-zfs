/*	$NetBSD$	*/

/*
 * The ZAP: name to value lookup.
 *
 * [S] chapter five.  Every constant here carries its section; the one
 * thing chapter five does not contain is the hash function itself, and
 * where that comes from is set out at zap_hash below.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "zfs_ondisk.h"
#include "zfsread.h"
#include "scratch.h"

/*
 * [Z] zap_impl.c, as zfs_ondisk.h records: a reflected CRC-64 over
 * ECMA-182's polynomial, seeded with the ZAP's salt.
 *
 * The table is built on first use rather than held as 2KB of constants,
 * because the bootloader's heap is cheaper than its image.  It is
 * checked against the property the polynomial's reflected form has --
 * table[128] is the polynomial -- so a table that did not get built
 * cannot be used silently.  A table of zeros hashes every name to the
 * salt, and every lookup then fails with ENOENT, which reads exactly
 * like an empty pool.
 */
static uint64_t crc64_table[256];
static int crc64_ready;

static void
crc64_init(void)
{
	uint64_t c;
	int i, j;

	for (i = 0; i < 256; i++) {
		c = (uint64_t)i;
		for (j = 0; j < 8; j++)
			c = (c >> 1) ^ (-(int64_t)(c & 1) & ZFS_CRC64_POLY);
		crc64_table[i] = c;
	}
	crc64_ready = (crc64_table[128] == ZFS_CRC64_POLY);
}

static uint64_t
zap_hash(uint64_t salt, const char *name)
{
	uint64_t h = salt;
	const uint8_t *p;

	if (!crc64_ready)
		crc64_init();

	/*
	 * [Z]: the terminating NUL is stored on disk but is not hashed.
	 */
	for (p = (const uint8_t *)name; *p != '\0'; p++)
		h = (h >> 8) ^ crc64_table[(h ^ *p) & 0xff];

	/*
	 * The low bits are left clear for the collision differentiator,
	 * so that the bucket comes from the top of the word.
	 */
	return (h & ~(((uint64_t)1 << (64 - ZAP_HASHBITS)) - 1));
}

static int
streq(const char *a, const char *b, size_t max)
{
	size_t i;

	for (i = 0; i < max; i++) {
		if (a[i] != b[i])
			return (0);
		if (a[i] == '\0')
			return (1);
	}
	return (0);
}

/*
 * [S] §5.1: one block, a 128 byte header whose last 64 bytes are the
 * first entry, then entries of 64 bytes.  Names are at most 50
 * characters and values are single 64 bit integers, so there is nothing
 * to hash: the entries are compared in order.
 */
static int
mzap_lookup(const void *blk, size_t blksize, const char *name, uint64_t *val)
{
	const mzap_phys_t *mz = blk;
	size_t n, i;

	n = (blksize - MZAP_HDR_LEN) / MZAP_ENT_LEN + 1;
	for (i = 0; i < n; i++) {
		const mzap_ent_phys_t *e = &mz->mz_chunk[i];

		if (e->mze_name[0] == '\0')
			continue;
		if (streq(e->mze_name, name, MZAP_NAME_LEN)) {
			*val = e->mze_value;
			return (0);
		}
	}
	return (ENOENT);
}

/*
 * [S] §5.2.4: a name or a value is held in a chain of 21 byte arrays.
 */
static int
leaf_array_get(const void *leaf, int bs, uint16_t chunk, void *out,
    size_t len)
{
	const zap_leaf_array_t *ch;
	const uint8_t *chunks;
	uint8_t *p = out;
	size_t i;

	chunks = (const uint8_t *)leaf + 2 * ZAP_LEAF_CHUNKSIZE +
	    2 * ZAP_LEAF_HASH_NUMENTRIES(bs);

	while (len > 0) {
		if (chunk == ZAP_LEAF_CHAIN_END ||
		    chunk >= ZAP_LEAF_NUMCHUNKS(bs))
			return (EINVAL);
		ch = (const void *)(chunks + (size_t)chunk *
		    ZAP_LEAF_CHUNKSIZE);
		if (ch->la_type != ZAP_LEAF_ARRAY)
			return (EINVAL);
		for (i = 0; i < ZAP_LEAF_ARRAY_BYTES && len > 0; i++, len--)
			*p++ = ch->la_array[i];
		chunk = ch->la_next;
	}
	return (0);
}

/*
 * [S] §5.2.3: the bits of the hash below the prefix that picked this
 * leaf index the leaf's hash table; each bucket holds the index of a
 * chunk, and collisions chain through le_next.
 *
 * [S] says "twelve bits", which is the width of a 128K leaf's table --
 * 4096 entries of two bytes is the "next 8KB" the same section
 * describes.  A leaf is as large as the ZAP object's data blocks, and
 * this pool's are 16K, whose table is 512 entries and nine bits.  So the
 * width is ZAP_LEAF_HASH_SHIFT of the block, and [S]'s twelve is the
 * case bs = 17 of it.
 */
static int
zap_leaf_lookup(const void *leaf, int bs, uint64_t hash, const char *name,
    uint64_t *val)
{
	const zap_leaf_header_t *lh = leaf;
	const uint16_t *lhash;
	const uint8_t *chunks;
	uint16_t chunk;
	char found[ZAP_MAXNAMELEN];

	if (lh->lh_block_type != ZBT_LEAF || lh->lh_magic != ZAP_LEAF_MAGIC)
		return (EINVAL);

	lhash = (const uint16_t *)((const uint8_t *)leaf +
	    2 * ZAP_LEAF_CHUNKSIZE);
	chunks = (const uint8_t *)lhash + 2 * ZAP_LEAF_HASH_NUMENTRIES(bs);

	chunk = lhash[(hash >> (64 - lh->lh_prefix_len -
	    ZAP_LEAF_HASH_SHIFT(bs))) & (ZAP_LEAF_HASH_NUMENTRIES(bs) - 1)];

	while (chunk != ZAP_LEAF_CHAIN_END) {
		const zap_leaf_entry_t *e;

		if (chunk >= ZAP_LEAF_NUMCHUNKS(bs))
			return (EINVAL);
		e = (const void *)(chunks + (size_t)chunk *
		    ZAP_LEAF_CHUNKSIZE);
		if (e->le_type != ZAP_LEAF_ENTRY)
			return (EINVAL);

		if (e->le_hash == hash &&
		    e->le_name_length <= sizeof(found) &&
		    leaf_array_get(leaf, bs, e->le_name_chunk, found,
		    e->le_name_length) == 0 &&
		    streq(found, name, e->le_name_length)) {
			uint8_t v[8];
			int i;

			/*
			 * [S] §5.2.4: integers in a leaf array are big
			 * endian whatever the machine, which is the one
			 * place in the format where that is so.  Only a
			 * single 64 bit value is wanted here.
			 */
			if (e->le_int_size != 8 || e->le_value_length != 1)
				return (EINVAL);
			if (leaf_array_get(leaf, bs, e->le_value_chunk, v,
			    sizeof(v)) != 0)
				return (EINVAL);
			*val = 0;
			for (i = 0; i < 8; i++)
				*val = (*val << 8) | v[i];
			return (0);
		}
		chunk = e->le_next;
	}
	return (ENOENT);
}

/*
 * Look one name up in a ZAP object.
 *
 * [S] chapter five: the first word of the object's first block says
 * which of the two kinds it is.
 */
static int
zap_lookup1(struct zfs_pool *pool, const dnode_phys_t *dn, const char *name,
    uint64_t *val, uint8_t *blk, uint8_t *leaf)
{
	const zap_phys_t *zp;
	uint64_t hash, blkid, *ptrtbl;
	size_t blksize;
	int bs, err;

	blksize = (size_t)dn->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (blksize == 0 || blksize > ZFS_MAXBLOCKSIZE)
		return (EINVAL);

	for (bs = 0; ((size_t)1 << bs) < blksize; bs++)
		continue;

	err = zfs_read_object_block(pool, dn, 0, blk, ZFS_MAXBLOCKSIZE);
	if (err != 0)
		return (err);

	switch (*(const uint64_t *)(const void *)blk) {
	case ZBT_MICRO:
		return (mzap_lookup(blk, blksize, name, val));
	case ZBT_HEADER:
		break;
	default:
		return (EINVAL);
	}

	zp = (const void *)blk;
	if (zp->zap_magic != ZAP_MAGIC)
		return (EINVAL);
	hash = zap_hash(zp->zap_salt, name);

	/*
	 * [S] §5.2.2: the prefix -- the zt_shift high bits of the hash --
	 * indexes the pointer table, whose entries are level 0 block ids.
	 *
	 * [S] §5.2.1 puts the table inside this block when it is small
	 * enough, in the zap_leafs array, declared there as 8192 entries
	 * after 8181 of padding -- which is the second half of a 128K
	 * block, and only of a 128K block.
	 *
	 * [Z] zap_impl.h says the rule the declaration is an instance of:
	 * "The embedded pointer table starts half-way through the block.
	 * Since the pointer table itself is half the block, it starts at
	 * (64-bit) word number (1<<ZAP_EMBEDDED_PTRTBL_SHIFT(zap))",
	 * which is block size / 8 / 2.  So it is the second half at any
	 * block size, and that is how it is addressed here.
	 */
	if (zp->zap_ptrtbl.zt_numblks != 0) {
		/*
		 * An external table means the ZAP outgrew its first
		 * block.  Nothing on the path to a kernel is that large,
		 * and reading it untested would be worse than saying so.
		 */
		return (ENOTSUP);
	}
	ptrtbl = (uint64_t *)(void *)(blk + blksize / 2);
	blkid = ptrtbl[hash >> (64 - zp->zap_ptrtbl.zt_shift)];

	err = zfs_read_object_block(pool, dn, blkid, leaf, ZFS_MAXBLOCKSIZE);
	if (err != 0)
		return (err);

	return (zap_leaf_lookup(leaf, bs, hash, name, val));
}

/*
 * Every name in a ZAP object, for the loader's "ls".
 *
 * [S] does not describe iteration, only lookup, but the structures say
 * how: a microzap is an array to walk, and a fatzap's entries all live
 * in leaves, which the pointer table names.  Several buckets can name
 * the same leaf -- [S] §5.2.3's lh_prefix_len may be shorter than
 * zt_shift -- so a leaf already visited is skipped.
 */
static int
zap_list1(struct zfs_pool *pool, const dnode_phys_t *dn,
    void (*fn)(void *, const char *, uint64_t), void *arg,
    uint8_t *blk, uint8_t *leaf)
{
	const zap_phys_t *zp;
	size_t blksize;
	uint64_t prev = (uint64_t)-1;
	int bs, err;
	uint64_t i, n;

	blksize = (size_t)dn->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (blksize == 0 || blksize > ZFS_MAXBLOCKSIZE)
		return (EINVAL);

	for (bs = 0; ((size_t)1 << bs) < blksize; bs++)
		continue;

	err = zfs_read_object_block(pool, dn, 0, blk, ZFS_MAXBLOCKSIZE);
	if (err != 0)
		return (err);

	if (*(const uint64_t *)(const void *)blk == ZBT_MICRO) {
		const mzap_phys_t *mz = (const void *)blk;

		n = (blksize - MZAP_HDR_LEN) / MZAP_ENT_LEN + 1;
		for (i = 0; i < n; i++) {
			const mzap_ent_phys_t *e = &mz->mz_chunk[i];

			if (e->mze_name[0] != '\0')
				fn(arg, e->mze_name, e->mze_value);
		}
		return (0);
	}

	zp = (const void *)blk;
	if (zp->zap_block_type != ZBT_HEADER || zp->zap_magic != ZAP_MAGIC)
		return (EINVAL);
	if (zp->zap_ptrtbl.zt_numblks != 0)
		return (ENOTSUP);

	n = (uint64_t)1 << zp->zap_ptrtbl.zt_shift;
	for (i = 0; i < n; i++) {
		const uint64_t *ptrtbl = (const void *)(blk + blksize / 2);
		const zap_leaf_header_t *lh;
		const uint8_t *chunks;
		uint64_t blkid = ptrtbl[i];
		int c;

		if (blkid == prev || blkid == 0)
			continue;
		prev = blkid;

		err = zfs_read_object_block(pool, dn, blkid, leaf,
		    ZFS_MAXBLOCKSIZE);
		if (err != 0)
			return (err);
		lh = (const void *)leaf;
		if (lh->lh_block_type != ZBT_LEAF ||
		    lh->lh_magic != ZAP_LEAF_MAGIC)
			return (EINVAL);
		chunks = leaf + 2 * ZAP_LEAF_CHUNKSIZE +
		    2 * ZAP_LEAF_HASH_NUMENTRIES(bs);

		for (c = 0; c < (int)ZAP_LEAF_NUMCHUNKS(bs); c++) {
			const zap_leaf_entry_t *e = (const void *)(chunks +
			    (size_t)c * ZAP_LEAF_CHUNKSIZE);
			char name[ZAP_MAXNAMELEN];
			uint8_t v[8];
			uint64_t val = 0;
			int j;

			if (e->le_type != ZAP_LEAF_ENTRY)
				continue;
			if (e->le_name_length > sizeof(name))
				continue;
			if (leaf_array_get(leaf, bs, e->le_name_chunk, name,
			    e->le_name_length) != 0)
				continue;
			if (e->le_int_size == 8 && e->le_value_length == 1 &&
			    leaf_array_get(leaf, bs, e->le_value_chunk, v,
			    sizeof(v)) == 0) {
				for (j = 0; j < 8; j++)
					val = (val << 8) | v[j];
			}
			fn(arg, name, val);
		}
	}
	return (0);
}

/*
 * Both of the above want two blocks in hand at once, and both have
 * several ways out.  Rather than free the scratch on each of them --
 * which is how an arena gets left shifted by one missed path -- the
 * space is taken and given back here, around a single call.
 */
int
zap_lookup(struct zfs_pool *pool, const dnode_phys_t *dn, const char *name,
    uint64_t *val)
{
	uint8_t *blk, *leaf;
	int err;

	if ((blk = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL)
		return (ENOMEM);
	if ((leaf = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL) {
		zfs_scratch_put(blk, ZFS_MAXBLOCKSIZE);
		return (ENOMEM);
	}

	err = zap_lookup1(pool, dn, name, val, blk, leaf);

	zfs_scratch_put(leaf, ZFS_MAXBLOCKSIZE);
	zfs_scratch_put(blk, ZFS_MAXBLOCKSIZE);
	return (err);
}

int
zap_list(struct zfs_pool *pool, const dnode_phys_t *dn,
    void (*fn)(void *, const char *, uint64_t), void *arg)
{
	uint8_t *blk, *leaf;
	int err;

	if ((blk = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL)
		return (ENOMEM);
	if ((leaf = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL) {
		zfs_scratch_put(blk, ZFS_MAXBLOCKSIZE);
		return (ENOMEM);
	}

	err = zap_list1(pool, dn, fn, arg, blk, leaf);

	zfs_scratch_put(leaf, ZFS_MAXBLOCKSIZE);
	zfs_scratch_put(blk, ZFS_MAXBLOCKSIZE);
	return (err);
}
