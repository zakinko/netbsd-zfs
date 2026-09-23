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
 * Read-only ZFS for the boot loader.
 *
 * The pool is reached through the device libsa has already opened, so a
 * pool whose top-level vdev is a single disk or partition can be read;
 * a mirror or raidz spans devices this layer never sees.  zfsimpl.c can
 * reconstruct those, and doing so needs the loader to enumerate the
 * other disks first, which is a separate piece of work.
 *
 * zfsimpl.c is included rather than linked, as it is on FreeBSD: its
 * entry points are all static and the file is meant to be read that way.
 */
#include <sys/param.h>

#include <lib/libkern/libkern.h>

#include "stand.h"
#include "zfs.h"

#include <nvlist.h>
#include <zfsimpl.h>

nvlist_t *vdev_read_bootenv(vdev_t *);

/*
 * zfsimpl.c asks for the size of the vdev this way.  Two of the four
 * labels sit at the end of it, so a wrong answer here loses them.
 */
static uint64_t
ldi_get_size(void *priv)
{
	struct open_file *f = priv;
	uint64_t size = 0;

	if (DEV_IOCTL(f->f_dev)(f, SAIODEVSIZE, &size) != 0)
		return 0;

	return size;
}

#include <zfsimpl.c>

static boolean_t zfs_initialised;

/*
 * zfsimpl.c keeps the vdev it is given, so probing the same one twice
 * fails.  The loader reads boot.cfg and then the kernel, so the pool is
 * opened more than once and the probe has to happen only the first time.
 */
static spa_t *zfs_spa;

struct zfs_file {
	struct zfsmount	zf_mount;
	dnode_phys_t	zf_dnode;
	uint64_t	zf_objnum;
	off_t		zf_size;
	off_t		zf_off;
};

/*
 * Hand zfsimpl.c a read of the vdev.  The offset is relative to the
 * start of the vdev, which is where the device libsa opened already
 * counts from, so no partition offset is added here.
 */
static int
zfs_vdev_read(vdev_t *vdev, void *priv, off_t offset, void *buf, size_t size)
{
	struct open_file *f = priv;
	size_t nread;
	u_int secsize;
	int rc;

	(void)vdev;

	/*
	 * The strategy routine counts in the device's own sectors, as the
	 * other file systems here do through GETSECSIZE().
	 */
	secsize = GETSECSIZE(f);
	if ((offset % secsize) != 0 || (size % secsize) != 0)
		return EINVAL;

#if !defined(LIBSA_NO_TWIDDLE)
	twiddle();
#endif
	rc = DEV_STRATEGY(f->f_dev)(f->f_devdata, F_READ,
	    (daddr_t)(offset / secsize), size, buf, &nread);
	if (rc)
		return rc;
	if (nread != size)
		return EIO;

	return 0;
}

/*
 * vdev_probe() keeps the open_file it is handed, and libsa returns that
 * descriptor to the free list as soon as the file is closed, so every
 * leaf of the pool has to be pointed at the current one before it is
 * read.
 *
 * Only after the device has been shown to be the one the pool was found
 * on, though.  The loader offers each file system in turn every device
 * it opens, so this is reached for disks that hold no pool at all, and
 * pointing the pool at one of those sends every later read to the wrong
 * partition -- which reads as zeroes and fails its checksum.
 */
static void
zfs_vdev_reattach(vdev_t *vdev, struct open_file *f)
{
	vdev_t *kid;

	STAILQ_FOREACH(kid, &vdev->v_children, v_childlink)
		zfs_vdev_reattach(kid, f);

	if (vdev->v_phys_read != NULL)
		vdev->v_priv = f;
}

/*
 * Is this the device the pool was found on?  The first label sits at the
 * start of the vdev and does not move, so its leading sector identifies
 * the device well enough to tell it from any other the loader offers.
 */
static boolean_t
zfs_same_device(struct open_file *f)
{
	static uint8_t known[DEV_BSIZE];
	static boolean_t have_known;
	uint8_t seen[DEV_BSIZE];

	if (zfs_vdev_read(NULL, f, 0, seen, sizeof(seen)) != 0)
		return B_FALSE;

	if (!have_known) {
		memcpy(known, seen, sizeof(known));
		have_known = B_TRUE;
		return B_TRUE;
	}

	return memcmp(known, seen, sizeof(known)) == 0 ? B_TRUE : B_FALSE;
}

