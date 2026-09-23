/* Print a directory's ZAP header: micro or fat, and where its pointer
 * table lives. */
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
img_read(void *c, uint64_t off, void *buf, size_t len)
{
	struct img *im = c;
	ssize_t n = pread(im->fd, buf, len, (off_t)(im->base + off));
	if (n < 0) return (errno);
	return ((size_t)n == len ? 0 : EIO);
}
static uint8_t arena[ZFS_SCRATCH_SIZE], blk[ZFS_MAXBLOCKSIZE];

int
main(int argc, char **argv)
{
	struct zfs_pool pool; struct zfs_dataset ds; struct img im;
	dnode_phys_t dn; zap_phys_t *zp;

	if (argc < 5) return (2);
	zfs_scratch_init(arena, sizeof(arena));
	im.fd = open(argv[1], O_RDONLY); im.base = 0;
	memset(&pool, 0, sizeof(pool));
	pool.pool_read = img_read; pool.pool_cookie = &im;
	pool.pool_size = strtoull(argv[2], NULL, 0) * 512;
	if (zfs_pool_open(&pool, NULL, 0)) { puts("pool"); return 1; }
	if (zfs_mount(&pool, argv[3], &ds)) { puts("mount"); return 1; }
	if (zfs_lookup(&ds, argv[4], &dn)) { puts("lookup"); return 1; }
	printf("dnode: datablk %u levels %u maxblkid %llu\n",
	    dn.dn_datablkszsec * 512, dn.dn_nlevels,
	    (unsigned long long)dn.dn_maxblkid);
	if (zfs_read_object_block(&pool, &dn, 0, blk, sizeof(blk))) {
		puts("read"); return 1;
	}
	if (*(uint64_t *)(void *)blk == ZBT_MICRO) { puts("microzap"); return 0; }
	zp = (void *)blk;
	printf("fatzap: entries %llu leafs %llu zt_shift %llu "
	    "zt_blk %llu zt_numblks %llu -> %s\n",
	    (unsigned long long)zp->zap_num_entries,
	    (unsigned long long)zp->zap_num_leafs,
	    (unsigned long long)zp->zap_ptrtbl.zt_shift,
	    (unsigned long long)zp->zap_ptrtbl.zt_blk,
	    (unsigned long long)zp->zap_ptrtbl.zt_numblks,
	    zp->zap_ptrtbl.zt_numblks ? "EXTERNAL" : "embedded");
	return 0;
}
