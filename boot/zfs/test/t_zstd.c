/*
 * t_zstd tables
 * t_zstd dec <file.zst> <size> <outfile>
 *
 * The first prints the decoding tables built from [R] RFC 8878's
 * predefined distributions, one "state symbol bits base" line each,
 * for comparing with the RFC's Appendix A.
 *
 * The second decodes a frame written by the zstd command, which keeps
 * the four byte Magic_Number ZFS leaves out; it is checked and then
 * skipped.  <size> is what the frame should decode to.
 *
 * The source is included rather than linked so that its static
 * functions can be reached, as t_dnode.c does with zfsread.c.
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

#include "../src/zfs_ondisk.h"	/* for ZFS_SCRATCH_SIZE */
#include "../src/zstd.c"

static void
table(const char *name, const int16_t *def, unsigned n, unsigned al)
{
	struct fse t[1 << LL_MAXAL];
	unsigned u;

	if (fse_build(t, def, n, al) != 0) {
		printf("%s: fse_build failed\n", name);
		exit(1);
	}
	for (u = 0; u < (1u << al); u++)
		printf("%s %u %u %u %u\n", name, u, t[u].sym, t[u].nb,
		    t[u].base);
}

static uint8_t *
slurp(const char *path, size_t *lenp)
{
	uint8_t *buf = NULL;
	size_t len = 0, cap = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		perror(path);
		exit(1);
	}
	for (;;) {
		if (len == cap) {
			cap = cap ? cap * 2 : 65536;
			if ((buf = realloc(buf, cap)) == NULL)
				exit(1);
		}
		n = read(fd, buf + len, cap - len);
		if (n < 0) {
			perror(path);
			exit(1);
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	*lenp = len;
	return (buf);
}

int
main(int argc, char **argv)
{
	static uint8_t arena[ZFS_SCRATCH_SIZE];
	uint8_t *in, *out;
	size_t inlen, outlen;
	int err, fd;

	if (argc == 2 && strcmp(argv[1], "tables") == 0) {
		table("LL", ll_default, LL_NSYM, 6);
		table("ML", ml_default, ML_NSYM, 6);
		table("OF", of_default, 29, 5);
		return (0);
	}
	if (argc != 5 || strcmp(argv[1], "dec") != 0) {
		fprintf(stderr, "usage: t_zstd tables | "
		    "t_zstd dec file.zst size outfile\n");
		return (2);
	}

	zfs_scratch_init(arena, sizeof(arena));
	in = slurp(argv[2], &inlen);
	outlen = strtoull(argv[3], NULL, 0);
	if ((out = malloc(outlen ? outlen : 1)) == NULL)
		return (1);

	/* [R] §3.1.1: Magic_Number, 0xFD2FB528 little endian. */
	if (inlen < 4 || in[0] != 0x28 || in[1] != 0xb5 || in[2] != 0x2f ||
	    in[3] != 0xfd) {
		printf("%s: not a zstd frame\n", argv[2]);
		free(in);
		free(out);
		return (1);
	}
	err = zstd_decompress(in + 4, inlen - 4, out, outlen);
	free(in);
	if (err != 0) {
		printf("%s: %s\n", argv[2], strerror(err));
		free(out);
		return (1);
	}
	fd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, out, outlen) != (ssize_t)outlen) {
		perror(argv[4]);
		return (1);
	}
	close(fd);
	free(out);
	return (0);
}
