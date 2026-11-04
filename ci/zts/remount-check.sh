#!/bin/sh
# zfs set, and remount in both directions, must reach the mount.
set -x
PATH=$PATH:/usr/pkg/sbin:/usr/sbin:/sbin
dd if=/dev/zero of=/var/tmp/rmv bs=1m count=256 2>/dev/null
zpool create -f rmt /var/tmp/rmv || exit 1
zfs create rmt/a; touch /rmt/a/f
show() { mount | grep ' /rmt/a '; }
show
zfs set setuid=off rmt/a; show
zfs set atime=off rmt/a; show
zfs set readonly=on rmt/a; show; touch /rmt/a/g; echo "touch rc=$? (want fail)"
zfs set readonly=off rmt/a; show; touch /rmt/a/g; echo "touch rc=$? (want 0)"
zfs mount -o remount,ro rmt/a; echo "remount,ro rc=$?"; show; touch /rmt/a/h; echo "touch rc=$? (want fail)"
zfs mount -o remount,rw rmt/a; echo "remount,rw rc=$?"; show; touch /rmt/a/h; echo "touch rc=$? (want 0)"
zfs snapshot rmt/a@s; mkdir -p /var/tmp/sn
setextattr user k v /rmt/a/f; zfs snapshot rmt/a@s2
ls /rmt/a/.zfs/snapshot/s2/
setextattr user k w /rmt/a/.zfs/snapshot/s2/f; echo "snap set rc=$? (want fail)"
rmextattr user k /rmt/a/.zfs/snapshot/s2/f; echo "snap del rc=$? (want fail)"
getextattr user k /rmt/a/.zfs/snapshot/s2/f
zpool destroy rmt; rm -f /var/tmp/rmv
echo REMOUNT CHECK DONE
