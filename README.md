# netbsd-zfs

NetBSD/amd64 with the root file system on ZFS, two ways:

1. **A ready-made disk image**, built and boot-tested in GitHub Actions
   ([build.yml](.github/workflows/build.yml)).
2. **An install ISO whose sysinst offers "root on ZFS"**: a patch to
   NetBSD's installer, the release built from source with it applied,
   and an unattended install from that CD to prove it works
   ([iso.yml](.github/workflows/iso.yml)).

## How root on ZFS boots

NetBSD's BIOS boot loader cannot read ZFS (the libsa work is an open
[project](https://wiki.netbsd.org/projects/project/zfs_root/)), so the
kernel and the `zfs` module live on a small FFS partition of their own:

| GPT | label / mount   | fs   | contents |
|-----|-----------------|------|----------|
| 1   | boot, `/altroot`| FFS  | `/boot`, `netbsd`, the `zfs` module, `ramdisk-zfsroot.fs` |
| 2   | swap            | swap | |
| 3   | rpool           | ZFS  | `rpool/ROOT`, mounted as `/` |

The two variants differ in what happens next.

**The disk image** is stock NetBSD 11.0 and uses the layout from
[Root On ZFS](https://wiki.netbsd.org/root_on_zfs/) on the wiki:
`boot.cfg` loads the `solaris` and `zfs` modules and the
`ramdisk-zfsroot.fs` shipped in every release.  The ramdisk imports
`rpool`, mounts `rpool/ROOT` on `/altroot` and sets `init.root`, so
`init` starts inside the ZFS file system.

**The ISO** installs a system whose kernel mounts the pool itself.
`boot.cfg` says

    load /stand/amd64/11.0/modules/zfs/zfs.kmod;root zfs:rpool;boot

`root zfs:rpool` names a file system type rather than a device; the
kernel hands it to the `zfs` module, which imports the pool from the
disks it finds and mounts the pool's `bootfs` (`rpool/ROOT`) as `/`,
read/write, before `init` runs.  The ramdisk stays in the boot partition
and in the boot menu ("Boot through the root ramdisk") as a way in when
something is wrong with that path.  At the boot prompt, `root` can also
be answered with `zfs:<pool>` or `zfs:<pool>/<dataset>`.

In both, the boot partition is in `fstab` as `/altroot` (`noauto`);
mount it before updating the kernel or modules.

## The ZFS on the ISO

The ZFS in NetBSD's base tree (`external/cddl/osnet`) came from illumos
by way of FreeBSD around 2016 and has not met OpenZFS since: 16 feature
flags against OpenZFS's 49, so no `large_blocks`, `device_removal`,
`encryption`, `zstd` or `draid`, and a pool created elsewhere with any of
those will not import.

The ISO carries OpenZFS instead, from
[zakinko/zfs](https://github.com/zakinko/zfs), branch `bsd-kernel`, at
the commit in [patches/OPENZFS_REV](patches/OPENZFS_REV).  The `iso`
workflow builds its userland natively in a NetBSD VM and `zfs.kmod`
against the release's kernel, then `ci/openzfs-sets.sh` swaps them into
the release sets in place of osnet's `zfs`, `zpool`, `mount_zfs`, `zdb`,
`ztest`, `libzfs`, `libzfs_core`, `libzpool`, their manual pages and
`zfs.kmod`.  osnet's `libavl`, `libnvpair`, `libumem` and `libuutil`
stay, as does `solaris.kmod`, which `dtrace` needs.

The OpenZFS commands in `/sbin` link against `libintl`, `librt` and
`libexecinfo` from `/usr/lib`, so they need `/usr` mounted; the ramdisk
gets static copies.

The disk image is still built from stock NetBSD 11.0 and uses osnet.

## The patches

[patches/](patches/) holds four diffs against the `netbsd-11` branch
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
before anything is written.  sysinst then creates the pool, sets its
`bootfs` to `rpool/ROOT`, extracts the sets into it, fetches
`ramdisk-zfsroot.fs` from `installation/ramdisk` next to the sets (from
the CD or the same ftp/http server), copies the kernel and the module to
the boot partition, writes its `boot.cfg` and installs the bootstrap
there.  Upgrading such a system works too: sysinst imports the pool,
mounts `rpool/ROOT` and the boot partition, and after the sets are in
refreshes the kernel, the module, the ramdisk and the bootstrap in the
boot partition.  The KASLR kernel is not offered with a ZFS root,
because the `zfs` module is built with `KDTRACE_HOOKS` and
`GENERIC_KASLR` without, so it cannot be loaded there.

[04-ramdisk-openzfs.diff](patches/04-ramdisk-openzfs.diff) puts the
static OpenZFS `zpool` and `mount_zfs` into `ramdisk-zfsroot.fs` in
place of osnet's, and makes the image large enough for them.

[05-root-by-fsname.diff](patches/05-root-by-fsname.diff) is the kernel
side of `root zfs:rpool`.  `setroot()` insisted on a device, and a pool
is not one: a root specification of the form `<fstype>:<spec>` now
selects the file system type and leaves the device unset, and
`vfs_mountroot()` then calls that type's `mountroot` with no device for
it to take over.  The same form is accepted at the `root device:`
prompt, so a mistyped pool name in `boot.cfg` lands there and can be
corrected.  `kern.root_device` reads as an empty string in that case.

[06-sysctl-namelen.diff](patches/06-sysctl-namelen.diff) lets a sysctl
node name be 255 bytes instead of 31.  OpenZFS names its tunables and
kstats after Linux's, where nothing limits them, and the pool name is
one component of every per-pool kstat, so six of the module's own
names and any pool name over 31 bytes were cut short.  The name is an
array in `struct sysctlnode`, which crosses into user space, so this
is `SYSCTL_VERS_2`, the second use of the version that sysctl(3) was
designed with: a binary built with the old node still passes and
receives it, through conversions in the kernel and in libc's user
tree, and libc keeps a `sysctlgetmibinfo()` and `__learn_tree()` that
work in it for programs, sysctl(8) among them, that walk the tree
themselves.  Such a program sees a longer name cut to 31 bytes.  The
node is larger, so kernel modules have to be built against the new
`<sys/sysctl.h>`; the module on the CD is, and a stock 11.0 module
loaded later is not.

Not covered: MBR/disklabel disks, because the pool is looked for on
whole disks and wedges, and GENERIC does not turn disklabel partitions
into wedges.

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

## Reading ZFS from the loader

[`boot/zfs/`](boot/zfs/) is the other half of that open project: with
it `bootx64.efi` reads the kernel and the modules out of the pool, so
they no longer need a partition of their own.  It is written from the
ZFS On-Disk Specification rather than carried from another loader, and
`install.sh` puts it into a NetBSD 11.0 tree.

It is EFI only: the BIOS `boot` has 192KB of heap
under the 640KB line, and a vdev label's nvlist alone is 112KB.
