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
 * The allocator and the two string routines the ZFS sources carried into
 * sys/external/cddl/boot/zfs expect, in terms of what libsa has.
 *
 * libsa offers alloc() and dealloc(), and dealloc() wants the size of the
 * block back.  The carried code frees without one, so each block is asked
 * for a little larger and carries its own size in front of what is handed
 * out.
 */

#include <sys/param.h>
#include <sys/stdarg.h>

#include <lib/libkern/libkern.h>

#include "stand.h"

/* The compat header renames these onto the wrappers below; undo that. */
#undef	malloc
#undef	calloc
#undef	realloc
#undef	free
#undef	asprintf
#undef	strdup

void	*zfs_malloc(size_t);
void	*zfs_calloc(size_t, size_t);
void	*zfs_realloc(void *, size_t);
void	 zfs_free(void *);
int	 zfs_asprintf(char **, const char *, ...) __printflike(2, 3);
char	*zfs_strdup(const char *);

void *
zfs_malloc(size_t size)
{
	size_t *p;

	if ((p = alloc(size + sizeof(*p))) == NULL)
		return NULL;
	*p = size + sizeof(*p);

	return p + 1;
}

void
zfs_free(void *ptr)
{
	size_t *p;

	if (ptr == NULL)
		return;
	p = (size_t *)ptr - 1;
	dealloc(p, *p);
}

void *
zfs_calloc(size_t n, size_t size)
{
	void *p;

	if ((p = zfs_malloc(n * size)) != NULL)
		memset(p, 0, n * size);

	return p;
}

void *
zfs_realloc(void *ptr, size_t size)
{
	size_t old;
	void *new;

	if (ptr == NULL)
		return zfs_malloc(size);

	old = *((size_t *)ptr - 1) - sizeof(size_t);
	if ((new = zfs_malloc(size)) == NULL)
		return NULL;
	memcpy(new, ptr, old < size ? old : size);
	zfs_free(ptr);

	return new;
}

/*
 * The carried code does not look at what asprintf() returns, so a failure
 * has to come back as a NULL pointer rather than as a short string.
 */
int
zfs_asprintf(char **ret, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= sizeof(buf)) {
		*ret = NULL;
		return -1;
	}
	if ((*ret = zfs_malloc((size_t)n + 1)) == NULL)
		return -1;
	memcpy(*ret, buf, (size_t)n + 1);

	return n;
}

char *
zfs_strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *p;

	if ((p = zfs_malloc(len)) != NULL)
		memcpy(p, s, len);

	return p;
}
