#!/bin/sh
# Scratch: a cached name behind a mode 700 directory on NetBSD's stock zfs.
uname -a
modload zfs; echo "modload rc=$?"; modstat | grep -E '^(zfs|solaris) '
dd if=/dev/zero of=/var/tmp/q.img bs=1m count=256 2>/dev/null
vndconfig vnd0 /var/tmp/q.img; echo "vnd rc=$?"
zpool create -f -m /tank tank /dev/vnd0d; echo "zpool rc=$?"
useradd -m -s /bin/sh ou
mkdir /tank/pl; chmod 700 /tank/pl; echo pub > /tank/pl/p; chmod 644 /tank/pl/p
stat -f '%Sp %N' /tank/pl /tank/pl/p
su -m ou -c 'stat -f %Sp /tank/pl/p'; echo "rc=$? stat"
su -m ou -c 'cat /tank/pl/p'; echo "rc=$? cat"
mkdir /var/tmp/ffs; chmod 700 /var/tmp/ffs; echo pub > /var/tmp/ffs/p; chmod 644 /var/tmp/ffs/p
cat /var/tmp/ffs/p >/dev/null; su -m ou -c 'cat /var/tmp/ffs/p'; echo "rc=$? cat ffs control"
