/*	$NetBSD$	*/

/*
 * Fletcher checksums.
 *
 * [S] §2.4, Table 5 names fletcher2 and fletcher4 as two of the eight
 * checksum values a block pointer can carry, and says nothing else about
 * them: not the word size, not the order, not what is done with the four
 * accumulators.  A reader cannot verify a block from [S] alone.
 *
 * [Z] module/zcommon/zfs_fletcher.c is the description that exists.
 * Both keep four running 64 bit sums and store all four, but they do
 * not read the block the same way, and the difference is not something
 * the names suggest: fletcher2 takes it as 64 bit words and adds them in
 * pairs, while fletcher4 takes it as *32 bit* words and adds each in
 * turn.  Reading fletcher4 as 64 bit words produces a plausible looking
 * checksum that matches nothing.
 *
 * fletcher4 is what a pool NetBSD writes uses by default.
 */

#include <sys/types.h>

#include "fletcher.h"

void
fletcher4(const void *buf, size_t len, uint64_t out[4])
{
	const uint32_t *p = buf;
	const uint32_t *end = p + len / sizeof(uint32_t);
	uint64_t a = 0, b = 0, c = 0, d = 0;

	while (p < end) {
		a += *p++;
		b += a;
		c += b;
		d += c;
	}
	out[0] = a;
	out[1] = b;
	out[2] = c;
	out[3] = d;
}

void
fletcher2(const void *buf, size_t len, uint64_t out[4])
{
	const uint64_t *p = buf;
	const uint64_t *end = p + len / sizeof(uint64_t);
	uint64_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;

	while (p < end) {
		a0 += *p++;
		a1 += *p++;
		b0 += a0;
		b1 += a1;
	}
	out[0] = a0;
	out[1] = a1;
	out[2] = b0;
	out[3] = b1;
}
