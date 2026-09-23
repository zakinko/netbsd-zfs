# netbsd-zfs

NetBSD/amd64 with the root file system on ZFS, two ways:

1. **A ready-made disk image**, built and boot-tested in GitHub Actions
   ([build.yml](.github/workflows/build.yml)).
2. **An install ISO whose sysinst offers "root on ZFS"**: a patch to
   NetBSD's installer, the release built from source with it applied,
   and an unattended install from that CD to prove it works
   ([iso.yml](.github/workflows/iso.yml)).

## How root on ZFS boots

NetBSD's boot loader cannot read ZFS (the libsa work is an open
[project](https://wiki.netbsd.org/projects/project/zfs_root/)), so
both variants use the layout from
[Root On ZFS](https://wiki.netbsd.org/root_on_zfs/) on the wiki:

| GPT | label / mount   | fs   | contents |
|-----|-----------------|------|----------|
| 1   | boot, `/altroot`| FFS  | `/boot`, `netbsd`, the `solaris` and `zfs` modules, `ramdisk-zfsroot.fs` |
| 2   | swap            | swap | |
| 3   | rpool           | ZFS  | `rpool/ROOT`, mounted as `/` |

`boot.cfg` loads the kernel and the two modules from the FFS partition,
then the `ramdisk-zfsroot.fs` shipped in every release.  The ramdisk
imports `rpool`, mounts `rpool/ROOT` on `/altroot` and sets `init.root`,
so `init` starts inside the ZFS file system.  The boot partition is in
`fstab` as `/altroot` (`noauto`); mount it before updating the kernel
or modules.

## The patches

[patches/](patches/) holds three diffs against the `netbsd-11` branch
(the revision they were made against is in
[patches/SRC_REV](patches/SRC_REV)); `ci/build-iso.sh` applies them
in name order.

[03-sysinst-zfs-root.diff](patches/03-sysinst-zfs-root.diff) adds one
entry to sysinst's partition layout question, and documents in
`sysctl(7)` the `kern.module.path` it reads to find the modules --
the sysctl was there but undocumented, and this is its first user
outside the kernel:

    a: Set sizes of NetBSD partitions
    b: Use default partition sizes
    c: Use default partition sizes, root on ZFS
    d: Manually define partitions

The entry appears when the medium has `zpool`/`zfs` and the disk uses
GPT.  Choosing it gives the layout above; the sizes can still be edited
before anything is written.  sysinst then creates the pool, extracts the
sets into it, fetches `ramdisk-zfsroot.fs` from `installation/ramdisk`
next to the sets (from the CD or the same ftp/http server), copies the
kernel and the two modules to the boot partition, writes its `boot.cfg`
and installs the bootstrap there.  Upgrading such a system works too:
sysinst imports the pool, mounts `rpool/ROOT` and the boot partition,
and after the sets are in refreshes the kernel, the modules, the
ramdisk and the bootstrap in the boot partition.  The KASLR kernel is
not offered with a ZFS root, because the `zfs` module is built with
`KDTRACE_HOOKS` and `GENERIC_KASLR` without, so it cannot be loaded
there.

[02-rc-root-zfs.diff](patches/02-rc-root-zfs.diff) makes `rc.d/root`
leave a ZFS root alone: the boot ramdisk already mounted it read/write
and `mount_zfs` cannot update a mount.  With the `noauto` entry sysinst
writes for `/`, `fsck_root` and `mountall` are quiet as well.

[01-libzfs-import-union-dev.diff](patches/01-libzfs-import-union-dev.diff)
has `zpool import` open the candidates in `/dev` by full path instead
of `openat(2)` relative to the directory: on the install CD `/dev` is a
union-mounted tmpfs, and there `openat` on such a directory does not
find the entries, so the pool was never found.  (That looks like a
kernel bug in its own right.)

Not covered: MBR/disklabel disks, because the ramdisk finds the pool
with `zpool import`, which on NetBSD only scans whole disks and wedges,
and GENERIC does not turn disklabel partitions into wedges.

## Using the results

The disk image: take `netbsd-zfs.img.gz` from a `build` run or a
release, `gunzip` it and boot it in a BIOS VM.  `consdev=auto` puts the
console on the serial port when the BIOS knows of one.  Root has no
password.

    qemu-system-x86_64 -enable-kvm -m 2048 -drive file=netbsd-zfs.img,format=raw -serial stdio

The ISO: `NetBSD-11.0_STABLE-amd64.iso` from an `iso` run or a release
is the normal install CD; pick "root on ZFS" at the layout question.

## Locally

`build.sh` runs as root on a NetBSD 11.0 host (it uses `vnd0`, `gpt`,
`zpool`), `test.sh` on anything with `qemu-system-x86_64` and `expect`.

`ci/build-iso.sh` builds the release and the ISO from a NetBSD src
checkout (`nbsrc/src`, netbsd-11) on Linux or macOS; `ci/test-install.sh`
drives sysinst through the install with
[anita](https://www.gson.org/netbsd/anita/) plus the small
[patch](ci/anita-zfs-root.patch) that knows the new menu entry, and
checks that the installed system boots with `/` on ZFS.
