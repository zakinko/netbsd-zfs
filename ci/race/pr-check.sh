#!/bin/sh
# Build what the send has to stand on: pr-check.sh module|trunk WORKDIR OUTDIR DIFF
#   module  11.0 kernel without vnd, plus vnd.kmod from the diff
#   trunk   current GENERIC with the diff
set -eu
M=$1; W=$2; OUT=$3; D=$(realpath "$4")
mkdir -p "$W" "$OUT"; cd "$W"
if [ "$M" = trunk ]; then
	curl -fsSL https://codeload.github.com/NetBSD/src/tar.gz/refs/heads/trunk | tar xzf -
	mkdir -p usr; mv src-trunk usr/src
else
	CDN=https://cdn.NetBSD.org/pub/NetBSD/NetBSD-11.0/source/sets
	for s in src gnusrc syssrc sharesrc; do curl -fsSL "$CDN/$s.tgz" | tar xzf -; done
fi
cd usr/src
patch -p1 <"$D"
{ echo 'include "arch/amd64/conf/GENERIC"'
  [ "$M" = trunk ] || echo 'no pseudo-device vnd'
} >sys/arch/amd64/conf/"$MARK"
J=$(nproc); B="./build.sh -U -m amd64 -j $J -O $W/obj -T $W/tools"
[ -x "$W/tools/bin/nbmake-amd64" ] || $B tools >"$OUT/tools.log" 2>&1 || { tail -40 "$OUT/tools.log"; exit 1; }
$B -u kernel="$MARK" >"$OUT/kernel.log" 2>&1 || { tail -40 "$OUT/kernel.log"; exit 1; }
cp "$W/obj/sys/arch/amd64/compile/$MARK/netbsd" "$OUT/netbsd"
if [ "$M" = module ]; then
	(cd sys/modules/vnd && "$W/tools/bin/nbmake-amd64" -j "$J" MKDEBUG=no dependall) >"$OUT/module.log" 2>&1 || { tail -40 "$OUT/module.log"; exit 1; }
	# No obj target was run, so it may have landed beside the source.
	k=$(find "$W/obj/sys/modules/vnd" sys/modules/vnd -name vnd.kmod 2>/dev/null | head -1)
	[ -n "$k" ] || { tail -40 "$OUT/module.log"; exit 1; }
	cp "$k" "$OUT/vnd.kmod"
fi
sha256sum "$OUT"/netbsd
