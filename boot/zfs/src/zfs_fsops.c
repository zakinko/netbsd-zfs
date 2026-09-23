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
 * Read-only ZFS for the boot loader: the libsa side.
 *
 * The reader proper is in zfsread.c and below, written from the ZFS
 * On-Disk Specification.  This file is only the join between it and
 * libsa's struct fs_ops, and holds everything that is about the loader
 * rather than about the format.
 *
 * The pool is reached through the device libsa has already opened, so a
 * pool whose top-level vdev is one disk or partition can be read; a
 * mirror or raidz spans devices this layer never sees, and reading
 * those needs the loader to enumerate disks first, which is its own
 * piece of work.
 *
 * The pool is opened again for every file rather than cached with the
 * device it was found on.  That costs one label read per open, and it
 * removes the failure this went wrong with before: libsa offers every
 * file system every device it opens, and hands the same struct
 * open_file back to the free list afterwards, so a pool that remembers
 * a device ends up reading somebody else's partition through a stale
 * pointer.  A label read is cheap; a pool pointed at the wrong disk
 * reads plausible rubbish.
 */

#include <sys/param.h>

#include <lib/libkern/libkern.h>

#include "stand.h"
#include "zfs.h"

#include "zfs_ondisk.h"
#include "zfsread.h"
#include "scratch.h"

struct zfs_file {
	struct zfs_pool		zf_pool;
	struct zfs_dataset	zf_ds;
	dnode_phys_t		zf_dnode;
	struct zfs_blkcache	zf_cache;
	uint64_t		zf_size;
	uint64_t		zf_off;
};

/*
 * The reader asks for bytes from the start of the vdev; libsa's
 * strategy routine counts in the device's own sectors, as the other
 * file systems here do through GETSECSIZE().
 */
static int
zfs_dev_read(void *cookie, uint64_t off, void *buf, size_t size)
{
	struct open_file *f = cookie;
	size_t nread;
	u_int secsize;
	int rc;

	secsize = GETSECSIZE(f);
	if ((off % secsize) != 0 || (size % secsize) != 0)
		return EINVAL;

#if !defined(LIBSA_NO_TWIDDLE)
	twiddle();
#endif
	rc = DEV_STRATEGY(f->f_dev)(f->f_devdata, F_READ,
	    (daddr_t)(off / secsize), size, buf, &nread);
	if (rc)
		return rc;
	if (nread != size)
		return EIO;

	return 0;
}

/*
 * [S] §1.2.1 puts two of the four labels at the end of the vdev, so the
 * size has to be known before they can be read.  libsa has no call for
 * it, so one was added: SAIODEVSIZE, answered by biosdisk_ioctl().
 */
static uint64_t
zfs_dev_size(struct open_file *f)
{
	uint64_t size = 0;

	if (DEV_IOCTL(f->f_dev)(f, SAIODEVSIZE, &size) != 0)
		return 0;
	return size;
}

/*
 * A path here is either "/path" in the pool's boot dataset, or
 * "dataset:/path" naming one.  The loader's own boot.cfg lives in the
 * former, so the common case needs no dataset name.
 */
static int
zfs_split_path(const char *path, char *ds, size_t dslen, const char **rest)
{
	const char *colon;
	size_t n;

	for (colon = path; *colon != '\0' && *colon != ':'; colon++)
		if (*colon == '/')
			break;

	if (*colon != ':') {
		*rest = path;
		ds[0] = '\0';
		return 0;
	}

	n = (size_t)(colon - path);
	if (n >= dslen)
		return ENAMETOOLONG;
	memcpy(ds, path, n);
	ds[n] = '\0';
	*rest = colon + 1;
	return 0;
}

/*
 * The reader needs room for a few whole blocks at once -- see
 * scratch.h -- and takes it from the loader's heap the first time a
 * pool is opened, so that a loader linked with this but never given a
 * ZFS disk pays nothing for it.
 */
static int
zfs_scratch_setup(void)
{
	static void *arena;

	if (arena != NULL)
		return 0;
	if ((arena = alloc(ZFS_SCRATCH_SIZE)) == NULL)
		return ENOMEM;
	return zfs_scratch_init(arena, ZFS_SCRATCH_SIZE);
}

