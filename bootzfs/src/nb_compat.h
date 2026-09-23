/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * What the files carried into this directory reach for and NetBSD either
 * spells differently or does not have.  It is force-included into each of
 * them, the way FreeBSD force-includes its own ccompile.h, so that they
 * can be re-imported unchanged.
 */

#ifndef _BOOT_ZFS_NB_COMPAT_H_
#define	_BOOT_ZFS_NB_COMPAT_H_

#include <sys/param.h>
#include <sys/stdarg.h>

#include <lib/libkern/libkern.h>

typedef unsigned int uint_t;

/*
 * list.c asks for <sys/debug.h> meaning ZFS's assertions.  NetBSD's is a
 * kernel-only header that refuses to be read anywhere else, and it sits
 * earlier on the include path, so close it here and answer instead.  The
 * loader has nowhere to report a failed assertion and nothing it could do
 * afterwards, so they stand down.
 */
#define	__SYS_DEBUG_H__
#define	ASSERT(x)		((void)0)
#define	ASSERT0(x)		((void)0)
#define	ASSERT3S(a, op, b)	((void)0)
#define	ASSERT3U(a, op, b)	((void)0)
#define	ASSERT3P(a, op, b)	((void)0)
#define	IMPLY(a, b)		((void)0)
#define	EQUIV(a, b)		((void)0)
#define	VERIFY(x)		((void)(x))

/*
 * NetBSD's boolean_t is the deprecated Mach int and carries no names for
 * its values.  zfsimpl.h offers NEED_SOLARIS_BOOLEAN so that its own enum
 * stands aside; the names still have to come from somewhere.
 */
#define	B_FALSE	0
#define	B_TRUE	1

#define	__DECONST(type, var)	((type)__UNCONST(var))

/* ZFS's private errno for a failed checksum; NetBSD has no name for it. */
#define	ECKSUM	EBADMSG

#define	bcmp(a, b, n)	memcmp((a), (b), (n))

#ifndef nitems
#define	nitems(x)	__arraycount(x)
#endif

/*
 * From FreeBSD's libzfs.h, which is not carried: the rest of it describes
 * that loader's device model, which is not this one.
 */
#define	ZFS_MAXNAMELEN	256

/*
 * libsa offers alloc()/dealloc(), and dealloc() wants the size back, which
 * the carried code does not have when it frees.  zfs_compat.c keeps the
 * size in front of each block to make up the difference.
 */
void	*zfs_malloc(size_t);
void	*zfs_calloc(size_t, size_t);
void	*zfs_realloc(void *, size_t);
void	 zfs_free(void *);
int	 zfs_asprintf(char **, const char *, ...) __printflike(2, 3);
char	*zfs_strdup(const char *);

#define	malloc(n)	zfs_malloc(n)
#define	calloc(n, s)	zfs_calloc((n), (s))
#define	realloc(p, n)	zfs_realloc((p), (n))
#define	free(p)		zfs_free(p)
#define	asprintf	zfs_asprintf
#define	strdup		zfs_strdup

/*
 * FreeBSD's loader pages long output.  Nothing here does, and the only
 * callers are the pool status printers the loader never reaches.
 */
#define	pager_output(s)	(printf("%s", (s)), 0)

#endif	/* _BOOT_ZFS_NB_COMPAT_H_ */
