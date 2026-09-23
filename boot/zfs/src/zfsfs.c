/*	$NetBSD$	*/

/*
 * From a dataset name to a file's bytes.
 *
 * [S] chapter four for the dataset, chapter six for what is inside it.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "zfs_ondisk.h"
#include "zfsread.h"
#include "scratch.h"

/*
 * zfs_mount needs the two object sets only while it is running: what it
 * keeps afterwards is the metadnode, copied into the caller's struct.
 * So these come from the arena like everything else.
 */

static int zfs_mount1(struct zfs_pool *, const char *, struct zfs_dataset *,
    objset_phys_t *, objset_phys_t *);
static int mount_dataset(struct zfs_pool *, objset_phys_t *, objset_phys_t *,
    uint64_t, struct zfs_dataset *);

/*
 * [S] §4.2: the MOS is what the uberblock points at, and its object 1
 * is the object directory, from which everything else is reached.
 */
int
zfs_mount(struct zfs_pool *pool, const char *dsname,
    struct zfs_dataset *dsp)
{
	objset_phys_t *mos, *os;
	uint8_t *mos_buf, *os_buf;
	int err;

	if ((mos_buf = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL)
		return (ENOMEM);
	if ((os_buf = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL) {
		zfs_scratch_put(mos_buf, ZFS_MAXBLOCKSIZE);
		return (ENOMEM);
	}
	mos = (void *)mos_buf;
	os = (void *)os_buf;

	err = zfs_mount1(pool, dsname, dsp, mos, os);

	zfs_scratch_put(os_buf, ZFS_MAXBLOCKSIZE);
	zfs_scratch_put(mos_buf, ZFS_MAXBLOCKSIZE);
	return (err);
}

static int
zfs_mount1(struct zfs_pool *pool, const char *dsname,
    struct zfs_dataset *dsp, objset_phys_t *mos, objset_phys_t *os)
{
	dnode_phys_t dn;
	dsl_dir_phys_t *dd;
	uint64_t obj;
	char comp[ZAP_MAXNAMELEN];
	int err;

	err = zfs_read_block(pool, &pool->pool_ub.ub_rootbp, mos,
	    ZFS_MAXBLOCKSIZE);
	if (err != 0)
		return (err);
	if (mos->os_type != DMU_OST_META)
		return (EINVAL);

	err = zfs_read_dnode(pool, &mos->os_meta_dnode, MASTER_NODE_OBJ, &dn);
	if (err != 0)
		return (err);
	err = zap_lookup(pool, &dn, DMU_POOL_ROOT_DATASET, &obj);
	if (err != 0)
		return (err);

	/*
	 * With no dataset named, the pool says which one to boot.
	 *
	 * [S] has no notion of this: its Table 12 is the properties of a
	 * DSL directory, and bootfs is a property of the pool.  [Z]
	 * dmu.h puts those in a ZAP named "pool_props" under the object
	 * directory, and spa.c stores bootfs there as the object number
	 * of the DSL dataset -- not a name, and not a directory.
	 *
	 * A pool with no bootfs set falls back to its root dataset,
	 * which is what a pool made by hand for a single filesystem
	 * looks like.
	 */
	if (*dsname == '\0') {
		uint64_t props, bootfs;

		err = zfs_read_dnode(pool, &mos->os_meta_dnode,
		    MASTER_NODE_OBJ, &dn);
		if (err != 0)
			return (err);
		if (zap_lookup(pool, &dn, DMU_POOL_PROPS, &props) == 0 &&
		    zfs_read_dnode(pool, &mos->os_meta_dnode, props,
		    &dn) == 0 &&
		    zap_lookup(pool, &dn, ZPOOL_PROP_BOOTFS, &bootfs) == 0 &&
		    bootfs != 0)
			return (mount_dataset(pool, mos, os, bootfs, dsp));
	}

	/*
	 * [S] §4.4: each component of the name is looked up in the
	 * current directory's dd_child_dir_zapobj, which maps a child's
	 * name to its DSL directory object.
	 */
	for (;;) {
		size_t n;

		err = zfs_read_dnode(pool, &mos->os_meta_dnode, obj, &dn);
		if (err != 0)
			return (err);
		if (dn.dn_type != DMU_OT_DSL_DIR)
			return (EINVAL);
		dd = DN_BONUS(&dn);

		while (*dsname == '/')
			dsname++;
		if (*dsname == '\0')
			break;

		for (n = 0; dsname[n] != '\0' && dsname[n] != '/'; n++) {
			if (n + 1 >= sizeof(comp))
				return (ENAMETOOLONG);
			comp[n] = dsname[n];
		}
		comp[n] = '\0';
		dsname += n;

		err = zfs_read_dnode(pool, &mos->os_meta_dnode,
		    dd->dd_child_dir_zapobj, &dn);
		if (err != 0)
			return (err);
		err = zap_lookup(pool, &dn, comp, &obj);
		if (err != 0)
			return (err);
	}

	return (mount_dataset(pool, mos, os, dd->dd_head_dataset_obj, dsp));
}

/*
 * [S] §4.3: a dataset's ds_bp points at the object set it represents,
 * and [S] §6.1: object 1 of that set is the master node, which names
 * the root directory.
 */
static int
mount_dataset(struct zfs_pool *pool, objset_phys_t *mos, objset_phys_t *os,
    uint64_t dsobj, struct zfs_dataset *dsp)
{
	dsl_dataset_phys_t *ds;
	dnode_phys_t dn;
	int err;

	err = zfs_read_dnode(pool, &mos->os_meta_dnode, dsobj, &dn);
	if (err != 0)
		return (err);
	if (dn.dn_type != DMU_OT_DSL_DATASET)
		return (EINVAL);
	ds = DN_BONUS(&dn);

	err = zfs_read_block(pool, &ds->ds_bp, os, ZFS_MAXBLOCKSIZE);
	if (err != 0)
		return (err);
	if (os->os_type != DMU_OST_ZFS)
		return (EINVAL);

	dsp->ds_pool = pool;
	dsp->ds_meta = os->os_meta_dnode;

	err = zfs_read_dnode(pool, &dsp->ds_meta, MASTER_NODE_OBJ, &dn);
	if (err != 0)
		return (err);
	err = zap_lookup(pool, &dn, ZPL_VERSION_STR, &dsp->ds_version);
	if (err != 0)
		return (err);
	return (zap_lookup(pool, &dn, ZFS_ROOT_OBJ, &dsp->ds_root));
}

/*
 * [S] §6.2: "Traversing through a directory tree is as simple as looking
 * up the value for an entry and reading that object number."
 *
 * [Z] zfs_dir.c: the value a directory holds is not a bare object
 * number.  Its top four bits carry the entry's type, so that readdir
 * need not open every entry, and the object number is the low 48.  [S]
 * describes the version before that was added, where the whole value
 * was the object number; masking is right for both, because the type
 * bits are zero in the old form.
 */
#define	ZFS_DIRENT_OBJ(v)	((v) & ((1ULL << 48) - 1))

int
zfs_lookup(struct zfs_dataset *ds, const char *path, dnode_phys_t *dn)
{
	uint64_t obj = ds->ds_root;
	char comp[ZAP_MAXNAMELEN];
	int err;

	err = zfs_read_dnode(ds->ds_pool, &ds->ds_meta, obj, dn);
	if (err != 0)
		return (err);

	for (;;) {
		uint64_t val;
		size_t n;

		while (*path == '/')
			path++;
		if (*path == '\0')
			return (0);

		if (ZFS_MODE_FMT(zfs_mode(dn)) != ZFS_IFDIR)
			return (ENOTDIR);

		for (n = 0; path[n] != '\0' && path[n] != '/'; n++) {
			if (n + 1 >= sizeof(comp))
				return (ENAMETOOLONG);
			comp[n] = path[n];
		}
		comp[n] = '\0';
		path += n;

		err = zap_lookup(ds->ds_pool, dn, comp, &val);
		if (err != 0)
			return (err);
		err = zfs_read_dnode(ds->ds_pool, &ds->ds_meta,
		    ZFS_DIRENT_OBJ(val), dn);
		if (err != 0)
			return (err);
	}
}

/*
 * A filesystem object's mode and size.
 *
 * [S] §6.2 puts them in a znode_phys_t in the bonus buffer at fixed
 * offsets.  [Z] zfs_sa.h replaced that from ZPL version 5 with a packed
 * run of attributes headed by an sa_hdr_phys_t, and the header's magic
 * is what tells the two apart -- the reader does not have to trust the
 * dataset's VERSION for this.
 *
 * Only the default layout is read.  A pool whose files use another one
 * is refused by returning zero, rather than answering with whatever
 * happens to lie at the offset.
 */
static const uint8_t *
sa_field(const dnode_phys_t *dn, size_t off)
{
	const uint8_t *bonus = DN_BONUS(dn);
	const sa_hdr_phys_t *hdr = (const void *)bonus;
	size_t hdrsize, end;

	if (dn->dn_bonustype == DMU_OT_ZNODE)
		return (NULL);
	if (hdr->sa_magic != SA_MAGIC)
		return (NULL);
	if (SA_HDR_LAYOUT(hdr->sa_layout_info) > 3)
		return (NULL);

	/*
	 * The header says how long it is, in a six bit field that is
	 * multiplied by eight, so a bonus buffer written on purpose can
	 * claim 504 bytes of header and send the read past the dnode.
	 * [S] §3.1 bounds the bonus buffer at 320 bytes and the dnode at
	 * 512, so the field has to lie inside both.
	 */
	hdrsize = SA_HDR_SIZE(hdr->sa_layout_info);
	end = (size_t)(bonus - (const uint8_t *)dn) + hdrsize + off +
	    sizeof(uint64_t);
	if (hdrsize + off + sizeof(uint64_t) > dn->dn_bonuslen)
		return (NULL);
	if (end > DNODE_SIZE)
		return (NULL);
	return (bonus + hdrsize + off);
}

uint64_t
zfs_mode(const dnode_phys_t *dn)
{
	const uint8_t *p = sa_field(dn, SA_MODE_OFFSET);
	const znode_phys_t *zp = DN_BONUS(dn);

	if (p != NULL)
		return (*(const uint64_t *)(const void *)p);
	return (zp->zp_mode);
}

uint64_t
zfs_size(const dnode_phys_t *dn)
{
	const uint8_t *p = sa_field(dn, SA_SIZE_OFFSET);
	const znode_phys_t *zp = DN_BONUS(dn);

	if (p != NULL)
		return (*(const uint64_t *)(const void *)p);
	return (zp->zp_size);
}

static uint8_t *
cache_buf(const struct zfs_blkcache *c)
{

	return (c == NULL ? NULL : c->bc_buf);
}

static int
cache_has(const struct zfs_blkcache *c, uint64_t blkid)
{

	return (c != NULL && c->bc_valid && c->bc_blkid == blkid);
}

/*
 * Read from a file.  [S] §3.1: the object's data is its level 0 blocks,
 * each dn_datablkszsec * 512 bytes long.
 */
int
zfs_read_file(struct zfs_dataset *ds, const dnode_phys_t *dn, uint64_t off,
    void *buf, size_t len, size_t *done, struct zfs_blkcache *cache)
{
	uint8_t *blk;
	uint8_t *p = buf;
	uint64_t size = zfs_size(dn);
	size_t blksize = (size_t)dn->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	size_t n = 0;
	int err;

	if (blksize == 0 || blksize > ZFS_MAXBLOCKSIZE)
		return (EINVAL);
	if (off >= size) {
		*done = 0;
		return (0);
	}
	if (len > size - off)
		len = (size_t)(size - off);

	/*
	 * With a cache the block stays between calls; without one the
	 * scratch arena lends a buffer for the length of this call.
	 */
	if (cache != NULL && cache->bc_buf != NULL)
		blk = cache->bc_buf;
	else if ((blk = zfs_scratch_get(ZFS_MAXBLOCKSIZE)) == NULL)
		return (ENOMEM);

	while (n < len) {
		uint64_t blkid = off / blksize;
		size_t o = (size_t)(off % blksize);
		size_t c = blksize - o;
		size_t i;

		if (c > len - n)
			c = len - n;

		if (blk != cache_buf(cache) || !cache_has(cache, blkid)) {
			err = zfs_read_object_block(ds->ds_pool, dn, blkid,
			    blk, ZFS_MAXBLOCKSIZE);
			if (err != 0) {
				if (blk != cache_buf(cache))
					zfs_scratch_put(blk,
					    ZFS_MAXBLOCKSIZE);
				else
					cache->bc_valid = 0;
				return (err);
			}
			if (blk == cache_buf(cache)) {
				cache->bc_blkid = blkid;
				cache->bc_valid = 1;
			}
		}
		for (i = 0; i < c; i++)
			p[n + i] = blk[o + i];
		n += c;
		off += c;
	}
	if (blk != cache_buf(cache))
		zfs_scratch_put(blk, ZFS_MAXBLOCKSIZE);
	*done = n;
	return (0);
}