__compactcall int
zfs_open(const char *path, struct open_file *f)
{
	struct zfs_file *zf;
	char ds[64];
	const char *rest;
	int rc;

	if ((rc = zfs_scratch_setup()) != 0)
		return rc;

	if ((zf = alloc(sizeof(*zf))) == NULL)
		return ENOMEM;
	memset(zf, 0, sizeof(*zf));

	zf->zf_pool.pool_read = zfs_dev_read;
	zf->zf_pool.pool_cookie = f;
	zf->zf_pool.pool_size = zfs_dev_size(f);
	if (zf->zf_pool.pool_size == 0) {
		dealloc(zf, sizeof(*zf));
		return EINVAL;
	}

	/*
	 * This is also the test of whether the device holds a pool at
	 * all, which is what libsa's open() is really asking: the label's
	 * nvlist has to parse and an uberblock has to pass SHA-256.
	 */
	rc = zfs_pool_open(&zf->zf_pool, NULL, 0);
	if (rc != 0) {
		dealloc(zf, sizeof(*zf));
		return rc;
	}

	rc = zfs_split_path(path, ds, sizeof(ds), &rest);
	if (rc != 0) {
		dealloc(zf, sizeof(*zf));
		return rc;
	}

	/*
	 * With no dataset named, the pool's own root dataset is used.
	 * A pool whose files live in a child dataset -- which is how
	 * NetBSD's installer lays one out -- needs the name, or the
	 * bootfs property, which is not read yet.
	 */
	rc = zfs_mount(&zf->zf_pool, ds, &zf->zf_ds);
	if (rc != 0) {
		dealloc(zf, sizeof(*zf));
		return rc;
	}

	rc = zfs_lookup(&zf->zf_ds, rest, &zf->zf_dnode);
	if (rc != 0) {
		dealloc(zf, sizeof(*zf));
		return rc;
	}

	zf->zf_size = zfs_size(&zf->zf_dnode);

	/*
	 * One block of the file kept between reads.  If the heap cannot
	 * spare it the file is still readable, only slowly, so this is
	 * not a failure.
	 */
	zf->zf_cache.bc_buf = alloc(ZFS_MAXBLOCKSIZE);

	f->f_fsdata = zf;
	return 0;
}

__compactcall int
zfs_close(struct open_file *f)
{
	struct zfs_file *zf = f->f_fsdata;

	f->f_fsdata = NULL;
	if (zf != NULL) {
		if (zf->zf_cache.bc_buf != NULL)
			dealloc(zf->zf_cache.bc_buf, ZFS_MAXBLOCKSIZE);
		dealloc(zf, sizeof(*zf));
	}
	return 0;
}

__compactcall int
zfs_read(struct open_file *f, void *start, size_t size, size_t *resid)
{
	struct zfs_file *zf = f->f_fsdata;
	size_t got = 0;
	int rc = 0;

	if (size > 0) {
		rc = zfs_read_file(&zf->zf_ds, &zf->zf_dnode, zf->zf_off,
		    start, size, &got, &zf->zf_cache);
		if (rc == 0)
			zf->zf_off += got;
	}
	if (resid != NULL)
		*resid = size - got;
	return rc;
}

__compactcall int
zfs_write(struct open_file *f, void *start, size_t size, size_t *resid)
{

	(void)f; (void)start; (void)size; (void)resid;
	return EROFS;
}

__compactcall off_t
zfs_seek(struct open_file *f, off_t offset, int where)
{
	struct zfs_file *zf = f->f_fsdata;

	switch (where) {
	case SEEK_SET:
		zf->zf_off = (uint64_t)offset;
		break;
	case SEEK_CUR:
		zf->zf_off += (uint64_t)offset;
		break;
	case SEEK_END:
		zf->zf_off = zf->zf_size - (uint64_t)offset;
		break;
	default:
		errno = EOFFSET;
		return -1;
	}
	return (off_t)zf->zf_off;
}

__compactcall int
zfs_stat(struct open_file *f, struct stat *sb)
{
	struct zfs_file *zf = f->f_fsdata;
	uint64_t mode = zfs_mode(&zf->zf_dnode);

	memset(sb, 0, sizeof(*sb));
	sb->st_size = (off_t)zf->zf_size;

	/*
	 * [S] §6.2, Table 15 gives the type in the same four bits as
	 * POSIX, twelve up; the permission bits below are POSIX's own.
	 * See zfs_ondisk.h on why the shift is twelve and not the
	 * thirteen the surrounding prose says.
	 */
	sb->st_mode = (mode_t)(mode & 07777);
	switch (ZFS_MODE_FMT(mode)) {
	case ZFS_IFREG:	sb->st_mode |= S_IFREG;	break;
	case ZFS_IFDIR:	sb->st_mode |= S_IFDIR;	break;
	case ZFS_IFLNK:	sb->st_mode |= S_IFLNK;	break;
	case ZFS_IFCHR:	sb->st_mode |= S_IFCHR;	break;
	case ZFS_IFBLK:	sb->st_mode |= S_IFBLK;	break;
	case ZFS_IFIFO:	sb->st_mode |= S_IFIFO;	break;
	case ZFS_IFSOCK: sb->st_mode |= S_IFSOCK; break;
	}
	return 0;
}

#if defined(LIBSA_ENABLE_LS_OP)
static void
zfs_ls_one(void *arg, const char *name, uint64_t val)
{

	(void)arg;
	printf("%s\n", name);
	(void)val;
}

__compactcall void
zfs_ls(struct open_file *f, const char *pattern)
{
	struct zfs_file *zf = f->f_fsdata;

	/*
	 * [S] §6.2: a directory is a ZAP, so listing one is walking it.
	 * The pattern is ignored for now, as ustarfs's ls does.
	 */
	(void)pattern;
	(void)zap_list(&zf->zf_pool, &zf->zf_dnode, zfs_ls_one, NULL);
}
#endif
