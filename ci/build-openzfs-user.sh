#!/bin/sh
#
# Build the OpenZFS userland natively on NetBSD, against base's libraries
# only, laid out as the release has it: commands in /sbin, libraries in
# /lib.  Also link zpool and mount_zfs statically for the zfsroot ramdisk.
#
#   sh ci/build-openzfs-user.sh <openzfs source> <output dir>
#
# The output dir gets root/ (a DESTDIR tree) and static/{zpool,mount_zfs}.

set -eu

SRC=$(cd "${1:?openzfs source}" && pwd)
mkdir -p "${2:?output dir}"
OUT=$(cd "$2" && pwd)
JOBS=$(sysctl -n hw.ncpu)

cd "$SRC"
sh autogen.sh

# Only base's libraries: pkgsrc's OpenSSL or gettext would put /usr/pkg
# into the commands' RPATH.
PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig PKG_CONFIG_PATH= MAKE=gmake \
    ./configure --with-config=user --prefix=/usr --exec-prefix= \
    --sysconfdir=/etc --localstatedir=/var --disable-nls --disable-pyzfs \
    --with-mounthelperdir=/sbin
gmake -j"$JOBS"
rm -rf "$OUT/root"
gmake install DESTDIR="$OUT/root"

# The ramdisk has no shared libraries.  libtool links a program
# statically with -all-static; libexecinfo needs libelf after it.
rm -f zpool mount_zfs_netbsd
gmake zpool mount_zfs_netbsd LDFLAGS=-all-static LIBS="-lintl -lelf"
mkdir -p "$OUT/static"
strip -o "$OUT/static/zpool" zpool
strip -o "$OUT/static/mount_zfs" mount_zfs_netbsd
file "$OUT/static/zpool" "$OUT/static/mount_zfs"
