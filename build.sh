#!/bin/sh
#
# Build a NetBSD/amd64 disk image whose root filesystem is ZFS.
#
# Runs as root on a NetBSD host of the same release (the CI uses the
# vmactions NetBSD VM).  Layout of the image:
#
#   GPT index 1  boot   FFSv2   kernel, modules, /boot, ramdisk-zfsroot.fs
#   GPT index 2  swap
#   GPT index 3  rpool  ZFS     rpool/ROOT mounted as /
#
# The bootloader cannot read ZFS, so it loads the kernel, the solaris and
# zfs modules and ramdisk-zfsroot.fs from the FFS partition.  The ramdisk
# imports rpool, mounts rpool/ROOT on /altroot and sets init.root so init
# chroots into it.  See https://wiki.netbsd.org/root_on_zfs/ .

set -eu

REL=${REL:-11.0}
ARCH=${ARCH:-amd64}
CDN=${CDN:-https://cdn.NetBSD.org/pub/NetBSD/NetBSD-$REL/$ARCH}
SETS="kern-GENERIC modules base etc rescue"

OUT=${OUT:-out}
IMG=$OUT/netbsd-zfs.img
IMG_MB=${IMG_MB:-4096}
BOOT_MB=${BOOT_MB:-512}
SWAP_MB=${SWAP_MB:-512}
HOSTNAME_=${HOSTNAME_:-netbsd-zfs}

# outside the tree so the CI does not rsync 400MB of sets back to the runner
DIST=${DIST:-/var/tmp/netbsd-zfs-dist}
ALTROOT=/altroot
BOOTMNT=/mnt/boot
VND=${VND:-vnd0}

log() { echo "==> $*"; }

fetch() {
	mkdir -p "$DIST"
	for s in $SETS; do
		[ -f "$DIST/$s.tar.xz" ] || ftp -o "$DIST/$s.tar.xz" "$CDN/binary/sets/$s.tar.xz"
	done
	[ -f "$DIST/ramdisk-zfsroot.fs" ] ||
	    ftp -o "$DIST/ramdisk-zfsroot.fs" "$CDN/installation/ramdisk/ramdisk-zfsroot.fs"
}

# dk wedge for the GPT partition labelled $1 on $VND
wedge() {
	dkctl "$VND" listwedges | awk -v l="$1" -F'[: ,]+' '$2 == l { print $1; exit }'
}

cleanup() {
	set +e
	umount "$ALTROOT" 2>/dev/null
	zpool export rpool 2>/dev/null
	umount "$BOOTMNT" 2>/dev/null
	vnconfig -u "$VND" 2>/dev/null
}
trap cleanup EXIT

fetch

log "creating $IMG (${IMG_MB}MB)"
mkdir -p "$OUT"
rm -f "$IMG"
# no truncate(1) on NetBSD; write the last block to size the sparse file
dd if=/dev/zero of="$IMG" bs=1m seek=$((IMG_MB - 1)) count=1 msgfmt=quiet
vnconfig "$VND" "$IMG"

log "partitioning"
gpt create "$VND"
gpt add -a 1m -s "${BOOT_MB}m" -t ffs  -l boot  "$VND"
gpt add -a 1m -s "${SWAP_MB}m" -t swap -l swap  "$VND"
gpt add -a 1m                  -t zfs  -l rpool "$VND"
gpt biosboot -i 1 "$VND"
dkctl "$VND" makewedges >/dev/null 2>&1 || true
BOOTDK=$(wedge boot)
ZFSDK=$(wedge rpool)
[ -n "$BOOTDK" ] && [ -n "$ZFSDK" ] || { dkctl "$VND" listwedges; exit 1; }
log "boot=$BOOTDK zfs=$ZFSDK"

log "boot partition"
newfs -O 2 "/dev/r$BOOTDK" >/dev/null
mkdir -p "$BOOTMNT"
mount "/dev/$BOOTDK" "$BOOTMNT"
cp /usr/mdec/boot "$BOOTMNT/boot"
installboot -v -o console=auto,timeout=5 "/dev/r$BOOTDK" /usr/mdec/bootxx_ffsv2
tar -xJpf "$DIST/kern-GENERIC.tar.xz" -C "$BOOTMNT"
# only the two modules the ramdisk needs; the rest live on the ZFS root
tar -xJpf "$DIST/modules.tar.xz" -C "$BOOTMNT" \
    "./stand/$ARCH/$REL/modules/solaris" "./stand/$ARCH/$REL/modules/zfs"
cp "$DIST/ramdisk-zfsroot.fs" "$BOOTMNT/"
cat > "$BOOTMNT/boot.cfg" <<'BOOTCFG'
banner=NetBSD root on ZFS
banner=
banner=/ is rpool/ROOT; the kernel and modules live on the FFS boot partition,
banner=mounted on /altroot with: mount /altroot
consdev=auto
menu=Boot ZFS root:load solaris;load zfs;fs /ramdisk-zfsroot.fs;boot
menu=Boot ZFS root, single user:load solaris;load zfs;fs /ramdisk-zfsroot.fs;boot -s
menu=Drop to boot prompt:prompt
default=1
timeout=5
clear=1
BOOTCFG
umount "$BOOTMNT"

log "zpool"
zpool create -f -o cachefile=none -O compression=lz4 -O atime=off rpool "/dev/$ZFSDK"
zfs create -o mountpoint=legacy rpool/ROOT
mkdir -p "$ALTROOT"
mount -t zfs rpool/ROOT "$ALTROOT"

log "extracting sets"
for s in $SETS; do
	tar -xJpf "$DIST/$s.tar.xz" -C "$ALTROOT"
done
(cd "$ALTROOT/dev" && ./MAKEDEV all)
mkdir -p "$ALTROOT/kern" "$ALTROOT/proc"

log "configuring"
cat > "$ALTROOT/etc/fstab" <<'FSTAB'
# / is rpool/ROOT, mounted by ramdisk-zfsroot.fs before init runs.
NAME=boot	/altroot	ffs	rw,noauto	1 1
NAME=swap	none		swap	sw,dp		0 0
kernfs		/kern		kernfs	rw
ptyfs		/dev/pts	ptyfs	rw
procfs		/proc		procfs	rw
tmpfs		/var/shm	tmpfs	rw,-m1777,-sram%25
FSTAB
cat > "$ALTROOT/etc/rc.conf" <<RCCONF
# See rc.conf(5) for more information.
if [ -r /etc/defaults/rc.conf ]; then
	. /etc/defaults/rc.conf
fi
rc_configured=YES
hostname=$HOSTNAME_
zfs=YES
dhcpcd=YES
wscons=YES
RCCONF
cat > "$ALTROOT/etc/motd" <<'MOTD'
NetBSD root on ZFS -- built by https://github.com/zakinko/netbsd-zfs
Root has no password; set one with passwd(1).
MOTD
echo "$HOSTNAME_" > "$ALTROOT/etc/myname"

log "done"
zpool status rpool
zfs list
df -h "$ALTROOT"
cleanup
trap - EXIT

log "compressing"
gzip -1 -f "$IMG"
ls -l "$OUT"
