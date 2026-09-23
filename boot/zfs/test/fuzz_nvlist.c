/*
 * A mutation driver for the label nvlist parser.  A label is whatever
 * was on the disk that got plugged in, so the parser has to survive
 * arbitrary bytes without reading outside the buffer it was given.
 *
 * There is no libFuzzer in this toolchain, so the corpus is a real
 * label mutated in the ways that break length-driven parsers: random
 * bytes, random words (sizes and lengths are words), and truncation to
 * every length, which is what catches a check made against the wrong
 * end.
 *
 * Each case is copied into an allocation of exactly its own size, so
 * that ASan's redzone sits immediately after the last valid byte and a
 * one byte over-read is a failure rather than slack.
 *
 *	fuzz_nvlist <label-file> [iterations]
 */
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "nvlist.h"

static const char *names[] = { "version", "name", "state", "txg",
    "pool_guid", "top_guid", "guid", "vdev_tree", "ashift", "asize",
    "children", "type", "path", "", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" };

static void
run(const uint8_t *data, size_t size)
{
	struct nvpair_value v;
	uint8_t *copy = malloc(size ? size : 1);
	size_t i;

	memcpy(copy, data, size);
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		nvlist_find(copy, size, names[i], NV_WANT_UINT64, &v);
		nvlist_find(copy, size, names[i], NV_WANT_STRING, &v);
		nvlist_find_nested(copy, size, names[i], NV_WANT_UINT64, &v);
		if (nvlist_find(copy, size, names[i], NV_WANT_NVLIST,
		    &v) == 0) {
			struct nvpair_value w;
			const uint8_t *l = v.nv_list;
			size_t n = v.nv_listlen;

			/* The nested list must stay inside the outer one. */
			if (l < copy || l + n > copy + size) {
				printf("nested list escapes the buffer\n");
				abort();
			}
			nvlist_find_nested(l, n, "ashift", NV_WANT_UINT64, &w);
			nvlist_find_nested(l, n, "type", NV_WANT_STRING, &w);
			nvlist_find_nested(l, n, "children",
			    NV_WANT_NVLIST, &w);
		}
	}
	free(copy);
}

int
main(int argc, char **argv)
{
	static uint8_t seed[8192], buf[8192];
	size_t seedlen;
	long iters = argc > 2 ? strtol(argv[2], NULL, 0) : 200000;
	long n;
	int fd;

	if (argc < 2)
		return (2);
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror(argv[1]); return (1); }
	seedlen = (size_t)read(fd, seed, sizeof(seed));
	close(fd);

	/* Every truncation of the real label. */
	for (n = 0; n <= (long)seedlen; n++)
		run(seed, (size_t)n);

	srandom(1);
	for (n = 0; n < iters; n++) {
		size_t len = 1 + (size_t)(random() % (long)seedlen);
		int muts = 1 + (int)(random() % 8);
		int i;

		memcpy(buf, seed, len);
		for (i = 0; i < muts; i++) {
			size_t at = (size_t)(random() % (long)len);

			switch (random() % 3) {
			case 0:
				buf[at] = (uint8_t)random();
				break;
			case 1:
				/* Sizes and lengths are four byte words. */
				at &= ~(size_t)3;
				if (at + 4 <= len) {
					uint32_t w = (uint32_t)random();

					if (random() % 2)
						w = (uint32_t)-1 >>
						    (random() % 24);
					memcpy(buf + at, &w, 4);
				}
				break;
			case 2:
				memset(buf + at, random() % 2 ? 0 : 0xff,
				    (size_t)(random() % 8) + 1 >
				    len - at ? len - at :
				    (size_t)(random() % 8) + 1);
				break;
			}
		}
		run(buf, len);
	}
	printf("fuzz_nvlist: %ld mutations and %zu truncations, no fault\n",
	    iters, seedlen + 1);
	return (0);
}
