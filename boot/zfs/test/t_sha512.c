/*
 * SHA-512/256: NIST's published examples for FIPS 180-4 (SHA512_256.pdf,
 * the one block "abc" and the two block 112 character message), then
 *
 *	t_sha512 <file>
 *
 * prints the digest of a file in hex, for comparing with another
 * implementation over many lengths.
 */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sha512.h"

static void
hex(const uint8_t d[32], char out[65])
{
	int i;

	for (i = 0; i < 32; i++)
		snprintf(out + i * 2, 3, "%02x", d[i]);
}

static int
check(const char *msg, const char *want)
{
	uint8_t d[32];
	uint64_t w[4];
	char got[65];

	sha512_256(msg, strlen(msg), d, w);
	hex(d, got);
	if (strcmp(got, want) != 0) {
		printf("FAIL %s\n  got  %s\n  want %s\n", msg, got, want);
		return (1);
	}
	return (0);
}

int
main(int argc, char **argv)
{
	int bad = 0;

	if (argc == 2) {
		static uint8_t buf[1 << 20];
		uint8_t d[32];
		uint64_t w[4];
		char out[65];
		size_t n;
		FILE *f = fopen(argv[1], "rb");

		if (f == NULL)
			return (1);
		n = fread(buf, 1, sizeof(buf), f);
		fclose(f);
		sha512_256(buf, n, d, w);
		hex(d, out);
		printf("%s\n", out);
		return (0);
	}
	bad |= check("abc",
	    "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");
	bad |= check("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
	    "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
	    "3928e184fb8690f840da3988121d31be65cb9d3ef83ee6146feac861e19b563a");
	printf(bad ? "=== SHA-512/256 FAILED\n" :
	    "=== SHA-512/256: NIST's two examples\n");
	return (bad);
}
