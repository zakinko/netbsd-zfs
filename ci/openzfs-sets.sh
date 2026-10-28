#!/bin/sh
#
# Put OpenZFS into the release in place of NetBSD's own ZFS (osnet): the
# commands and libraries into the base set, the manual pages into the man
# set, zfs.kmod into the modules set.  osnet's libavl, libnvpair, libumem
# and libuutil stay, as does solaris.kmod, which dtrace needs.
#
#   sh ci/openzfs-sets.sh <userland output dir> <zfs.kmod>
#
# The userland dir is what build-openzfs-user.sh made.  Members are
# swapped with GNU tar's --delete and --append, so every other file in a
# set keeps the owner, group and mode the build gave it.

set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$TOP/nbsrc}
REL=${REL:-$WORK/rel/amd64}
SETS=$REL/binary/sets
ROOT=$(cd "${1:?userland output dir}/root" && pwd)
KMOD=$(cd "$(dirname "${2:?zfs.kmod}")" && pwd)/$(basename "$2")
T=$WORK/openzfs-sets
rm -rf "$T" && mkdir -p "$T"

# What goes in, by set.  Nothing for Linux (fsck.zfs, vdev_id, the init
# functions), nothing that needs python, and no tests.
(cd "$ROOT" && find ./sbin ./lib ./usr/share/zfs/compatibility.d \
    ./libexec/zfs/zfs_prepare_disk \( -type f -o -type l \) |
    grep -vE '^\./sbin/fsck\.zfs$|^\./lib/security/|^\./lib/pkgconfig/|\.a$|\.la$' |
    sort) > "$T/base.add"
(cd "$ROOT" && find ./usr/share/man/man4 ./usr/share/man/man5 \
    ./usr/share/man/man7 ./usr/share/man/man8 \( -type f -o -type l \) |
    grep -vE '/fsck\.zfs\.8$|/zfs-mount-generator\.8$|/zed\.8$' |
    sort) > "$T/man.add"

# osnet's, out of base; the libraries every other osnet program uses
# stay.  Names the new files take are dropped too, so there are no
# duplicates.
cat > "$T/base.del" <<EOF
./sbin/zfs
./sbin/zpool
./sbin/mount_zfs
./usr/sbin/zdb
./usr/bin/ztest
./lib/libzfs.so
./lib/libzfs.so.0
./lib/libzfs.so.0.0
./lib/libzfs_core.so
./lib/libzfs_core.so.0
./lib/libzfs_core.so.0.0
./lib/libzpool.so
./lib/libzpool.so.0
./lib/libzpool.so.0.0
./usr/lib/libzfs.so
./usr/lib/libzfs.so.0
./usr/lib/libzfs.so.0.0
./usr/lib/libzfs_core.so
./usr/lib/libzfs_core.so.0
./usr/lib/libzfs_core.so.0.0
./usr/lib/libzpool.so
./usr/lib/libzpool.so.0
./usr/lib/libzpool.so.0.0
EOF
cat "$T/base.add" >> "$T/base.del"
# osnet's preformatted and HTML pages of the same names would be stale.
sed -e 's|/man\([0-9]\)/\(.*\)\.[0-9]$|/cat\1/\2.0|' "$T/man.add" > "$T/man.del"
sed -e 's|/man/man\([0-9]\)/\(.*\)\.[0-9]$|/man/html\1/\2.html|' "$T/man.add" \
    >> "$T/man.del"
cat "$T/man.add" >> "$T/man.del"
echo "./stand/amd64/@VER@/modules/zfs/zfs.kmod" > "$T/modules.del"

# The set's file name: .tar.xz, or .tgz with MKXZ=no.
setfile() {
	for f in "$SETS/$1.tar.xz" "$SETS/$1.tgz"; do
		[ -f "$f" ] && { echo "$f"; return; }
	done
	echo "no $1 set in $SETS" >&2
	exit 1
}

