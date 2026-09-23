/*	$NetBSD$	*/

/*
 * Zero-length encoding.
 *
 * [S] §2.5, Table 6 does not have it: zle came later, as [Z]
 * zio_compress.h's compression 14.  It is the simplest of the three
 * this reader understands, and its description in [Z] zle.c is one
 * sentence:
 *
 *	"Each chunk of compressed data begins with a length byte, b.
 *	 If b < n (where n is the compression parameter) then the next
 *	 b + 1 bytes are literal values.  If b >= n then the next
 *	 (256 - b + 1) bytes are zero."
 *
 * [Z] zio_compress.c gives n as 64 for the "zle" entry, so that is what
 * a block on disk was written with.
 *
 * Worth knowing when testing: a file of nothing but zeros never
 * exercises this, because ZFS stores it as holes and there is no block
 * to decompress.  It takes zeros with something in them.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "zle.h"

#define	ZLE_N	64		/* [Z] zio_compress.c */

int
zle_decompress(const void *src, void *dst, size_t srclen, size_t dstlen)
{
	const uint8_t *in = src, *inend = in + srclen;
	uint8_t *out = dst, *outend = out + dstlen;

	while (in < inend && out < outend) {
		int len = 1 + *in++;

		if (len <= ZLE_N) {
			if (in + len > inend || out + len > outend)
				return (EINVAL);
			while (len-- != 0)
				*out++ = *in++;
		} else {
			len -= ZLE_N;
			if (out + len > outend)
				return (EINVAL);
			while (len-- != 0)
				*out++ = 0;
		}
	}

	/*
	 * The encoding carries no end marker, so the only check on it is
	 * that it produced exactly the logical block.
	 */
	return (out == outend ? 0 : EINVAL);
}
