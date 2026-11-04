#!/bin/sh
# in-vm.sh SECONDS DRIVER...  Hammer unconfigured units; then, with a
# unit configured, check the driver still does its job.
S=$1; shift
cd /var/tmp/race; cc -O2 -pthread -o race race.c || exit 1
uname -v
for d in "$@"; do
	echo "### $d start"
	sync
	./race "$S" /dev/${d}0a /dev/${d}0d /dev/r${d}0a /dev/r${d}0d /dev/${d}0e /dev/r${d}0e
	echo "### $d survived rc=$?"
done
[ "${REGRESS:-}" ] || exit 0
set -ex
dd if=/dev/zero of=/var/tmp/img bs=1m count=32
vnconfig vnd0 /var/tmp/img
newfs -q /dev/rvnd0a 2>&1 | tail -1 || newfs /dev/rvnd0d
disklabel vnd0 >/dev/null
./race 10 /dev/vnd0d /dev/rvnd0d /dev/vnd0a /dev/rvnd0a
mount /dev/vnd0a /mnt; echo hello >/mnt/x; sync; umount /mnt
mount -r /dev/vnd0a /mnt; cat /mnt/x; umount /mnt
vnconfig -u vnd0
vnconfig -l | head -2
dd if=/dev/urandom of=/var/tmp/key bs=32 count=1
dd if=/dev/zero of=/var/tmp/img2 bs=1m count=8
vnconfig vnd1 /var/tmp/img2
cgdconfig -g -k storedkey -o /var/tmp/cgd.conf aes-cbc 256
cgdconfig cgd0 /dev/vnd1d /var/tmp/cgd.conf
./race 10 /dev/cgd0d /dev/rcgd0d
dd if=/dev/zero of=/dev/rcgd0d bs=64k count=4
cgdconfig -u cgd0
vnconfig -u vnd1
echo REGRESSION OK
