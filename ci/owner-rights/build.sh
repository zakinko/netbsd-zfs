#!/bin/sh
# build.sh WORK OUT: main at $FBSD_SHA plus the patches, cross built on
# Linux.  The kernel is named OWNERRIGHTS so uname proves it is the one
# running.  The world goes in a chroot on the VM, so 15.1 stays bootable.
set -eu
W=$1; OUT=$2; R=$(dirname "$(realpath "$0")")
mkdir -p "$W" "$OUT"; cd "$W"
git init -q src; cd src
git remote add origin "$FBSD_REPO"
git fetch -q --filter=blob:none origin main
git checkout -q "$FBSD_SHA"
for p in "$R"/patches/*.patch; do patch -f -s -p1 -i "$p"; done
find . -name '*.orig' -delete
printf 'include GENERIC\nident OWNERRIGHTS\n' >sys/amd64/conf/OWNERRIGHTS
cat >"$W/src.conf" <<C
WITHOUT_TESTS=yes
WITHOUT_LIB32=yes
WITHOUT_TOOLCHAIN=yes
WITHOUT_LLDB=yes
WITHOUT_BHYVE=yes
WITHOUT_GAMES=yes
WITHOUT_SENDMAIL=yes
WITHOUT_DEBUG_FILES=yes
C
export MAKEOBJDIRPREFIX="$W/obj"
M="./tools/build/make.py --cross-bindir=/usr/lib/llvm-18/bin TARGET=amd64 TARGET_ARCH=amd64 SRCCONF=$W/src.conf"
J=$(nproc)
$M -n
$M KERNCONF=OWNERRIGHTS buildworld buildkernel -s -j"$J" >"$OUT/build.log" 2>&1 ||
	{ grep -n -E 'error:|\*\*\* \[' "$OUT/build.log" | head -40; tail -40 "$OUT/build.log"; exit 1; }
D="$W/root"; mkdir -p "$D"
$M KERNCONF=OWNERRIGHTS DESTDIR="$D" -DNO_ROOT installworld installkernel -s >"$OUT/install.log" 2>&1 ||
	{ tail -40 "$OUT/install.log"; exit 1; }
cd "$D"
grep -E '^\./boot/kernel' METALOG >k.mtree
grep -v -E '^\./boot/kernel' METALOG >w.mtree
bsdtar --zstd -cf "$OUT/kernel.tar.zst" @k.mtree
bsdtar --zstd -cf "$OUT/world.tar.zst" @w.mtree
cp -R "$W/src/tests/sys/acl" "$OUT/acltests"
ls -la "$OUT"
