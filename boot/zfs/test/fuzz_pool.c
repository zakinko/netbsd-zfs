/*
 * Mutate a pool image and read a file out of it, under ASan and UBSan.
 *
 *	fuzz_pool <image> <start-lba> <path> <iterations> [seed]
 *
 * It counts how far each case got, because "no fault" means nothing if
 * the mutations never reached a dnode: a run where opened and mounted
 * are zero is a broken harness, not a clean reader.
 *
 * Built with -DZFS_FUZZ_NO_CKSUM, which takes the block checksums out
 * of the way.  That is deliberate, and it is the threat model: the
 * reader is hardened against a disk somebody wrote on purpose, and
 * whoever writes one recomputes the checksums.  A fuzzer left to fight
 * fletcher4 tests fletcher4 and never reaches a dnode.
 *
 * The mutation happens where the reader reads, not where the image
 * sits.  Flipping bits at addresses chosen from the image almost never
 * touches a structure the reader walks -- ZFS puts its metadata
 * wherever the allocator felt like, and with the checksums out of the
 * way a flip in a data block simply changes the data.  Measured: four
 * hundred cases mutated by address all read the file unharmed, which
 * says nothing about the parser.
 *
 * So the device hands back a corrupted buffer instead: every read has
 * a chance of coming back with a few bits flipped, which is exactly
 * what a disk written against this reader would do.  The image itself
 * is never written, so it is mapped once and read from -- a copy per
 * case cost far more than the parsing it was feeding.
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

static uint64_t st = 1;

static uint64_t rnd(void);

static uint64_t
rnd(void)
{

	st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
	return (st * 2685821657736338717ULL);
}

struct img { uint8_t *base; size_t len; uint64_t off; };

static long n_opened, n_mounted, n_found, n_read;
static const char *g_path = "/payload";

static int g_corrupt;		/* one in this many reads comes back wrong */

static int
img_read(void *cookie, uint64_t off, void *buf, size_t len)
{
	struct img *im = cookie;

	off += im->off;
	if (off > im->len || len > im->len - off)
		return (EIO);
	memcpy(buf, im->base + off, len);

	if (g_corrupt != 0 && len != 0 && (rnd() % g_corrupt) == 0) {
		int k, n = 1 + (int)(rnd() % 8);

		for (k = 0; k < n; k++) {
			size_t p = (size_t)(rnd() % len);

			((uint8_t *)buf)[p] ^= (uint8_t)(1u << (rnd() & 7));
		}
	}
	return (0);
}

static uint8_t arena[ZFS_SCRATCH_SIZE];
static uint8_t out[1 << 20];

static void
one(uint8_t *image, size_t len, uint64_t base)
{
	struct zfs_pool pool;
	struct zfs_dataset ds;
	struct img im;
	dnode_phys_t dn;
	size_t got;

	im.base = image;
	im.len = len;
	im.off = base;
	memset(&pool, 0, sizeof(pool));
	pool.pool_read = img_read;
	pool.pool_cookie = &im;
	pool.pool_size = len - base;

	if (zfs_pool_open(&pool, NULL, 0) != 0)
		return;
	n_opened++;
	if (zfs_mount(&pool, "", &ds) != 0)
		return;
	n_mounted++;
	if (zfs_lookup(&ds, g_path, &dn) != 0)
		return;
	n_found++;
	if (zfs_read_file(&ds, &dn, 0, out, sizeof(out), &got, NULL) == 0)
		n_read++;
}

int
main(int argc, char **argv)
{
	uint8_t *orig;
	size_t len;
	uint64_t base;
	long iters, i;
	int fd;

	if (argc < 5) {
		fprintf(stderr, "usage: %s image start-lba path "
		    "iterations [seed]\n", argv[0]);
		return (2);
	}
	base = strtoull(argv[2], NULL, 0) * 512;
	g_path = argv[3];
	iters = atol(argv[4]);
	if (argc > 5)
		st = strtoull(argv[5], NULL, 0);
	if (st == 0)
		st = 1;

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror(argv[1]); return (1); }
	len = (size_t)lseek(fd, 0, SEEK_END);
	orig = malloc(len);
	if (orig == NULL) { perror("malloc"); return (1); }
	if (pread(fd, orig, len, 0) != (ssize_t)len) {
		perror("read");
		return (1);
	}
	close(fd);

	zfs_scratch_init(arena, sizeof(arena));

	for (i = 0; i < iters; i++) {
		g_corrupt = 1 + (int)(rnd() % 32);
		one(orig, len, base);
	}
	printf("fuzz_pool: %ld mutations, no fault\n", iters);
	printf("  reached: %ld opened, %ld mounted, %ld found the file, "
	    "%ld read it\n", n_opened, n_mounted, n_found, n_read);
	free(orig);
	return (0);
}