# Uncompress set $1 to $T/$1.tar and delete from it the members listed
# in $T/$1.del that it has.
open_set() {
	f=$(setfile "$1")
	case $f in
	*.tar.xz) xz -dc "$f" > "$T/$1.tar" ;;
	*) gzip -dc "$f" > "$T/$1.tar" ;;
	esac
	tar -tf "$T/$1.tar" | sort > "$T/$1.list"
	sort "$T/$1.del" | comm -12 - "$T/$1.list" > "$T/$1.present"
	if [ -s "$T/$1.present" ]; then
		tar --delete -f "$T/$1.tar" -T "$T/$1.present"
	fi
	echo "$1: removed $(wc -l < "$T/$1.present") members"
}

close_set() {
	f=$(setfile "$1")
	case $f in
	*.tar.xz) xz -9 -T0 -c "$T/$1.tar" > "$f" ;;
	*) gzip -9 -n -c "$T/$1.tar" > "$f" ;;
	esac
	rm -f "$T/$1.tar"
}

# Append from directory $2 the names in file $3, owned by root:wheel.
append() {
	(cd "$2" && tar --append -f "$T/$1.tar" --owner=0 --group=0 \
	    --numeric-owner --no-recursion -T "$3")
	echo "$1: added $(wc -l < "$3") members"
}

# ld.elf_so looks in /usr/lib, so every shared library in /lib has a
# link there to its file, as osnet's had (compatfile in the set lists).
mkdir -p "$T/compat/usr/lib"
grep '^\./lib/lib[^/]*\.so' "$T/base.add" | while read -r f; do
	n=${f##*/}
	real=$(cd "$ROOT/lib" && basename "$(readlink -f "$n")")
	ln -s "../../lib/$real" "$T/compat/usr/lib/$n"
	echo "./usr/lib/$n"
done | sort > "$T/compat.add"
cat "$T/compat.add" >> "$T/base.del"

# libexec/zfs and the compatibility.d directory are new to base.
printf '%s\n' ./libexec/zfs ./usr/share/zfs ./usr/share/zfs/compatibility.d \
    > "$T/base.dirs"
open_set base
append base "$ROOT" "$T/base.dirs"
append base "$ROOT" "$T/base.add"
append base "$T/compat" "$T/compat.add"
close_set base

open_set man
append man "$ROOT" "$T/man.add"
close_set man

# The module directory carries the kernel's version, as in the set.
f=$(setfile modules)
case $f in
*.tar.xz) ver=$(xz -dc "$f" | tar -tf - | sed -n 's|^\./stand/amd64/\([^/]*\)/modules/zfs/zfs\.kmod$|\1|p') ;;
*) ver=$(gzip -dc "$f" | tar -tf - | sed -n 's|^\./stand/amd64/\([^/]*\)/modules/zfs/zfs\.kmod$|\1|p') ;;
esac
[ -n "$ver" ] || { echo "no zfs.kmod in the modules set" >&2; exit 1; }
sed -i "s|@VER@|$ver|" "$T/modules.del"
open_set modules
mkdir -p "$T/kroot/stand/amd64/$ver/modules/zfs"
install -m 444 "$KMOD" "$T/kroot/stand/amd64/$ver/modules/zfs/zfs.kmod"
echo "./stand/amd64/$ver/modules/zfs/zfs.kmod" > "$T/modules.add"
append modules "$T/kroot" "$T/modules.add"
close_set modules

# The checksum files list every set; the lines for the three changed
# ones are replaced, the others kept as they are.
for set in base man modules; do
	f=$(basename "$(setfile $set)")
	for sum in MD5 SHA512; do
		[ -f "$SETS/$sum" ] || continue
		case $sum in
		MD5) h=$(md5sum "$SETS/$f" | cut -d' ' -f1) ;;
		SHA512) h=$(sha512sum "$SETS/$f" | cut -d' ' -f1) ;;
		esac
		sed -i "s|^$sum ($f) = .*|$sum ($f) = $h|" "$SETS/$sum"
		grep -q "^$sum ($f) = $h\$" "$SETS/$sum" ||
		    { echo "$sum: no line for $f" >&2; exit 1; }
	done
done
echo "sets updated"
