# Reading ZFS from the EFI boot loader

The rest of this repository boots root on ZFS the way the wiki does: an
FFS partition holds the kernel and a ramdisk, and the ramdisk imports the
pool and hands the real root over. That works because the boot loader
cannot read ZFS.

This directory teaches it to. With these files in place, `bootx64.efi`
reads the kernel and the kernel modules out of the pool itself, and no
FFS partition is needed for them.

## What it can and cannot do

    reads a pool on a single disk or partition       yes
    kernel, modules, boot.cfg out of the pool        yes
    fletcher2/4, sha256/512, lzjb, zle, lz4, gzip    yes
    skein, blake3, zstd                              no
    mirror, raidz                                    no
    mounting root from the pool                      no -- see below
    the BIOS `boot`                                  no -- see below

A mirror or raidz spans devices this layer never sees: libsa hands a
file system one already-opened partition, and finding the other members
needs the loader to enumerate disks first. `zfsimpl.c` can reconstruct
them once it has them.

The kernel still cannot mount a dataset as root, so the ramdisk the rest
of this repository builds is still what completes the boot. What changes
is that the kernel and the modules no longer need a partition of their
own.

The BIOS `boot` is out of reach for a different reason. Its heap is fixed
at 192KB by `HEAP_START`/`HEAP_LIMIT` under the 640KB line, and a vdev
label's nvlist alone is `VDEV_PHYS_SIZE`, 112KB, with another 128KB for
the uberblock ring. `efiboot` takes its heap with `AllocatePages` and the
size is one number in one file.

## Using it

    ./fetch.sh  /path/to/src      # brings the ZFS sources in
    ./adjust.sh /path/to/src      # fits them to NetBSD, wires up the build
    cd /path/to/src/sys/arch/i386/stand/efiboot && make USETOOLS=no

`fetch.sh` needs the network. Both scripts can be re-run; `adjust.sh`
notices what it has already done.

On a NetBSD/amd64 host the native `make USETOOLS=no` is enough -- no
cross toolchain, no `build.sh`. The system's own `/usr/share/mk` serves,
and `syssrc.tgz` is the whole of the source needed: it carries
`common/dist/zlib` as well, which gzip decompression wants.

Then point `boot.cfg` at the pool by its GPT label:

    menu=Boot from the ZFS pool:boot NAME=zroot:netbsd

and set the pool's `bootfs`, which is what tells the loader which dataset
holds the kernel:

    zpool set bootfs=rpool/ROOT rpool

## Where the sources come from

Five places, and `fetch.sh` says which as it goes.

FreeBSD keeps a copy of the on-disk format with the write support
removed, in `sys/cddl/boot/zfs`, deliberately *not* shared with its own
ZFS so that re-importing from upstream stays easy. That copy is what a
boot loader wants, and it is most of what is fetched. `stand/libsa/zfs`
adds the reader built on it, and `nvlist.c`.

NetBSD's own `external/cddl/osnet` cannot supply two of the pieces:
its `lz4.c` reaches for `<sys/zfs_context.h>` and its `list.c` for
osnet's include tree, neither of which a standalone program has. Those
come from FreeBSD and OpenZFS instead.

## What is in `src/`

`zfs.c` is the `struct fs_ops` layer -- open, read, seek, stat -- and is
the only file here written for NetBSD rather than carried. `zfs_compat.c`
holds the allocator: libsa offers `alloc()`/`dealloc()`, and `dealloc()`
wants the size back, which the carried code does not have when it frees,
so each block carries its own size in front of what is handed out.

`nb_compat.h` is force-included into the carried files, the way FreeBSD
force-includes its own `ccompile.h`, so that they need no changes of
their own. `assert.h` and `stdbool.h` beside it are there because the
loader builds with `-nostdinc` and libsa has neither.

Only the files under `sys/external/cddl/boot/zfs` see those: `nb_compat.h`
renames `malloc()` and friends, and a `stdbool.h` on the include path
would shadow the compiler's for every other file in libsa.

## What `adjust.sh` changes in NetBSD

Three of them are small and stand on their own:

`saioctl.h` gains `SAIODEVSIZE`. ZFS keeps two of its four labels at the
end of the vdev and cannot find them without knowing where the end is.

`biosdisk_ioctl()` answered `EIO` to everything; it now answers that one
question. The size was already in `struct biosdisk`.

`biosdisk_open_name()` -- the `NAME=<label>` path -- had the partition
size to hand and passed it only to `bi_wedge.nblks`, never to `d->size`.
Nothing read `d->size` back except `bi_wedge`, so it staying zero had
never mattered.

The rest wires ZFS into the build: `SA_INCLUDE_ZFS` in libsa's
`Makefile`, `SUPPORT_ZFS` in `Makefile.efiboot`, an entry in each of
`conf.c`'s two file-system tables, and a larger heap for `efiboot` when
ZFS is built in -- `zfsimpl.c` takes a `SPA_MAXBLOCKSIZE` buffer to cache
dnodes in, so 1MB is not enough. Loaders without ZFS keep the 1MB.

## Tested

NetBSD 11.0/amd64, built natively on an 11.0 host, run under qemu with
OVMF against a GPT image holding an ESP and a 496MB pool:

    Loading NAME=zroot:/stand/amd64/11.0/modules/solaris/solaris.kmod
    Loading NAME=zroot:/stand/amd64/11.0/modules/zfs/zfs.kmod
    [   1.0865360] dk1 at ld0: "zroot", 1032125 blocks at 16418, type: zfs
    [   2.7364190] root on dk1
    [   2.7364190] Supported file systems: zfs mfs lfs ffs ext2fs nfs ...
    [   2.7364190] cannot mount root, error = 79

The 30MB kernel and the modules were read from the pool. The root mount
is the kernel's limitation, not the loader's.

Not tested: `bootia32.efi` builds but has not been run; aarch64 and the
other machines that use `sys/stand/efiboot` are untouched -- that loader
would need `SAIODEVSIZE` in `efiblock.c` as well.
