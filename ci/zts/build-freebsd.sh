#!/bin/sh
# Scratch: build the same OpenZFS commit on FreeBSD 15.0 for the control run.
#   sh build-freebsd.sh <commit> <out dir>
set -eu
SHA=$1; OUT=$2; W=/var/tmp/zb
mkdir -p "$W" "$OUT"; cd "$W"
fetch -q -o src.txz https://download.freebsd.org/releases/amd64/15.0-RELEASE/src.txz
tar xJf src.txz -C / usr/src/sys usr/src/include 2>/dev/null || true
git init -q zfs; cd zfs
git fetch -q --depth 1 https://github.com/zakinko/zfs "$SHA"; git checkout -q FETCH_HEAD
git log -1 --format='%H %cs %s'
(sh autogen.sh && MAKE=gmake ./configure --with-config=all && gmake -j4 &&
    gmake install DESTDIR="$W/inst") > "$W/b.log" 2>&1 || { grep -n ' error' "$W/b.log" | head; tail -5 "$W/b.log"; exit 1; }
tar czf "$OUT/fb-inst.tgz" -C "$W/inst" .
cp module/openzfs.ko "$OUT/"
ls -l "$OUT"
