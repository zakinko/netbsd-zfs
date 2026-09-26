/*	$NetBSD$	*/

#ifndef _LIBSA_ZFS_BLAKE3_H_
#define	_LIBSA_ZFS_BLAKE3_H_

void	blake3_keyed(const uint8_t[32], const void *, size_t, uint8_t *,
	    uint64_t[4]);
void	blake3_hash(const void *, size_t, uint8_t[32]);

#endif	/* _LIBSA_ZFS_BLAKE3_H_ */
