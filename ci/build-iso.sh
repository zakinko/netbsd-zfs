#!/bin/sh
#
# Build the NetBSD/amd64 release and install ISO with the sysinst
# patch applied.  Runs on a Linux or macOS host with the NetBSD src
# tree checked out at $SRC (netbsd-11 branch, see patches/SRC_REV).
#
#   sh ci/build-iso.sh [release|iso-image ...]
#
# Everything the build writes goes under $WORK; a second run with the
# same $WORK is an update build and only rebuilds what changed.

set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$TOP/nbsrc/src}
WORK=${WORK:-$TOP/nbsrc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

# Keep the build small: no X, no compat libs, no debug or tests.
VARS="-V MKDEBUG=no -V MKDEBUGLIB=no -V MKHTML=no -V MKCOMPAT=no
      -V MKX11=no -V MKATF=no -V MKKYUA=no"

targets=${*:-"release iso-image"}

cd "$SRC"
if git apply --check "$TOP/patches/sysinst-zfs-root.diff" 2>/dev/null; then
	git apply "$TOP/patches/sysinst-zfs-root.diff"
elif git apply --check -R "$TOP/patches/sysinst-zfs-root.diff" 2>/dev/null; then
	echo "sysinst patch already applied"
else
	echo "sysinst patch does not apply to $(git rev-parse HEAD)" >&2
	exit 1
fi

for t in $targets; do
	./build.sh -U -u -m amd64 -j"$JOBS" \
	    -O "$WORK/obj" -D "$WORK/dest" -T "$WORK/tools" -R "$WORK/rel" \
	    $VARS "$t"
done
