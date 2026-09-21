# netbsd-zfs

Builds a NetBSD/amd64 disk image with its root filesystem on ZFS, and
boots it to check that it really is.  Everything happens in GitHub
Actions: the image is assembled inside a NetBSD 11.0 VM
([vmactions/netbsd-vm](https://github.com/vmactions/netbsd-vm)), then
booted with qemu-kvm on the runner and inspected over the serial console.

## How it boots

NetBSD's bootloader cannot read ZFS, so the layout is the one from
[Root On ZFS](https://wiki.netbsd.org/root_on_zfs/) on the NetBSD wiki:

| GPT | label | fs   | contents |
|-----|-------|------|----------|
| 1   | boot  | FFS  | `/boot`, `netbsd`, the `solaris` and `zfs` modules, `ramdisk-zfsroot.fs` |
| 2   | swap  | swap | |
| 3   | rpool | ZFS  | `rpool/ROOT`, mounted as `/` |

`boot.cfg` loads the kernel and the two modules from the FFS partition and
then the ramdisk shipped in the release.  The ramdisk imports `rpool`,
mounts `rpool/ROOT` on `/altroot` and sets `init.root`, so `init` starts
inside the ZFS filesystem.  The boot partition is in `fstab` as `/altroot`
(`noauto`); mount it before updating the kernel or modules.

## Using the image

Take `netbsd-zfs.img.gz` from the workflow artifact or a release,
`gunzip` it and boot it in a BIOS VM:

    qemu-system-x86_64 -enable-kvm -m 2048 -drive file=netbsd-zfs.img,format=raw -serial stdio

`consdev=auto` in `boot.cfg` puts the console on the serial port when the
BIOS knows of one, otherwise on the display.  Root has no password.

## Locally

`build.sh` runs as root on a NetBSD 11.0 host (it uses `vnd0`, `gpt`,
`zpool`), and `test.sh` on anything with `qemu-system-x86_64` and
`expect`:

    sh build.sh
    sh test.sh out/netbsd-zfs.img
