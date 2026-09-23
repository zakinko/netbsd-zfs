/*
 * Read a file in small pieces, the way the loader does, and time it.
 *
 *	t_bench <image> <start-lba> <nsectors> <dataset> <path> [cache]
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
#include <time.h>
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
static uint8_t arena[ZFS_SCRATCH_SIZE], cachebuf[ZFS_MAXBLOCKSIZE];

int
main(int argc, char **argv)
{
	struct zfs_pool pool; struct zfs_dataset ds; struct img im;
	struct zfs_blkcache cache = { cachebuf, 0, 0 };
	dnode_phys_t dn;
	uint8_t piece[4096];
	uint64_t off, size;
	clock_t t0;
	int usecache = argc > 6 ? atoi(argv[6]) : 1;

	if (argc < 6) {
		fprintf(stderr, "usage: %s image lba nsectors dataset path "
		    "[cache]\n", argv[0]);
		return (2);
	}
	zfs_scratch_init(arena, sizeof(arena));
	im.fd = open(argv[1], O_RDONLY);
	if (im.fd < 0) { perror(argv[1]); return (1); }
	im.base = strtoull(argv[2], NULL, 0) * 512;
	memset(&pool, 0, sizeof(pool));
	pool.pool_read = img_read; pool.pool_cookie = &im;
	pool.pool_size = strtoull(argv[3], NULL, 0) * 512;
	if (zfs_pool_open(&pool, NULL, 0)) return (1);
	if (zfs_mount(&pool, argv[4], &ds)) return (1);
	if (zfs_lookup(&ds, argv[5], &dn)) return (1);
	size = zfs_size(&dn);

	t0 = clock();
	for (off = 0; off < size; ) {
		size_t got;
		if (zfs_read_file(&ds, &dn, off, piece, sizeof(piece), &got,
		    usecache ? &cache : NULL) != 0 || got == 0)
			break;
		off += got;
	}
	printf("%s in %zu byte pieces, cache %s: %llu bytes in %.2f s\n",
	    argv[5], sizeof(piece), usecache ? "on" : "off",
	    (unsigned long long)off, (double)(clock() - t0) / CLOCKS_PER_SEC);
	return (0);
}
