#!/bin/sh
# Scratch: build the OpenZFS NetBSD port, its tests included, against 11.0 sys.
#   sh build-netbsd.sh <commit> <out dir>
set -eu
SHA=$1; OUT=$2; W=/var/tmp/zb
export TERM=vt100
mkdir -p "$W" "$OUT"; cd "$W"
ftp -o sys.tgz https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.0/source/sets/syssrc.tgz
mkdir nb; tar xzf sys.tgz -C nb
git init -q zfs; cd zfs
git fetch -q --depth 1 https://github.com/zakinko/zfs "$SHA"; git checkout -q FETCH_HEAD
git log -1 --format='%H %cs %s'
sh autogen.sh > "$W/ub.log" 2>&1
PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig PKG_CONFIG_PATH= MAKE=gmake \
    ./configure --with-config=user --prefix=/usr --exec-prefix= \
    --sysconfdir=/etc --localstatedir=/var --disable-nls \
    --with-mounthelperdir=/sbin >> "$W/ub.log" 2>&1 || { tail -30 "$W/ub.log"; exit 1; }
gmake -j4 >> "$W/ub.log" 2>&1 || { grep -nE ' error|Error ' "$W/ub.log" | head -40; exit 1; }
gmake install DESTDIR="$W/inst" >> "$W/ub.log" 2>&1
mkdir "$W/kobj"; cd module
make -f Makefile.netbsd S="$W/nb/usr/src/sys" MAKEOBJDIR="$W/kobj" MKDEBUG=no MKCTF=no -j4 > "$W/kb.log" 2>&1 ||
    { grep -n ' error' "$W/kb.log" | head -20; exit 1; }
ls "$W/inst/usr/share/zfs" "$W/inst/usr/share/zfs/zfs-tests/bin" | wc -l
tar czf "$OUT/nb-inst.tgz" -C "$W/inst" .
cp "$W/kobj/zfs.kmod" "$OUT/"
ls -l "$OUT"
