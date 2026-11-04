#!/bin/sh
# Scratch: suiddir on FreeBSD with OpenZFS from zakinko/zfs.
set -u
W=/var/tmp/z; mkdir -p $W; cd $W || exit 1
fetch -q -o src.txz https://download.freebsd.org/releases/amd64/15.0-RELEASE/src.txz || exit 1
tar xJf src.txz -C / usr/src/sys usr/src/include 2>/dev/null; ls /usr/src/sys | head -2
git clone -q --depth 1 -b aclownerdeny https://github.com/zakinko/zfs zfs; git -C zfs log -1 --format='%h %s'
cd zfs || exit 1
(sh autogen.sh && MAKE=gmake ./configure --with-config=all && gmake -j4 && gmake install) > $W/b.log 2>&1; echo "build rc=$?"; tail -3 $W/b.log
ls -l module/openzfs.ko || { grep -n ' error' $W/b.log | head; exit 1; }
# The VM boots from ZFS, so zfs.ko cannot be unloaded: replace it and reboot.
cp module/openzfs.ko /boot/kernel/zfs.ko && kldxref /boot/kernel; echo "replace rc=$?"
sync; (sleep 3; shutdown -r now) >/dev/null 2>&1 &
exit 0
