/*	$NetBSD$	*/

#ifndef _LIBSA_ZFS_EDONR_H_
#define	_LIBSA_ZFS_EDONR_H_

void	edonr512(const void *, size_t, uint8_t[64]);
void	edonr_zfs(const uint8_t[32], const void *, size_t, uint64_t[4]);

#endif	/* _LIBSA_ZFS_EDONR_H_ */
