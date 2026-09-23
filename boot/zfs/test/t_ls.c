/* t_ls <image> <lba> <nsec> <dataset> <dir> */
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
static void
show(void *arg, const char *name, uint64_t val)
{
	(void)arg;
	printf("  %-24s %llu\n", name, (unsigned long long)val);
}
int
main(int argc, char **argv)
{
	struct zfs_pool pool; struct zfs_dataset ds; struct img im;
	dnode_phys_t dn; int err;

	if (argc < 6) return (2);
	im.fd = open(argv[1], O_RDONLY);
	im.base = strtoull(argv[2], NULL, 0) * 512;
	{
		static uint8_t arena[ZFS_SCRATCH_SIZE];

		zfs_scratch_init(arena, sizeof(arena));
	}
	memset(&pool, 0, sizeof(pool));
	pool.pool_read = img_read; pool.pool_cookie = &im;
	pool.pool_size = strtoull(argv[3], NULL, 0) * 512;
	pool.pool_ashift = 9;
	if ((err = zfs_uberblock_find(&pool)) != 0) goto bad;
	if ((err = zfs_mount(&pool, argv[4], &ds)) != 0) goto bad;
	if ((err = zfs_lookup(&ds, argv[5], &dn)) != 0) goto bad;
	printf("%s%s:\n", argv[4], argv[5]);
	if ((err = zap_list(&pool, &dn, show, NULL)) != 0) goto bad;
	return (0);
bad:
	printf("%s\n", strerror(err));
	return (1);
}
