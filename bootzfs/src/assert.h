/*	$NetBSD$	*/

/*
 * lz4.c asks for <assert.h> in its standalone branch, which FreeBSD's
 * libsa has and NetBSD's does not.  The loader has nowhere to report a
 * failed assertion and nothing it could do afterwards, so it stands down.
 * Naming it here rather than editing that file keeps it re-importable.
 */

#ifndef _BOOT_ZFS_ASSERT_H_
#define	_BOOT_ZFS_ASSERT_H_

/*
 * libkern.h maps assert() onto KASSERT(), which reaches vpanic(); libsa
 * has panic() and no vpanic(), and a failed assertion in a decompressor
 * is not worth carrying one in for.
 */
#undef	assert
#define	assert(x)	((void)0)

#endif	/* _BOOT_ZFS_ASSERT_H_ */
