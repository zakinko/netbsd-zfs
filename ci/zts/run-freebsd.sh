#!/bin/sh
# Scratch: the same runfile on FreeBSD 15.0, after the reboot into the same
# OpenZFS commit's zfs.ko.
#   sh run-freebsd.sh <ci/zts dir> [runfile name]
set -u
HERE=$1; RUN=${2:-acl-lookup}
export PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
uname -a; id -un; df -h /var/tmp | tail -1; sysctl -n hw.physmem
kldstat | grep -i zfs; sysctl vfs.zfs.version.module; hash -r 2>/dev/null; which zfs zpool; zfs version
Z=/usr/local/share/zfs
cp "$HERE"/*.run $Z/runfiles/
pw usershow zts >/dev/null 2>&1 || pw useradd zts -m -G wheel -s /bin/sh
printf 'zts ALL=(ALL) NOPASSWD:SETENV: ALL\n' > /usr/local/etc/sudoers.d/zts
su -m zts -c "cd /var/tmp && PATH=$PATH $Z/zfs-tests.sh -v -f -x -s 4G -r $RUN"
rc=$?; echo "zfs-tests.sh rc=$rc"
exit 0
