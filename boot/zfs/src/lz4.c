/*	$NetBSD$	*/

/*
 * LZ4 decompression, from the LZ4 Block Format Description
 * (lz4/lz4, doc/lz4_Block_format.md, version 1.6.1).  Only the
 * decompressor is here; a bootloader never writes.
 *
 * A block is a sequence of sequences.  Each begins with a token byte
 * whose high nibble is a literal length and whose low nibble is a match
 * length; either, when its nibble is 15, is continued by bytes that add
 * 255 each until a byte below 255 ends the run.  The literals follow the
 * token, then a two byte little endian offset back into what has already
 * been produced, and the match copies (length + 4) bytes from there --
 * overlapping, so the copy has to go a byte at a time.  The last
 * sequence has literals and no match.
 *
 * [Z] lz4.c: ZFS prefixes the compressed block with its own length as a
 * four byte big endian count, because a block pointer records only the
 * allocated size.  That prefix is not part of the format above.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "lz4.h"

int
lz4_decompress(const void *src, void *dst, size_t srclen, size_t dstlen)
{
	const uint8_t *in = src, *inend;
	uint8_t *out = dst, *outend = out + dstlen;
	uint32_t csize;

	/* [Z] the four byte big endian length ZFS puts in front. */
	if (srclen < 4)
		return (EINVAL);
	csize = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
	    ((uint32_t)in[2] << 8) | in[3];
	if (csize > srclen - 4)
		return (EINVAL);
	in += 4;
	inend = in + csize;

	while (in < inend) {
		unsigned token, litlen, matchlen;
		const uint8_t *match;
		unsigned off;

		token = *in++;

		litlen = token >> 4;
		if (litlen == 15) {
			unsigned c;

			do {
				if (in >= inend)
					return (EINVAL);
				c = *in++;
				litlen += c;
			} while (c == 255);
		}

		if ((size_t)(inend - in) < litlen ||
		    (size_t)(outend - out) < litlen)
			return (EINVAL);
		while (litlen-- > 0)
			*out++ = *in++;

		/*
		 * The final sequence stops here: it has literals and no
		 * match, and nothing follows the token's literal run.
		 */
		if (in >= inend)
			break;

		if (inend - in < 2)
			return (EINVAL);
		off = in[0] | ((unsigned)in[1] << 8);
		in += 2;
		if (off == 0 || (size_t)(out - (uint8_t *)dst) < off)
			return (EINVAL);
		match = out - off;

		matchlen = token & 0xf;
		if (matchlen == 15) {
			unsigned c;

			do {
				if (in > inend)
					return (EINVAL);
				c = *in++;
				matchlen += c;
			} while (c == 255);
		}
		matchlen += 4;		/* the minimum match length */

		if ((size_t)(outend - out) < matchlen)
			return (EINVAL);
		/*
		 * Byte at a time: a match may overlap what it produces,
		 * which is how a run of one byte is encoded.
		 */
		while (matchlen-- > 0)
			*out++ = *match++;
	}

	return (0);
}
