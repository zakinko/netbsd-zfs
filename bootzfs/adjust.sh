#!/bin/sh
#
# Adjust the fetched files and the NetBSD tree so that the EFI boot loader
# builds with read-only ZFS.
#
# The fetched files are left as close to their originals as they can be:
# one #include line in zfsimpl.c, two in gzip.c, and the two checksums
# NetBSD has no implementation of.  Everything else is force-fed from
# nb_compat.h, the way FreeBSD force-feeds its own ccompile.h.
#
# usage: adjust.sh /path/to/src
#
set -e

case "$1" in
"")	echo "usage: $0 /path/to/netbsd/src" >&2; exit 1 ;;
esac

src=$1
test -f "$src/sys/external/cddl/boot/zfs/zfsimpl.c" || {
	echo "run fetch.sh first" >&2
	exit 1
}

python3 - "$src" <<'PY'
import sys

SRC = sys.argv[1]
Z = SRC + "/sys/external/cddl/boot/zfs"

def edit(path, pairs):
    s = open(path).read()
    for old, new in pairs:
        if new in s:		# already adjusted
            continue
        if old not in s:
            raise SystemExit("not found in %s:\n%s" % (path, old[:70]))
        s = s.replace(old, new)
    open(path, 'w').write(s)

# NetBSD spells this <machine/int_fmtio.h>.
edit(Z + "/zfsimpl.c", [
  ("#include <machine/_inttypes.h>",
   "#include <machine/int_fmtio.h>\t/* NetBSD spelling */")])

# zlib is not under contrib/ here.
edit(Z + "/gzip.c", [
  ("#include <contrib/zlib/zlib.h>", "#include <zlib.h>"),
  ("#include <contrib/zlib/zutil.h>", "#include <zutil.h>")])

# skein and blake3 have no implementation to carry, so they stand aside
# the way edonr already does a few lines further up.
edit(Z + "/zfssubr.c", [
  ('#include "blake3_zfs.c"', '/* blake3: no implementation here */'),
  ('#include "skein_zfs.c"',  '/* skein: not carried */'),
  ('''	{{zio_checksum_skein_native, zio_checksum_skein_byteswap},
	    zio_checksum_skein_tmpl_init, zio_checksum_skein_tmpl_free,''',
   '''	/* no skein for now */
	{{NULL, NULL}, NULL, NULL,'''),
  ('''	{{zio_checksum_blake3_native,	zio_checksum_blake3_byteswap},
	    zio_checksum_blake3_tmpl_init, zio_checksum_blake3_tmpl_free,''',
   '''	/* no blake3 for now */
	{{NULL, NULL}, NULL, NULL,''')])

# --- the NetBSD side ---

edit(SRC + "/sys/lib/libsa/saioctl.h", [
  ("#define SAIOSECSIZE\t(('d'<<8)|15)\t/* get sector size */",
   "#define SAIOSECSIZE\t(('d'<<8)|15)\t/* get sector size */\n"
   "#define SAIODEVSIZE\t(('d'<<8)|16)\t/* get device size in bytes */")])

# biosdisk_ioctl() answered nothing at all, and biosdisk_open_name() had
# the partition size to hand and dropped it -- d->size was only ever read
# back into bi_wedge.nblks, so nobody noticed it staying zero.
edit(SRC + "/sys/arch/i386/stand/lib/biosdisk.c", [
  ('''biosdisk_ioctl(struct open_file *f, u_long cmd, void *arg)
{
	return EIO;
}''',
   '''biosdisk_ioctl(struct open_file *f, u_long cmd, void *arg)
{
	struct biosdisk *d = f->f_devdata;

	switch (cmd) {
	case SAIODEVSIZE:
		/*
		 * ZFS keeps two of its four labels at the end of the vdev
		 * and cannot find them without this.
		 */
		*(uint64_t *)arg = (uint64_t)d->size * d->ll.secsize;
		return 0;
	default:
		return EIO;
	}
}'''),
  ('''	d->boff = offset;

	bi_wedge.startblk = offset;''',
   '''	d->boff = offset;
	d->size = size;

	bi_wedge.startblk = offset;''')])

# zfsimpl.c takes a SPA_MAXBLOCKSIZE buffer to cache dnodes in, so 1MB of
# heap is not enough.  Only the loader that carries ZFS pays for this.
edit(SRC + "/sys/arch/i386/stand/efiboot/efiboot.c", [
  ("static UINTN heap_size = 1 * 1024 * 1024;\t\t\t/* 1MB */",
   '''#ifdef SUPPORT_ZFS
/*
 * ZFS wants room the other file systems do not: zfsimpl.c takes a
 * SPA_MAXBLOCKSIZE (16MB) buffer to cache dnodes in, and a vdev label's
 * nvlist is VDEV_PHYS_SIZE (112KB) on its own.
 */
static UINTN heap_size = 64 * 1024 * 1024;\t\t\t/* 64MB */
#else
static UINTN heap_size = 1 * 1024 * 1024;\t\t\t/* 1MB */
#endif''')])

