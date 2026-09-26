#!/bin/sh
#
# Everything that can be checked without a loader: make a pool with ZFS,
# read it back with the reader, and compare.
#
#	sh ci.sh
#
# NetBSD only, and it needs root for vnconfig and zpool.  The point of
# making the pool here rather than checking an image in is that the
# format moves: what this reads is what the ZFS on the machine wrote
# today.
set -e
cd "$(dirname "$0")"

# vnconfig, zpool and paxctl are all in /sbin or /usr/sbin, which a
# login shell here need not have on its path.
PATH=/sbin:/usr/sbin:$PATH
export PATH

IMG=${IMG-/var/tmp/zfsreader-test.img}
VND=${VND-vnd3}
NSEC=393216			# 192MB in 512 byte sectors

# A run that stops in the middle would otherwise leave the pool
# imported and the vnd attached, which the next run would trip over --
# and on a machine that is not only running this, somebody else would.
cleanup() {
	zpool destroy testpool 2>/dev/null || true
	vnconfig -u "$VND" 2>/dev/null || true
	rm -f "$IMG" out.bin label.bin mkpool.out
	rm -f t_sha256 t_nv t_walk t_ls t_cat t_bench fuzz_nvlist t_dnode fuzz_pool
}
trap cleanup EXIT INT TERM

echo "=== building the test drivers"
for t in t_sha256 t_nv t_walk t_ls t_cat t_bench; do
	./build.sh $t.c -o $t
done

echo
echo "=== making a pool with ZFS"
sh mkpool.sh "$IMG" "$VND" > mkpool.out
cat mkpool.out

echo
echo "=== SHA-256 against FIPS 180-4"
./t_sha256

echo
echo "=== the label"
./t_nv "$IMG" 0

echo
echo "=== uberblock to root directory"
./t_walk "$IMG" 0 $NSEC

echo
echo "=== reading back what ZFS wrote"
fail=0
# cksum -a sha256 prints "SHA256 (<path>) = <hex>", so the value ZFS
# gave for a file is found by its name.
check() {		# <dataset> <path>
	./t_cat "$IMG" 0 $NSEC "$1" "$2" out.bin > /dev/null
	got=$(cksum -a sha256 out.bin | sed 's/.* = //')
	exp=$(grep "$2)" mkpool.out | sed 's/.* = //')
	rm -f out.bin
	[ -n "$exp" ] || { echo "  FAIL  no checksum for $2 from mkpool"; fail=1; return; }
	if [ "$got" = "$exp" ]; then
		echo "  ok    $1$2"
	else
		echo "  FAIL  $1$2"
		echo "        reader $got"
		echo "        zfs    $exp"
		fail=1
	fi
}
# bootfs is set, so the empty dataset name must find testpool/fs.
check "" /payload
check "" /zeros
check gz /text

echo
echo "=== the directory with more names than one ZAP block holds"
n=$(./t_ls "$IMG" 0 $NSEC "" /many | grep -c entry-with)
echo "  listed $n of 3000"
[ "$n" = 3000 ] || fail=1

echo
echo "=== loader sized reads, with and without the block cache"
./t_bench "$IMG" 0 $NSEC "" /payload 0
./t_bench "$IMG" 0 $NSEC "" /payload 1

echo
echo "=== the dnode geometry check"
# This one includes the source rather than linking it, so it is built
# on its own and not through build.sh.
cc -std=c99 -Wall -Wextra -O1 -g -I../src -DZFS_SUPPORT_GZIP \
	-fsanitize=undefined -fno-sanitize-recover=all \
	../src/sha256.c ../src/fletcher.c ../src/lz4.c ../src/gzip.c \
	../src/zle.c ../src/zstd.c ../src/scratch.c ../src/sha512.c \
	../src/skein.c ../src/edonr.c ../src/blake3.c ../src/nvlist.c t_dnode.c \
	-o t_dnode -lz
command -v paxctl >/dev/null 2>&1 && paxctl +a ./t_dnode || true
./t_dnode
rm -f t_dnode

echo
echo "=== the whole read path under ASan and UBSan"
# The checksums are taken out of the way on purpose here: the reader is
# hardened against a disk written deliberately, and whoever writes one
# recomputes them.  The counts say how far the corrupted cases got --
# a run that never mounted anything would be a broken harness rather
# than a clean reader.
if cc -std=c99 -O1 -g -I../src -DZFS_SUPPORT_GZIP -DZFS_FUZZ_NO_CKSUM \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	../src/zfsread.c ../src/zap.c ../src/zfsfs.c ../src/nvlist.c \
	../src/sha256.c ../src/fletcher.c ../src/lz4.c ../src/gzip.c \
	../src/zle.c ../src/zstd.c ../src/scratch.c ../src/sha512.c \
	../src/skein.c ../src/edonr.c ../src/blake3.c fuzz_pool.c -o fuzz_pool -lz \
	2>/dev/null; then
	command -v paxctl >/dev/null 2>&1 && paxctl +a ./fuzz_pool || true
	./fuzz_pool "$IMG" 0 /payload 2000 7
else
	echo "  (no sanitizer here; skipped)"
fi
rm -f fuzz_pool

echo
echo "=== the nvlist parser under ASan and UBSan"
dd if="$IMG" bs=512 skip=32 count=16 of=label.bin 2>/dev/null
if cc -std=c99 -g -O1 -I../src -fsanitize=address,undefined \
	-fno-sanitize-recover=all ../src/nvlist.c fuzz_nvlist.c \
	-o fuzz_nvlist 2>/dev/null; then
	# NetBSD randomises the address space by default and the
	# sanitizer refuses to start under it, saying so and exiting
	# non-zero -- which reads as a test failure rather than a
	# configuration one.  paxctl turns it off for this binary.
	command -v paxctl >/dev/null 2>&1 && paxctl +a ./fuzz_nvlist || true
	./fuzz_nvlist label.bin 50000
else
	echo "  (no sanitizer here; skipped)"
fi
[ $fail = 0 ] || { echo "FAILED"; exit 1; }
echo
echo "=== all checks passed"
