/*	$NetBSD$	*/

#ifndef _LIBSA_FLETCHER_H_
#define	_LIBSA_FLETCHER_H_

void	fletcher2(const void *, size_t, uint64_t[4]);
void	fletcher4(const void *, size_t, uint64_t[4]);

#endif	/* _LIBSA_FLETCHER_H_ */