__compactcall int
zfs_open(const char *path, struct open_file *f)
{
	struct zfs_file *zf;
	struct stat sb;
	spa_t *spa;
	int rc;

	/*
	 * zfsimpl.c builds the CRC64 table that zap_hash() needs, and
	 * takes its dnode cache buffer, here.  Nothing else calls it, and
	 * without the table every name hashes to the same place and no
	 * entry is ever found.
	 */
	if (!zfs_initialised) {
		zfs_init();
		zfs_initialised = B_TRUE;
	}

	if ((zf = alloc(sizeof(*zf))) == NULL)
		return ENOMEM;
	memset(zf, 0, sizeof(*zf));

	/*
	 * vdev_probe() reads the labels and builds the pool out of what it
	 * finds there.  A pool spread over several devices comes back
	 * short of its members, and the mount below is what then fails.
	 */
	if (zfs_spa == NULL) {
		if ((rc = vdev_probe(zfs_vdev_read, NULL, f, &spa)) != 0)
			goto out;
		if ((rc = zfs_spa_init(spa)) != 0)
			goto out;
		zfs_spa = spa;
		(void)zfs_same_device(f);
	}
	if (!zfs_same_device(f)) {
		rc = EINVAL;
		goto out;
	}
	spa = zfs_spa;
	zfs_vdev_reattach(spa->spa_root_vdev, f);

	/* Object number 0 asks for the pool's own root dataset. */
	if ((rc = zfs_mount_impl(spa, 0, &zf->zf_mount)) != 0)
		goto out;

	if ((rc = zfs_lookup(&zf->zf_mount, path, &zf->zf_dnode,
	    &zf->zf_objnum)) != 0)
		goto out;

	/*
	 * The size is wanted on every read, and nothing writes to the pool
	 * while the loader runs, so ask for it once here.
	 */
	rc = zfs_dnode_stat(zf->zf_mount.spa, &zf->zf_dnode, &sb,
	    zf->zf_mount.fsid_guid, zf->zf_objnum);
	if (rc)
		goto out;

	zf->zf_size = sb.st_size;
	zf->zf_off = 0;
	f->f_fsdata = zf;

	return 0;
out:
	dealloc(zf, sizeof(*zf));
	return rc;
}

__compactcall int
zfs_close(struct open_file *f)
{
	struct zfs_file *zf = f->f_fsdata;

	/*
	 * zfsimpl.c caches the last dnode it read, keyed by an object
	 * number that means nothing once this file is gone.  Leaving it
	 * set hands the next open stale bytes for a different object.
	 */
	dnode_cache_obj = NULL;

	f->f_fsdata = NULL;
	if (zf != NULL)
		dealloc(zf, sizeof(*zf));

	return 0;
}

__compactcall int
zfs_read(struct open_file *f, void *start, size_t size, size_t *resid)
{
	struct zfs_file *zf = f->f_fsdata;
	size_t n = size;
	int rc;

	if (zf->zf_off < 0 || zf->zf_off >= zf->zf_size)
		n = 0;
	else if ((off_t)(zf->zf_off + n) > zf->zf_size)
		n = zf->zf_size - zf->zf_off;

	if (n != 0) {
#if !defined(LIBSA_NO_TWIDDLE)
		twiddle();
#endif
		rc = dnode_read(zf->zf_mount.spa, &zf->zf_dnode, zf->zf_off,
		    start, n);
		if (rc)
			return rc;
		zf->zf_off += n;
	}

	if (resid)
		*resid = size - n;

	return 0;
}

__compactcall int
zfs_write(struct open_file *f, void *start, size_t size, size_t *resid)
{

	(void)f;
	(void)start;
	(void)size;
	(void)resid;

	return EROFS;
}

#if !defined(LIBSA_NO_FS_SEEK)
__compactcall off_t
zfs_seek(struct open_file *f, off_t offset, int where)
{
	struct zfs_file *zf = f->f_fsdata;

	switch (where) {
	case SEEK_SET:
		zf->zf_off = offset;
		break;
	case SEEK_CUR:
		zf->zf_off += offset;
		break;
	case SEEK_END:
		zf->zf_off = zf->zf_size - offset;
		break;
	default:
		return -1;
	}

	return zf->zf_off;
}
#endif /* !defined(LIBSA_NO_FS_SEEK) */

__compactcall int
zfs_stat(struct open_file *f, struct stat *sb)
{
	struct zfs_file *zf = f->f_fsdata;

	return zfs_dnode_stat(zf->zf_mount.spa, &zf->zf_dnode, sb,
	    zf->zf_mount.fsid_guid, zf->zf_objnum);
}

#if defined(LIBSA_ENABLE_LS_OP)
__compactcall void
zfs_ls(struct open_file *f, const char *pattern)
{

	(void)f;
	(void)pattern;

	printf("zfs: ls is not implemented\n");
}
#endif
