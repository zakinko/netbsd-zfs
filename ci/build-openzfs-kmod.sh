#!/bin/sh
#
# Build OpenZFS's zfs.kmod against the kernel source in $SRC with the
# cross tools build.sh made, so that it matches the kernel in the
# release.
#
#   sh ci/build-openzfs-kmod.sh <openzfs source>
#
# Leaves the module in $WORK/openzfs-kmod/zfs.kmod.

set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$TOP/nbsrc/src}
WORK=${WORK:-$TOP/nbsrc}
OZ=$(cd "${1:?openzfs source}" && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

mkdir -p "$WORK/openzfs-kmod"
cd "$OZ/module"
"$WORK/tools/bin/nbmake-amd64" -f Makefile.netbsd S="$SRC/sys" \
    MAKEOBJDIR="$WORK/openzfs-kmod" MKDEBUG=no MKCTF=no -j"$JOBS"
ls -l "$WORK/openzfs-kmod/zfs.kmod"
