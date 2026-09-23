/*
 * Walk a pool image the way [S] chapters 4 and 6 say to:
 * uberblock -> MOS -> object directory -> root DSL directory ->
 * active dataset -> ZPL object set -> master node -> root directory.
 *
 *	t_walk <image> <start-lba> <nsectors>
 */
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "zfs_ondisk.h"
#include "zfsread.h"
#include "scratch.h"

struct img { int fd; uint64_t base; };

static int
img_read(void *cookie, uint64_t off, void *buf, size_t len)
{
	struct img *im = cookie;
	ssize_t n = pread(im->fd, buf, len, (off_t)(im->base + off));

	if (n < 0)
		return (errno);
	return ((size_t)n == len ? 0 : EIO);
}

static uint8_t mosbuf[128 * 1024], zplbuf[128 * 1024];

#define	CHECK(what, e)	do {						\
	if ((e) != 0) {							\
		printf("%-28s %s\n", what, strerror(e));		\
		return (1);						\
	}								\
} while (0)

int
main(int argc, char **argv)
{
	struct zfs_pool pool;
	struct img im;
	objset_phys_t *mos = (void *)mosbuf, *zpl = (void *)zplbuf;
	dnode_phys_t dn, dsdn;
	dsl_dir_phys_t *dd;
	dsl_dataset_phys_t *ds;
	uint64_t obj, root;

	if (argc != 4)
		return (2);
	im.fd = open(argv[1], O_RDONLY);
	if (im.fd < 0) { perror(argv[1]); return (1); }
	im.base = strtoull(argv[2], NULL, 0) * 512;

	{
		static uint8_t arena[ZFS_SCRATCH_SIZE];

		zfs_scratch_init(arena, sizeof(arena));
	}
	memset(&pool, 0, sizeof(pool));
	pool.pool_read = img_read;
	pool.pool_cookie = &im;
	pool.pool_size = strtoull(argv[3], NULL, 0) * 512;
	pool.pool_ashift = 9;

	CHECK("uberblock", zfs_uberblock_find(&pool));
	printf("uberblock                    txg %llu\n",
	    (unsigned long long)pool.pool_ub.ub_txg);

	CHECK("MOS", zfs_read_block(&pool, &pool.pool_ub.ub_rootbp, mosbuf,
	    sizeof(mosbuf)));
	printf("MOS os_type                  %llu\n",
	    (unsigned long long)mos->os_type);

	CHECK("object directory dnode",
	    zfs_read_dnode(&pool, &mos->os_meta_dnode, MASTER_NODE_OBJ, &dn));
	printf("object directory type        %u\n", dn.dn_type);

	CHECK("root_dataset",
	    zap_lookup(&pool, &dn, DMU_POOL_ROOT_DATASET, &obj));
	printf("root_dataset                 object %llu\n",
	    (unsigned long long)obj);

	CHECK("root DSL directory",
	    zfs_read_dnode(&pool, &mos->os_meta_dnode, obj, &dn));
	if (dn.dn_type != DMU_OT_DSL_DIR) {
		printf("root DSL directory type %u, want %u\n", dn.dn_type,
		    DMU_OT_DSL_DIR);
		return (1);
	}
	dd = DN_BONUS(&dn);
	printf("dd_head_dataset_obj          %llu\n",
	    (unsigned long long)dd->dd_head_dataset_obj);
	printf("dd_child_dir_zapobj          %llu\n",
	    (unsigned long long)dd->dd_child_dir_zapobj);

	CHECK("active dataset",
	    zfs_read_dnode(&pool, &mos->os_meta_dnode,
	    dd->dd_head_dataset_obj, &dsdn));
	printf("active dataset type          %u\n", dsdn.dn_type);
	ds = DN_BONUS(&dsdn);

	CHECK("ZPL object set",
	    zfs_read_block(&pool, &ds->ds_bp, zplbuf, sizeof(zplbuf)));
	printf("ZPL os_type                  %llu (2 = DMU_OST_ZFS)\n",
	    (unsigned long long)zpl->os_type);

	CHECK("master node",
	    zfs_read_dnode(&pool, &zpl->os_meta_dnode, MASTER_NODE_OBJ, &dn));
	CHECK("ROOT", zap_lookup(&pool, &dn, ZFS_ROOT_OBJ, &root));
	printf("ROOT                         object %llu\n",
	    (unsigned long long)root);

	CHECK("root directory dnode",
	    zfs_read_dnode(&pool, &zpl->os_meta_dnode, root, &dn));
	printf("root directory type          %u (20 = DIRECTORY_CONTENTS)\n",
	    dn.dn_type);

	/* Look up each name the command line was given. */
	{
		static const char *want[] = { "netbsd", "boot", "etc",
		    "@", NULL };
		int i;

		for (i = 0; want[i] != NULL; i++) {
			if (zap_lookup(&pool, &dn, want[i], &obj) == 0)
				printf("/%-27s object %llu\n", want[i],
				    (unsigned long long)obj);
			else
				printf("/%-27s not found\n", want[i]);
		}
	}
	return (0);
}
