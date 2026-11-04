#!/bin/sh
# Cross build GENERIC under the name $MARK from the 11.0 source sets, with
# the diff, on Linux.  kernel.sh WORKDIR OUTDIR DIFF
set -eu
W=$1; OUT=$2; D=$(realpath "$3"); REL=11.0
CDN=https://cdn.NetBSD.org/pub/NetBSD/NetBSD-$REL/source/sets
mkdir -p "$W" "$OUT"; cd "$W"
for s in src gnusrc syssrc sharesrc; do curl -fsSL "$CDN/$s.tgz" | tar xzf -; done
cd usr/src
patch -p1 <"$D"
printf 'include "arch/amd64/conf/GENERIC"\n' >sys/arch/amd64/conf/"$MARK"
J=$(nproc)
[ -x "$W/tools/bin/nbmake-amd64" ] || ./build.sh -U -m amd64 -j "$J" -O "$W/obj" -T "$W/tools" tools >"$OUT/tools.log" 2>&1 || { tail -40 "$OUT/tools.log"; exit 1; }
./build.sh -U -m amd64 -j "$J" -O "$W/obj" -T "$W/tools" -u kernel="$MARK" >"$OUT/kernel.log" 2>&1 || { tail -40 "$OUT/kernel.log"; exit 1; }
cp "$W/obj/sys/arch/amd64/compile/$MARK/netbsd" "$OUT/netbsd"
sha256sum "$OUT/netbsd"
