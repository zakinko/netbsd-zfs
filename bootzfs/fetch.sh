#!/bin/sh
#
# Fetch the ZFS sources the boot loader needs into a NetBSD source tree.
#
# They come from five places.  FreeBSD keeps a copy of the on-disk format
# with the write support removed, deliberately not shared with its own ZFS
# so that re-importing from upstream stays easy, and that copy is what a
# boot loader wants.  The rest fills the gaps.
#
# NetBSD's own external/cddl/osnet cannot supply lz4.c or list.c: the
# first reaches for <sys/zfs_context.h> and the second for osnet's include
# tree, neither of which a standalone program has.
#
# usage: fetch.sh /path/to/src
#
set -e

case "$1" in
"")	echo "usage: $0 /path/to/netbsd/src" >&2; exit 1 ;;
esac

src=$1
dst=$src/sys/external/cddl/boot/zfs
here=$(cd "$(dirname "$0")" && pwd)

test -d "$src/sys/lib/libsa" || {
	echo "$src does not look like a NetBSD source tree" >&2
	exit 1
}

mkdir -p "$dst/sys"

FB=https://raw.githubusercontent.com/freebsd/freebsd-src/main
OZ=https://raw.githubusercontent.com/openzfs/zfs/master

get() {
	echo "  $2"
	curl -sSf -o "$dst/$2" "$1/$3"
}

echo "FreeBSD sys/cddl/boot/zfs:"
for f in zfsimpl.h zfssubr.c sha256.c fletcher.c lzjb.c zle.c gzip.c \
    blkptr.c; do
	get $FB $f sys/cddl/boot/zfs/$f
done

echo "FreeBSD stand/libsa/zfs:"
for f in zfsimpl.c nvlist.c nvlist.h; do
	get $FB $f stand/libsa/zfs/$f
done

echo "FreeBSD sys/cddl/contrib/opensolaris/common/lz4:"
for f in lz4.c lz4.h; do
	get $FB $f sys/cddl/contrib/opensolaris/common/lz4/$f
done

echo "OpenZFS:"
get $OZ list.c module/os/freebsd/spl/list.c
get $OZ sys/list.h include/os/freebsd/spl/sys/list.h
get $OZ sys/list_impl.h include/os/freebsd/spl/sys/list_impl.h
get $OZ sys/zfs_bootenv.h include/sys/zfs_bootenv.h

# Boot environments are not carried, so the header they hang off is empty.
printf '/* NetBSD has no boot environment support yet. */\n' \
    > "$dst/sys/zfs_bootenv_os.h"

echo "shims and the libsa layer:"
for f in nb_compat.h assert.h stdbool.h; do
	echo "  $f"
	cp "$here/src/$f" "$dst/$f"
done
for f in zfs.c zfs.h zfs_compat.c; do
	echo "  sys/lib/libsa/$f"
	cp "$here/src/$f" "$src/sys/lib/libsa/$f"
done

echo
echo "now run: $here/adjust.sh $src"
