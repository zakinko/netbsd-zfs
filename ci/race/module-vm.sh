#!/bin/sh
# In a kernel without vnd: load the module, hammer, use it, unload, again.
set -ex
cd /var/tmp/race; cc -O2 -pthread -o race race.c
! modstat | grep -qw '^vnd'
for i in 1 2; do
	modload /var/tmp/race/vnd.kmod
	modstat | grep -w '^vnd'
	./race 100000000 /dev/vnd0a /dev/vnd0d /dev/rvnd0a /dev/rvnd0d /dev/vnd0e /dev/rvnd0e
	dd if=/dev/zero of=/var/tmp/img bs=1m count=32
	newfs -F -s 32m /var/tmp/img | tail -1
	vnconfig vnd0 /var/tmp/img
	mount /dev/vnd0d /mnt; echo hello >/mnt/x; sync; umount /mnt
	# Configured: the unload is refused and the module still works.
	mount /dev/vnd0d /mnt
	! modunload vnd
	echo written >/mnt/z; sync; cat /mnt/x /mnt/z
	umount /mnt
	vnconfig -u vnd0
	modunload vnd
done
echo MODULE OK
