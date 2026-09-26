/*
 * Mutate zstd frames and decode them, under ASan and UBSan.
 *
 *	fuzz_zstd <iterations> <seed> <file.zst> <size> [<file.zst> <size> ...]
 *
 * Each case takes one of the frames, flips a few bits or cuts it short,
 * and decodes it.  The frame is copied into an allocation of exactly
 * its own length, and the output into one of exactly the size asked
 * for, so that a read or write one byte past either is caught -- inside
 * a pool those buffers are larger than what they hold, and an overrun
 * would land on memory that is merely unused.
 *
 * "no fault" means only as much as the counts after it: a run where
 * nothing got past the frame header tested nothing.
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
#include "scratch.h"
#include "zstd.h"

static uint64_t st;

static uint64_t
rnd(void)
{

	st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
	return (st * 2685821657736338717ULL);
}

struct frame { uint8_t *base, *p; size_t len, size; };

static void
load(struct frame *f, const char *path, const char *size)
{
	uint8_t buf[1 << 16], *p = NULL;
	size_t len = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		perror(path);
		exit(1);
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if ((p = realloc(p, len + (size_t)n)) == NULL)
			exit(1);
		memcpy(p + len, buf, (size_t)n);
		len += (size_t)n;
	}
	close(fd);
	if (len < 4) {
		fprintf(stderr, "%s: too short\n", path);
		exit(1);
	}
	/* The zstd command writes a Magic_Number; ZFS does not. */
	f->base = p;
	f->p = p + 4;
	f->len = len - 4;
	f->size = strtoull(size, NULL, 0);
}

int
main(int argc, char **argv)
{
	static uint8_t arena[ZFS_SCRATCH_SIZE];
	struct frame fr[64];
	unsigned long iters, i, ok = 0, failed = 0;
	int nfr = 0, a;

	if (argc < 5 || (argc - 3) % 2 != 0) {
		fprintf(stderr, "usage: fuzz_zstd iterations seed "
		    "file.zst size ...\n");
		return (2);
	}
	iters = strtoul(argv[1], NULL, 0);
	st = strtoull(argv[2], NULL, 0) | 1;
	for (a = 3; a + 1 < argc && nfr < 64; a += 2)
		load(&fr[nfr++], argv[a], argv[a + 1]);
	zfs_scratch_init(arena, sizeof(arena));

	for (i = 0; i < iters; i++) {
		struct frame *f = &fr[rnd() % (uint64_t)nfr];
		size_t len = f->len, k, flips;
		uint8_t *in, *out;

		if (rnd() % 8 == 0)
			len = (size_t)(rnd() % (f->len + 1));
		in = malloc(len ? len : 1);
		out = malloc(f->size ? f->size : 1);
		if (in == NULL || out == NULL)
			return (1);
		memcpy(in, f->p, len);
		flips = 1 + rnd() % 4;
		for (k = 0; len > 0 && k < flips; k++)
			in[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
		if (zstd_decompress(in, len, out, f->size) == 0)
			ok++;
		else
			failed++;
		free(in);
		free(out);
	}
	for (a = 0; a < nfr; a++)
		free(fr[a].base);
	printf("fuzz_zstd: %lu mutations of %d frames, no fault\n", iters,
	    nfr);
	printf("  %lu decoded to the full size anyway, %lu refused\n", ok,
	    failed);
	return (0);
}
