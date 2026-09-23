/*	$NetBSD$	*/

/*
 * gzip decompression, by way of zlib.
 *
 * [S] §2.5, Table 6 knows only lzjb.  [Z] zio_compress.h added gzip at
 * levels one to nine (5 through 13); the level only affects how the
 * block was written, so one decompressor serves all nine.
 *
 * [Z] gzip.c calls zlib's uncompress() on the block as it lies, which
 * is to say that a gzip compressed ZFS block is a plain zlib stream --
 * header and all -- of exactly the logical block's contents.  There is
 * no length prefix, unlike lz4: the block pointer already gives both
 * sizes.
 *
 * The loader links libz anyway, for gzipped kernels, so this adds a
 * call and not a library.  A build without zlib leaves it out and the
 * reader then refuses gzip rather than guessing at it.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "gzip.h"

#ifdef ZFS_SUPPORT_GZIP

#include <zlib.h>

int
gzip_decompress(const void *src, void *dst, size_t srclen, size_t dstlen)
{
	uLongf out = dstlen;

	if (uncompress(dst, &out, src, (uLong)srclen) != Z_OK)
		return (EINVAL);
	if (out != dstlen)
		return (EINVAL);
	return (0);
}

#else

int
gzip_decompress(const void *src, void *dst, size_t srclen, size_t dstlen)
{

	(void)src; (void)dst; (void)srclen; (void)dstlen;
	return (ENOTSUP);
}

#endif
