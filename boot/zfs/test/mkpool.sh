#!/bin/sh
#
# Make a small pool for the reader to be tested against, using ZFS
# itself, so that what the test reads is what ZFS wrote today rather
# than an image checked in months ago.
#
#	sh mkpool.sh <image> <vnd>		e.g. sh mkpool.sh /tmp/p.img vnd3
#
# NetBSD only: it needs vnconfig(8) and zpool(8), and root.
set -e

IMG=$1
VND=${2-vnd3}
[ -n "$IMG" ] || { echo "usage: $0 <image> [vnd]" >&2; exit 2; }
PATH=/sbin:/usr/sbin:$PATH
export PATH

# Run as root if we are not.
if [ "$(id -u)" != 0 ]; then
	SUDO="sudo -n"
else
	SUDO=
fi

MNT=/tmp/testpool.$$

$SUDO zpool destroy testpool 2>/dev/null || true
$SUDO vnconfig -u $VND 2>/dev/null || true
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1m count=192 2>/dev/null
$SUDO vnconfig $VND "$IMG"
$SUDO zpool create -f -m $MNT testpool /dev/$VND
$SUDO zfs create -o mountpoint=$MNT/fs testpool/fs
$SUDO zfs create -o compression=gzip -o mountpoint=$MNT/gz testpool/gz

# The shapes that have caught bugs here: a file large enough for two
# levels of indirection, a directory of more names than one ZAP block
# holds, a file of nothing but zeros (which ZFS stores as holes all the
# way up), and a compressed one.
$SUDO dd if=/dev/urandom of=$MNT/fs/payload bs=1m count=6 2>/dev/null
$SUDO dd if=/dev/zero of=$MNT/fs/zeros bs=1m count=2 2>/dev/null
$SUDO sh -c "yes hello | head -50000 > $MNT/gz/text"
$SUDO sh -c "mkdir -p $MNT/fs/many; cd $MNT/fs/many; i=0;
	while [ \$i -lt 3000 ]; do : > entry-with-a-longish-name-\$i;
	i=\$((i + 1)); done"

$SUDO zpool set bootfs=testpool/fs testpool
echo "--- what ZFS says these are:"
$SUDO cksum -a sha256 $MNT/fs/payload $MNT/fs/zeros $MNT/gz/text
$SUDO zpool export testpool
$SUDO vnconfig -u $VND
ls -l "$IMG"
