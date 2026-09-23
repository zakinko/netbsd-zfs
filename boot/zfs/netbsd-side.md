# What the NetBSD tree needs besides these files

Written against NetBSD 11.0. None of it is in the tree; this is the list
so that the next person does not have to find it again.

The four changes outside `sys/lib/libsa` are in `netbsd-side.diff`,
which `install.sh` applies; what follows says what each one is for.

## `sys/lib/libsa/saioctl.h`

```c
#define	SAIODEVSIZE	(('d'<<8)|16)	/* get device size in bytes */
```

Two of a vdev's four labels sit at the end of the device (§1.2.1), and
nothing in libsa could ask how long one was.

## `sys/arch/i386/stand/lib/biosdisk.c`

`biosdisk_ioctl()` was one line returning `EIO`. It answers `SAIODEVSIZE`
with `d->size * d->ll.secsize`.

And `biosdisk_open_name()` — the path taken for `NAME=`, which is how a
GPT wedge is named — had the size in hand and passed it only to
`bi_wedge.nblks`, never to `d->size`. Without that line the ioctl above
answers zero and the two labels at the end are unreachable.

```c
 	d->boff = offset;
+	d->size = size;
```

## `sys/lib/libsa/Makefile`

```make
SA_INCLUDE_ZFS?= no
.if (${SA_INCLUDE_ZFS} == "yes")
SRCS+=	zfs.c zfsread.c zfs_zap.c zfs_dsl.c zfs_nvlist.c \
	zfs_sha256.c zfs_fletcher.c zfs_lz4.c zfs_gzip.c \
	zfs_zle.c zfs_scratch.c
ZFSZLIBDIR:=	${.PARSEDIR}/../../../common/dist/zlib
CPPFLAGS.zfs_gzip.c+= -DZFS_SUPPORT_GZIP -I${ZFSZLIBDIR}
.endif
```

Nothing else: no force-included compatibility header, no warnings
turned off, no include paths into another tree. The files compile with
the flags libsa already uses, including `-Werror`.

## `sys/arch/i386/stand/efiboot/Makefile.efiboot`

```make
CPPFLAGS+= -DSUPPORT_ZFS
SAMISCMAKEFLAGS+="SA_INCLUDE_ZFS=yes"
```

## `sys/arch/i386/stand/efiboot/conf.c`

`FS_OPS(zfs)` in `file_system[]` and in `file_system_disk[]`, guarded by
`SUPPORT_ZFS`, with `FS_OPS(null)` in the template so that a
`__CTASSERT` on the two tables' lengths still holds.

## `sys/arch/i386/stand/efiboot/efiboot.c`

The heap goes from 1MB to 4MB under `SUPPORT_ZFS`. The reader asks for
about 900KB of scratch when a pool is first opened and 128KB per open
file; 1MB is not enough and 4MB leaves room.

## The BIOS loader is a different question

`sys/arch/i386/stand/boot` has a 192KB heap between `HEAP_START` 0x40000
and `HEAP_LIMIT` 0x70000, under the 640KB line. A reader that must hold
a 128KB block does not fit there, and moving that heap is not something
to do on the way past. So this is the EFI loader only.
