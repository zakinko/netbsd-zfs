# ZFS for the NetBSD boot loader, written from the on-disk specification

Read-only ZFS for `libsa`, so that `efiboot` can load a kernel out of a
pool. It is written from the *ZFS On-Disk Specification* (Sun
Microsystems, 2006, Draft) rather than carried from another
implementation, which is what NetBSD expects of code in its tree.

Every constant in `src/zfs_ondisk.h` carries the section it came from:

```
[S]  ZFS On-Disk Specification, Draft, Sun Microsystems, 2006
[Z]  OpenZFS include/sys/, where [S] no longer describes the disk
```

`[Z]` is not decoration. That document stops at pool version 1 and the
pools NetBSD writes are version 5000, so a reader built only from it
cannot read one: it has no lz4, no embedded block pointers, no system
attributes, and — the one that matters most — no hash function for the
fat ZAP, which it mentions without defining. Each of those is marked
where it is used, with the file it was read out of.

`spec/` holds the chapter-by-chapter transcription the code was written
from, including six places where the specification contradicts itself
or is wrong; each is repeated as a comment beside the code that had to
decide. The shortest is §3.1's worked example, which says the parent of
level 0 block 16360 is `16360 % 1024 = 15`. It is 1000. The division
gives 15, and division is what finding a parent means.

## What it reads

A pool on one disk or partition: labels, uberblocks, block pointers
(including embedded ones), gang-free DVAs, fletcher2/fletcher4/SHA-256
checksums, lz4 and uncompressed blocks, dnodes to six levels of
indirection, micro and fat ZAPs, the DSL down to a dataset, ZPL
directories, and files through either a `znode_phys_t` or the system
attributes that replaced it.

gzip goes through zlib, which the loader links anyway for gzipped
kernels; a build without zlib leaves it out and gzip is then refused
like the rest.

It refuses, rather than guessing: gang blocks, more than one top-level
vdev, an external ZAP pointer table, zle and zstd, and checksums newer
than SHA-256.

## What it was tested against

A 512MB NetBSD 11.0 image with a GPT and a pool at LBA 16418:

- `/netbsd`, 30,097,456 bytes, read whole; its ELF section headers end
  exactly at the file size and all 34 sections lie inside it. Since
  `zfs_read_block` does not return a block whose checksum fails, reading
  it to the end means every level 0 block and every indirect block above
  them verified.
- SHA-256 against FIPS 180-4's B.1 and B.2 vectors and the one million
  `a` case.
- The nvlist parser under ASan and UBSan: 300,000 mutations of a real
  label and every one of its 8,193 truncations. Removing one bound check
  makes that fail within 5,000 cases, which is how the harness was shown
  to be looking.
A second image, built to be awkward: a 20,000 entry directory (a fat ZAP
with many leaves), a 64MB file of 512 byte records (three levels of
indirection), a 100MB sparse file, a gzip dataset, and a file of nothing
but zeros.

- `/deep`, 64MB of random data through three levels, and the gzip
  dataset's `text`: both come out with the same SHA-256 as ZFS itself
  gives for the same files. A checksum that matches is worth more than
  "it read 64MB without complaining", and it is how gzip was checked.
- All 20,000 names listed.
- The all-zeros file found a real bug. ZFS stores it as holes all the
  way up -- the dnode's own block pointer is empty -- and the reader
  only looked for a hole at level 0, so descending read a pointer full
  of zeros as if it addressed sector zero and returned EIO. A hole is
  now recognised at every level.
- `efiboot` built with it loads `solaris.kmod`, `zfs.kmod`, `msdos.kmod`
  and a 30MB kernel out of the pool under qemu, and the kernel boots.
  It then says `cannot mount root, error = 79`, which is the kernel's
  own limitation and what the ramdisk in this repository is for.

## Laid out for two builds

`src/` compiles both against NetBSD's `libsa` headers and on a host,
which is what makes `test/` possible: the reader takes a read callback
rather than calling `DEV_STRATEGY` itself, so the same code runs against
a file. `test/build.sh` builds the drivers on a host; each takes an
image, a starting LBA and a sector count.

## In the tree

`src/zfs_fsops.c` becomes `sys/lib/libsa/zfs.c`; the rest keeps its name
with a `zfs_` in front where it is generic. Besides that, four changes
outside `libsa`:

- `sys/lib/libsa/saioctl.h`: `SAIODEVSIZE`, because two of the four vdev
  labels sit at the end of the device and nothing in libsa could ask how
  long it was.
- `sys/arch/i386/stand/lib/biosdisk.c`: answer it, and set `d->size` in
  `biosdisk_open_name()`, which had the size to hand and dropped it.
- `sys/arch/i386/stand/efiboot/conf.c`: one `FS_OPS(zfs)` in each of the
  two tables.
- `Makefile.efiboot`: `-DSUPPORT_ZFS` and `SA_INCLUDE_ZFS=yes`.

The loader's heap is raised from 1MB to 4MB under `SUPPORT_ZFS`. The
reader takes about 900KB of scratch and a 128KB cache per open file,
both from the heap and only once a pool is actually opened. The carried
version wanted 64MB, because it cached dnodes in a 16MB buffer.