edit(SRC + "/sys/arch/i386/stand/efiboot/Makefile.efiboot", [
  ("CPPFLAGS+= -DSUPPORT_EXT2FS\n",
   "CPPFLAGS+= -DSUPPORT_EXT2FS\nCPPFLAGS+= -DSUPPORT_ZFS\n"),
  ('SAMISCMAKEFLAGS+="SA_INCLUDE_NET=yes"\n',
   'SAMISCMAKEFLAGS+="SA_INCLUDE_NET=yes"\n'
   'SAMISCMAKEFLAGS+="SA_INCLUDE_ZFS=yes"\n')])

# file_system[] is the template devopen() fills; file_system_disk[] holds
# the real ones.  A __CTASSERT checks the two are the same length.
edit(SRC + "/sys/arch/i386/stand/efiboot/conf.c", [
  ('''#ifdef SUPPORT_CD9660
#include <lib/libsa/cd9660.h>
#endif''',
   '''#ifdef SUPPORT_CD9660
#include <lib/libsa/cd9660.h>
#endif
#ifdef SUPPORT_ZFS
#include <lib/libsa/zfs.h>
#endif'''),
  ('''#ifdef SUPPORT_DOSFS
	FS_OPS(null),
#endif
};
int nfsys = __arraycount(file_system);''',
   '''#ifdef SUPPORT_DOSFS
	FS_OPS(null),
#endif
#ifdef SUPPORT_ZFS
	FS_OPS(null),
#endif
};
int nfsys = __arraycount(file_system);'''),
  ('''#ifdef SUPPORT_DOSFS
	FS_OPS(dosfs),
#endif
};''',
   '''#ifdef SUPPORT_DOSFS
	FS_OPS(dosfs),
#endif
#ifdef SUPPORT_ZFS
	FS_OPS(zfs),
#endif
};''')])

mk = SRC + "/sys/lib/libsa/Makefile"
s = open(mk).read()
if "SA_INCLUDE_ZFS" not in s:
    s = s.replace("SA_INCLUDE_NET?= yes\t\t# Netboot via TFTP, NFS",
      "SA_INCLUDE_NET?= yes\t\t# Netboot via TFTP, NFS\n"
      "SA_INCLUDE_ZFS?= no\t\t# Read-only ZFS; 6,000 lines, so off by default")
    old = "SRCS+=\tminixfs3.c\nSRCS+=\tfnmatch.c"
    if old not in s:
        raise SystemExit("libsa Makefile is not the shape this expects")
    s = s.replace(old, old + '''

.if (${SA_INCLUDE_ZFS} == "yes")
ZFSBOOTDIR:=	${.PARSEDIR}/../../external/cddl/boot/zfs
ZLIBSRCDIR:=	${.PARSEDIR}/../../../common/dist/zlib
.PATH.c:	${ZFSBOOTDIR}

ZFSSRCS=	zfs.c zfs_compat.c nvlist.c list.c lz4.c
SRCS+=		${ZFSSRCS}

#
# The files carried into ${ZFSBOOTDIR} are force-fed what they reach for,
# the way FreeBSD force-includes its own ccompile.h, so that they need no
# changes of their own.  Only these files see it: nb_compat.h renames
# malloc() and friends, and stdbool.h beside it would shadow the
# compiler's for every other file in libsa.
#
# zfsimpl.c in particular is read whole, so it offers more than the
# loader calls and keeps its own idea of what is static.
#
.for f in ${ZFSSRCS}
CPPFLAGS.${f}+=	-I${ZFSBOOTDIR} -I${ZLIBSRCDIR} -DNEED_SOLARIS_BOOLEAN
CPPFLAGS.${f}+=	-include ${ZFSBOOTDIR}/nb_compat.h
COPTS.${f}+=	-Wno-unused-function -Wno-missing-prototypes
COPTS.${f}+=	${CC_WNO_MAYBE_UNINITIALIZED} -Wno-unknown-pragmas
.endfor
.endif''')
    open(mk, 'w').write(s)

print("adjusted")
PY

echo
echo "now build: cd $src/sys/arch/i386/stand/efiboot && make USETOOLS=no"
