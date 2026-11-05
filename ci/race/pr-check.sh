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
  if [ "$M" = trunk ]; then
	echo 'options DIAGNOSTIC'; echo 'options DEBUG'; echo 'options LOCKDEBUG'
  else
	echo 'no pseudo-device vnd'
  fi
} >sys/arch/amd64/conf/"$MARK"
J=$(nproc); B="./build.sh -U -m amd64 -j $J -O $W/obj -T $W/tools"
[ -x "$W/tools/bin/nbmake-amd64" ] || $B tools >"$OUT/tools.log" 2>&1 || { tail -40 "$OUT/tools.log"; exit 1; }
$B -u kernel="$MARK" >"$OUT/kernel.log" 2>&1 || { tail -40 "$OUT/kernel.log"; exit 1; }
cp "$W/obj/sys/arch/amd64/compile/$MARK/netbsd" "$OUT/netbsd"
if [ "$M" = trunk ]; then
	# The strictest warnings, for the two files only; the tree as a whole
	# is not clean at this level.  Not -Werror, so all of them are listed.
	(cd "$W/obj/sys/arch/amd64/compile/$MARK" && rm -f vnd.o &&
	 "$W/tools/bin/nbmake-amd64" -n NOGCCERROR=1 \
	    COPTS+='-Wall -Wextra -Wshadow' vnd.o | grep -c -e -Wshadow) || :
	(cd "$W/obj/sys/arch/amd64/compile/$MARK" && rm -f vnd.o cgd.o &&
	 "$W/tools/bin/nbmake-amd64" NOGCCERROR=1 \
	    COPTS+='-Wall -Wextra -Wshadow -Wcast-qual -Wpointer-arith -Wunused -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers' \
	    vnd.o cgd.o) >"$OUT/strict.log" 2>&1 || { tail -40 "$OUT/strict.log"; exit 1; }
	echo "=== strict warnings in vnd.c, vndvar.h, cgd.c"
	grep -E 'dev/(vnd\.c|vndvar\.h|cgd\.c):[0-9]+:[0-9]+: (warning|error)' "$OUT/strict.log" || echo none
	grep -cE '(vnd|cgd)\.c' "$OUT/strict.log" || :
	grep -m1 -E 'Wshadow.*vnd\.c|vnd\.c.*Wshadow|compile.*vnd' "$OUT/strict.log" | cut -c1-200 || :
	ls -l "$W/obj/sys/arch/amd64/compile/$MARK/vnd.o"
	echo "=== end strict"
fi
if [ "$M" = module ]; then
	(cd sys/modules/vnd && "$W/tools/bin/nbmake-amd64" -j "$J" MKDEBUG=no dependall) >"$OUT/module.log" 2>&1 || { tail -40 "$OUT/module.log"; exit 1; }
	# No obj target was run, so it may have landed beside the source.
	k=$(find "$W/obj/sys/modules/vnd" sys/modules/vnd -name vnd.kmod 2>/dev/null | head -1)
	[ -n "$k" ] || { tail -40 "$OUT/module.log"; exit 1; }
	cp "$k" "$OUT/vnd.kmod"
fi
sha256sum "$OUT"/netbsd
