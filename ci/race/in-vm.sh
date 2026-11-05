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
echo '--- units left'; drvctl -l | grep -c vnd || :; drvctl -l | grep vnd | head
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
# A failed configuration must leave nothing to the next one.
dd if=/dev/urandom of=/var/tmp/junk bs=1m count=1
if vnconfig -z vnd0 /var/tmp/junk; then echo 'junk accepted'; vnconfig -u vnd0; fi
vnconfig vnd0 /var/tmp/img
mount /dev/vnd0d /mnt; echo again >/mnt/y; sync; umount /mnt
vnconfig -u vnd0
vnconfig vnd0 /var/tmp/img
mount -r /dev/vnd0d /mnt; cat /mnt/y; umount /mnt
vnconfig -u vnd0
# drvctl must refuse a unit in use and take an idle one.
vnconfig vnd0 /var/tmp/img
mount /dev/vnd0d /mnt
! drvctl -d vnd0
cat /mnt/y; umount /mnt
vnconfig -u vnd0
drvctl -d vnd0; ./race 5 /dev/vnd0d /dev/rvnd0d
# A detach of a configured unit with opens in progress must finish.
./race 40 /dev/vnd0d /dev/rvnd0d &
i=0; while [ $i -lt 100 ]; do
	vnconfig vnd0 /var/tmp/img 2>/dev/null || :
	drvctl -d vnd0 2>/dev/null || :
	i=$((i + 1))
done
wait
vnconfig -u vnd0 2>/dev/null || :
# A forced unconfigure with writes queued: a sparse file uses vn_rdwr(),
# and nothing may be left for the next configuration.
dd if=/dev/zero of=/var/tmp/sp bs=1 count=0 seek=256m
vnconfig vnd4 /var/tmp/sp
for w in 1 2 3 4 5 6 7 8; do
	dd if=/dev/zero of=/dev/rvnd4d bs=64k count=2000 2>/dev/null &
done
sleep 2
vnconfig -F -u vnd4
wait || :
dd if=/dev/zero of=/var/tmp/clean bs=1m count=32
vnconfig vnd4 /var/tmp/clean
dd if=/dev/zero of=/dev/rvnd4d bs=64k count=16
vnconfig -u vnd4
cmp /var/tmp/clean /dev/zero 2>&1 | grep -q EOF
# A detach racing with an open of a wedge on the unit must finish.
vnconfig vnd5 /var/tmp/clean
gpt create -f vnd5
gpt add -t ffs -s 4m vnd5
dkctl vnd5 listwedges
wedge=$(dkctl vnd5 listwedges | sed -n 's/^\(dk[0-9]*\):.*/\1/p' | head -1)
echo "wedge $wedge"
(i=0; while [ $i -lt 2000 ]; do : </dev/r$wedge 2>/dev/null || :; i=$((i + 1)); done) &
sleep 1
drvctl -d vnd5 || :
wait
vnconfig -u vnd5 2>/dev/null || :
echo FORCE+WEDGE OK
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
