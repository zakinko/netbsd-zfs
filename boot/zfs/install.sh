#!/bin/sh
#
# Put the reader into a NetBSD source tree and wire it up.
#
#	sh install.sh /path/to/usr/src
#
# Everything it touches is listed in netbsd-side.md.  It is written to
# be run twice without harm, so that a tree can be re-patched after a
# fetch.
set -e

SRC=$1
[ -n "$SRC" ] || { echo "usage: $0 <srcdir>" >&2; exit 2; }
[ -d "$SRC/sys/lib/libsa" ] || { echo "$SRC: not a NetBSD source tree" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
LIBSA=$SRC/sys/lib/libsa
EFI=$SRC/sys/arch/i386/stand/efiboot

# The reader, as libsa files.  The generic names are prefixed so that
# nvlist.c, sha256.c and lz4.c cannot collide with libsa's own.
cp "$HERE/src/zfs_ondisk.h" "$LIBSA/zfs_ondisk.h"
cp "$HERE/src/zfsread.c"    "$LIBSA/zfsread.c"
cp "$HERE/src/zfsread.h"    "$LIBSA/zfsread.h"
cp "$HERE/src/zap.c"        "$LIBSA/zfs_zap.c"
cp "$HERE/src/zfsfs.c"      "$LIBSA/zfs_dsl.c"
cp "$HERE/src/nvlist.c"     "$LIBSA/zfs_nvlist.c"
cp "$HERE/src/nvlist.h"     "$LIBSA/nvlist.h"
cp "$HERE/src/sha256.c"     "$LIBSA/zfs_sha256.c"
cp "$HERE/src/sha256.h"     "$LIBSA/sha256.h"
cp "$HERE/src/fletcher.c"   "$LIBSA/zfs_fletcher.c"
cp "$HERE/src/fletcher.h"   "$LIBSA/fletcher.h"
cp "$HERE/src/lz4.c"        "$LIBSA/zfs_lz4.c"
cp "$HERE/src/lz4.h"        "$LIBSA/lz4.h"
cp "$HERE/src/gzip.c"       "$LIBSA/zfs_gzip.c"
cp "$HERE/src/gzip.h"       "$LIBSA/gzip.h"
cp "$HERE/src/zle.c"        "$LIBSA/zfs_zle.c"
cp "$HERE/src/zle.h"        "$LIBSA/zle.h"
cp "$HERE/src/zstd.c"       "$LIBSA/zfs_zstd.c"
cp "$HERE/src/zstd.h"       "$LIBSA/zstd.h"
cp "$HERE/src/scratch.c"    "$LIBSA/zfs_scratch.c"
cp "$HERE/src/scratch.h"    "$LIBSA/scratch.h"
cp "$HERE/src/zfs_fsops.c"  "$LIBSA/zfs.c"

cat > "$LIBSA/zfs.h" <<'H'
/*	$NetBSD$	*/

FS_DEF(zfs);
H

# libsa's Makefile.  The block goes in ahead of <bsd.lib.mk>, because
# SRCS is read when that is included and anything appended after it is
# simply ignored -- the library then builds without a word, the loader
# links, and only the undefined zfs_open at the very end says so.
#
# Any previous copy of the block is dropped first, so that running this
# again does not stack two.
awk '
/^SA_INCLUDE_ZFS\?=/ { next }
/^\.if \(\$\{SA_INCLUDE_ZFS\} == "yes"\)/ { skip = 1; next }
skip && /^\.endif/ { skip = 0; next }
skip { next }
/^\.include <bsd\.lib\.mk>/ && !done {
	print "# Read-only ZFS, written from the ZFS On-Disk Specification."
	print "# Off by default: ten kilobytes of text and a megabyte of"
	print "# scratch that a loader which will never see a pool should"
	print "# not carry."
	print "SA_INCLUDE_ZFS?= no"
	print ".if (${SA_INCLUDE_ZFS} == \"yes\")"
	print "SRCS+=\tzfs.c zfsread.c zfs_zap.c zfs_dsl.c zfs_nvlist.c \\"
	print "\tzfs_sha256.c zfs_fletcher.c zfs_lz4.c zfs_gzip.c \\"
	print "\tzfs_zle.c zfs_zstd.c zfs_scratch.c"
	print "#"
	print "# gzip goes through zlib, which the loader links anyway for"
	print "# gzipped kernels, so this is a call and not a library."
	print "#"
	print "ZFSZLIBDIR:=\t${.PARSEDIR}/../../../common/dist/zlib"
	print "CPPFLAGS.zfs_gzip.c+= -DZFS_SUPPORT_GZIP -I${ZFSZLIBDIR}"
	print ".endif"
	print ""
	done = 1
}
{ print }
' "$LIBSA/Makefile" > "$LIBSA/Makefile.new"
grep -q SA_INCLUDE_ZFS "$LIBSA/Makefile.new" || {
	echo "could not find <bsd.lib.mk> in $LIBSA/Makefile" >&2
	rm -f "$LIBSA/Makefile.new"
	exit 1
}
mv "$LIBSA/Makefile.new" "$LIBSA/Makefile"

# The loader.
grep -q SUPPORT_ZFS "$EFI/Makefile.efiboot" || cat >> "$EFI/Makefile.efiboot" <<'M'

CPPFLAGS+= -DSUPPORT_ZFS
SAMISCMAKEFLAGS+="SA_INCLUDE_ZFS=yes"
M

sh "$HERE/patch-tree.sh" "$SRC"

echo "installed into $SRC"
