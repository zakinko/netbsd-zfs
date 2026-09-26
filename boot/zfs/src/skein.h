/*	$NetBSD$	*/

#ifndef _LIBSA_ZFS_SKEIN_H_
#define	_LIBSA_ZFS_SKEIN_H_

void	skein512(const uint8_t *, size_t, const void *, size_t, unsigned,
	    uint8_t *);
void	skein_zfs(const uint8_t[32], const void *, size_t, uint64_t[4]);
void	skein512_iv(unsigned, uint64_t[8]);

#endif	/* _LIBSA_ZFS_SKEIN_H_ */
