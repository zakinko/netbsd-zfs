/*
 * t_cat <image> <start-lba> <nsectors> <dataset> <path> [outfile]
 *
 * Mounts a dataset out of a pool image and reads a file from it, the
 * whole way down from the uberblock.
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
img_read(void *c, uint64_t off, void *buf, size_t len)
{
	struct img *im = c;
	ssize_t n = pread(im->fd, buf, len, (off_t)(im->base + off));

	if (n < 0)
		return (errno);
	return ((size_t)n == len ? 0 : EIO);
}

static uint8_t chunk[1 << 20];

int
main(int argc, char **argv)
{
	struct zfs_pool pool;
	struct zfs_dataset ds;
	struct img im;
	dnode_phys_t dn;
	static uint8_t cachebuf[ZFS_MAXBLOCKSIZE];
	struct zfs_blkcache cache = { cachebuf, 0, 0 };
	uint64_t off, size;
	int err, out = -1;

	if (argc < 6) {
		fprintf(stderr, "usage: %s image lba nsec dataset path "
		    "[outfile]\n", argv[0]);
		return (2);
	}
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
	{
		char pname[64];

		if ((err = zfs_pool_open(&pool, pname, sizeof(pname))) != 0) {
			printf("pool: %s\n", strerror(err));
			return (1);
		}
		printf("pool %s: guid %llu, ashift %u, txg %llu\n", pname,
		    (unsigned long long)pool.pool_guid, pool.pool_ashift,
		    (unsigned long long)pool.pool_ub.ub_txg);
	}
	if ((err = zfs_mount(&pool, argv[4], &ds)) != 0) {
		printf("mount %s: %s\n", argv[4], strerror(err));
#ifdef ZFS_SCRATCH_DEBUG
		printf("scratch: high %zu of %u, %d misput, %d exhausted\n",
		    zfs_scratch_high, (unsigned)ZFS_SCRATCH_SIZE,
		    zfs_scratch_misput, zfs_scratch_exhausted);
#endif
		return (1);
	}
	printf("%s: ZPL version %llu, root object %llu\n", argv[4],
	    (unsigned long long)ds.ds_version,
	    (unsigned long long)ds.ds_root);

	if ((err = zfs_lookup(&ds, argv[5], &dn)) != 0) {
		printf("%s: %s\n", argv[5], strerror(err));
		return (1);
	}
	size = zfs_size(&dn);
	printf("%s: mode %#llo size %llu bytes, dnode type %u, %u levels, "
	    "%u byte blocks\n", argv[5],
	    (unsigned long long)zfs_mode(&dn), (unsigned long long)size,
	    dn.dn_type, dn.dn_nlevels,
	    dn.dn_datablkszsec * 512);

	if (argc > 6) {
		out = open(argv[6], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) { perror(argv[6]); return (1); }
	}
	for (off = 0; off < size; ) {
		size_t got;

		err = zfs_read_file(&ds, &dn, off, chunk, sizeof(chunk),
		    &got, &cache);
		if (err != 0) {
			printf("read at %llu: %s\n",
			    (unsigned long long)off, strerror(err));
			return (1);
		}
		if (got == 0)
			break;
		if (out >= 0 && write(out, chunk, got) != (ssize_t)got) {
			perror("write");
			return (1);
		}
		off += got;
	}
	printf("read %llu bytes\n", (unsigned long long)off);
#ifdef ZFS_SCRATCH_DEBUG
	printf("scratch: high %zu of %u bytes, %d misput, %d exhausted\n",
	    zfs_scratch_high, (unsigned)ZFS_SCRATCH_SIZE, zfs_scratch_misput,
	    zfs_scratch_exhausted);
#endif
	if (out >= 0)
		close(out);
	return (0);
}
