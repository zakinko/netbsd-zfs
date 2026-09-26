/*	$NetBSD$	*/

#ifndef _LIBSA_ZFS_ZSTD_H_
#define	_LIBSA_ZFS_ZSTD_H_

/*
 * The scratch a frame needs while it is decoded: the state carried
 * between blocks, and one block of literals.  See scratch.h.
 */
#define	ZSTD_STATE_MAX		(16 * 1024)

int	zstd_decompress(const void *, size_t, void *, size_t);
int	zfs_zstd_decompress(const void *, void *, size_t, size_t);

#endif	/* _LIBSA_ZFS_ZSTD_H_ */
