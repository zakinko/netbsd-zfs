#!/bin/sh
# Run every host-side check against a pool image.
#
#	./run.sh <image> <start-lba> <sector-count> [dataset]
#
# The dataset is normally left out, so that the pool's bootfs property
# is what picks one -- which is the path the loader takes.
set -e
IMG=$1
LBA=$2
NSEC=$3
DS=${4-}
[ -n "$NSEC" ] || { echo "usage: $0 image lba nsectors [dataset]" >&2; exit 2; }

for t in t_sha256 t_nv t_walk t_ls t_cat t_bench; do
	./build.sh $t.c -o $t
done

echo "=== SHA-256 against FIPS 180-4"
./t_sha256

echo "=== the label's name-value pairs"
./t_nv "$IMG"

echo "=== uberblock to root directory"
./t_walk "$IMG" "$LBA" "$NSEC"

echo "=== the root directory"
./t_ls "$IMG" "$LBA" "$NSEC" "$DS" /

echo "=== reading a file"
./t_cat "$IMG" "$LBA" "$NSEC" "$DS" /netbsd

echo "=== loader sized reads, with and without the block cache"
./t_bench "$IMG" "$DS" /netbsd 0
./t_bench "$IMG" "$DS" /netbsd 1

echo "=== the nvlist parser under ASan and UBSan"
dd if="$IMG" bs=512 skip=$((LBA + 32)) count=16 of=label.bin 2>/dev/null
cc -std=c99 -g -O1 -I../src -include stdint.h -include stddef.h \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	../src/nvlist.c fuzz_nvlist.c -o fuzz_nvlist
./fuzz_nvlist label.bin 100000
rm -f label.bin

echo "=== all checks passed"
