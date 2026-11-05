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
# An open held across the churn must keep its unit.
sleep 20 </dev/rvnd2d &
./race 15 /dev/vnd2d /dev/rvnd2d /dev/vnd2e /dev/rvnd2e
wait
dd if=/dev/zero of=/var/tmp/img bs=1m count=32
newfs -F -s 32m /var/tmp/img | tail -1
vnconfig vnd0 /var/tmp/img
./race 10 /dev/vnd0d /dev/rvnd0d
mount /dev/vnd0d /mnt; echo hello >/mnt/x; sync; umount /mnt
mount -r /dev/vnd0d /mnt; cat /mnt/x; umount /mnt
vnconfig -u vnd0
vnconfig -l | head -2
# A unit configured and closed must stay so with failing opens around.
vnconfig vnd3 /var/tmp/img
./race 10 /dev/vnd3f /dev/rvnd3f
vnconfig -l | grep '^vnd3: /'
vnconfig -u vnd3
# VNDIOCSET opens a vnd as its backing file, then refuses it: that open
# is what could wait on a lock held across detach.
vnconfig vnd0 /var/tmp/img
./race 25 /dev/vnd1a /dev/rvnd1a /dev/vnd2a /dev/rvnd2a &
i=0; while [ $i -lt 200 ]; do vnconfig vnd1 /dev/vnd0d 2>/dev/null || :; vnconfig -u vnd1 2>/dev/null || :; i=$((i + 1)); done
wait
vnconfig -u vnd0
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
