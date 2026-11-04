#!/bin/sh
#
# Cross build a NetBSD 11.0 amd64 kernel on Linux from the release's source
# sets.  The configuration is GENERIC under another name, so that uname(1)
# tells it from the kernel in the image; the modules of the release must
# still load into it.
#
#   kernel-build.sh WORKDIR OUTDIR

set -eu

W=$1
OUT=$2
REL=11.0
CDN=https://cdn.NetBSD.org/pub/NetBSD/NetBSD-$REL/source/sets
CONF=${MARK:-M0SWAP}

mkdir -p "$W" "$OUT"
cd "$W"
for s in src gnusrc syssrc sharesrc; do
	curl -fsSL "$CDN/$s.tgz" | tar xzf -
done
cd usr/src

cat >sys/arch/amd64/conf/"$CONF" <<CONF
include "arch/amd64/conf/GENERIC"
CONF

J=$(nproc)
if [ ! -x "$W/tools/bin/nbmake-amd64" ]; then
	./build.sh -U -m amd64 -j "$J" -O "$W/obj" -T "$W/tools" tools \
	    >"$OUT/tools.log" 2>&1 || { tail -40 "$OUT/tools.log"; exit 1; }
fi
./build.sh -U -m amd64 -j "$J" -O "$W/obj" -T "$W/tools" -u kernel="$CONF" \
    >"$OUT/kernel.log" 2>&1 || { tail -40 "$OUT/kernel.log"; exit 1; }

cp "$W/obj/sys/arch/amd64/compile/$CONF/netbsd" "$OUT/netbsd"
ls -l "$OUT/netbsd"
sha256sum "$OUT/netbsd"
