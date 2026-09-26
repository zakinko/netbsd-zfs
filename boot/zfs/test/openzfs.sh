#!/bin/sh
# Make pools with OpenZFS on Linux and read them back with the reader.
#
#	sudo sh openzfs.sh
#
# ci.sh reads a pool made by NetBSD's own ZFS.  That ZFS is the 2016
# illumos import, and there is a good deal it never writes: this runs
# where the current OpenZFS does, and covers what only it can produce.
#
#   plain	nothing forced; the pool a Linux user makes today
#   gangold	gang blocks, with [S] §2.3's 512 byte headers
#   gangnew	gang blocks, with dynamic_gang_header's 1 << ashift ones
#   stripe	two top-level vdevs
#   mirror	a two-way mirror, then with one side's data wiped
#   stale	a mirror whose second side was offline for the last write
#   mirrors	two two-way mirrors
#
# Each file is read through the reader and compared with the SHA-256
# ZFS gave it, and each pool is then fuzzed with the checksums out of
# the way, as ci.sh does.  A gang pool that was not actually ganged
# would pass for the wrong reason, so zdb is asked to count them; and
# each pool on several disks is also read from one disk alone, where
# it has to fail, so that a pass shows the other disks were used.
set -eu
cd "$(dirname "$0")"
W=$(mktemp -d)
P=/sys/module/zfs/parameters
old_fg=$(cat $P/metaslab_force_ganging)
old_pct=$(cat $P/metaslab_force_ganging_pct)
cleanup() {
	echo "$old_fg" > $P/metaslab_force_ganging
	echo "$old_pct" > $P/metaslab_force_ganging_pct
	for p in $(zpool list -H -o name 2>/dev/null); do
		case $p in zbt_*) zpool destroy -f "$p" ;; esac
	done
	rm -rf "$W"
	rm -f t_cat fuzz_pool
}
trap cleanup EXIT

# glibc hides pread() from -std=c99 without this.
./build.sh -D_DEFAULT_SOURCE t_cat.c -o t_cat
cc -std=c99 -O1 -g -I../src -include stdint.h -include stddef.h \
	-D_DEFAULT_SOURCE -DZFS_SUPPORT_GZIP -DZFS_FUZZ_NO_CKSUM \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	../src/zfsread.c ../src/zap.c ../src/zfsfs.c ../src/nvlist.c \
	../src/sha256.c ../src/fletcher.c ../src/lz4.c ../src/gzip.c \
	../src/zle.c ../src/scratch.c fuzz_pool.c -o fuzz_pool -lz

gang() {
	echo "$1" > $P/metaslab_force_ganging
	echo "$2" > $P/metaslab_force_ganging_pct
}

# mk <name> <what to write> <layout> [zpool create options]
#
# The layout is zpool's, with D for each disk: "D", "mirror D D".
# Disk n is the file <name>-<n>.img.
mk() {
	name=zbt_$1; fill=$2; layout=$3; shift 3
	vdevs= i=0
	for w in $layout; do
		if [ "$w" = D ]; then
			w=$W/$name-$i.img
			truncate -s 128M "$w"
			i=$((i + 1))
		fi
		vdevs="$vdevs $w"
	done
	zpool create -f -o ashift=12 -O compression=lz4 -O atime=off \
	    -O mountpoint=$W/mnt/$name "$@" "$name" $vdevs
	m=$W/mnt/$name
	$fill "$m"
	zpool sync "$name"
	gang 16777217 3
	zpool set bootfs="$name" "$name"
	(cd "$m" && find . -type f | sed 's|^\./||' | sort |
	    xargs sha256sum) > "$W/$name.sha256"
	zpool get -H -o value feature@dynamic_gang_header "$name" \
	    > "$W/$name.dgh"
	zpool export "$name"
}

files() {
	head -c 1048576 /dev/urandom > "$1/random"
	seq 1 200000 > "$1/text"
	head -c 3000 /dev/urandom > "$1/small"
	zfs create -o recordsize=16k -o compression=off \
	    "${1##*/}/rs16k"
	head -c 262144 /dev/urandom > "$1/rs16k/random"
}

# 40000 makes a 128K block a gang of three near 43K members, and each
# of those a gang again: two levels, and a threshold the members can
# fall below.  At 4096 every member is itself forced to gang, the
# allocation never finishes and the pool suspends.
gangfiles() {
	gang 40000 100
	files "$1"
	# dynamic_gang_header goes active in the txg after a gang first
	# needs more than three members, and only headers written after
	# that are large ([Z] zio.c, zio_write_gang_block).
	zpool sync "${1##*/}"
	head -c 1048576 /dev/urandom > "$1/random2"
}

# The second disk misses the last write, so its uberblocks are older
# than the first's: [S] §1.3.4's newest uberblock has to be found
# across the disks, not on whichever the pool was found on.
stalefiles() {
	files "$1"
	zpool sync "${1##*/}"
	zpool offline "${1##*/}" "$W/${1##*/}-1.img"
	head -c 1048576 /dev/urandom > "$1/after"
}

fail=0

# check <pool> [<disks>] [fail]
#
# <disks> is the images to give the reader, by number, the first being
# the one the pool is found on: "0", "1,0".  All of them by default.
# With "wrong", every file has to come out wrong.
check() {
	name=zbt_$1
	disks=${2:-}
	expect=${3:-ok}
	if [ -z "$disks" ]; then
		disks=$(ls "$W/$name"-*.img | sed 's|.*-\([0-9]*\)\.img$|\1|' |
		    sort -n | paste -sd, -)
	fi
	img=$(echo "$disks" | sed "s|\([0-9][0-9]*\)|$W/$name-\1.img|g")
	first=${img%%,*}
	n=$(( $(wc -c < "$first") / 512 ))
	while read -r want f; do
		case $f in
		*/*)	ds=${f%/*}; fp=/${f##*/} ;;
		*)	ds=; fp=/$f ;;
		esac
		rm -f "$W/out"
		msg=$(./t_cat "$img" 0 $n "$ds" "$fp" "$W/out" 2>&1 | tail -1)
		got=$(sha256sum "$W/out" 2>/dev/null | cut -d' ' -f1)
		if [ "$got" = "$want" ]; then r=ok; else r=wrong; fi
		if [ $r = "$expect" ]; then
			echo "  $1 [$disks] $f: $r, as it should be"
		elif [ $r = ok ]; then
			echo "  $1 [$disks] $f: READ, and should not have been"
			fail=1
		else
			echo "  $1 [$disks] $f: WRONG ($msg)"
			fail=1
		fi
	done < "$W/$name.sha256"
	if [ "$expect" = ok ] && [ "$first" = "$img" ]; then
		./fuzz_pool "$img" 0 /random 1000 7 | sed 's/^/  /'
	fi
}

# check1 <pool> <disks> <file>: that one file has to come out wrong.
# A pool on several disks read from fewer may still find the files that
# happen to lie on those; a one megabyte file of random data is eight
# blocks, and on a stripe some of them land on each disk.
check1() {
	grep "  $3\$" "$W/zbt_$1.sha256" > "$W/one.sha256"
	cp "$W/zbt_$1.sha256" "$W/all.sha256"
	cp "$W/one.sha256" "$W/zbt_$1.sha256"
	check "$1" "$2" wrong
	cp "$W/all.sha256" "$W/zbt_$1.sha256"
}

# Zero everything on a disk but its labels ([S] §1.2.1: two of 256K at
# each end, and the boot block after the first two), so that every
# block read from it fails its checksum.
wipe() {
	f=$W/zbt_$1-$2.img
	sz=$(wc -c < "$f")
	dd if=/dev/zero of="$f" bs=1M seek=4 \
	    count=$(( (sz - 4 * 1048576 - 524288) / 1048576 )) \
	    conv=notrunc status=none
}

ganged() {
	name=zbt_$1
	g=$(zdb -e -p "$W" -bb "$name" 2>/dev/null | awk '/ganged count:/ { print $3 }')
	echo "  $1: $g gang blocks"
	[ "${g:-0}" -gt 0 ] || { echo "  $1 was not ganged"; fail=1; }
}

echo "=== OpenZFS $(zfs version | head -1)"

echo "=== a pool as OpenZFS makes it"
mk plain files D
check plain

echo "=== gang blocks with 512 byte headers"
mk gangold gangfiles D -o feature@dynamic_gang_header=disabled
ganged gangold
check gangold

echo "=== gang blocks with dynamic headers"
mk gangnew gangfiles D
ganged gangnew
s=$(cat "$W/zbt_gangnew.dgh")
echo "  dynamic_gang_header: $s"
[ "$s" = active ] || { echo "  the large headers were never written"; fail=1; }
check gangnew

echo "=== two top-level vdevs"
mk stripe files "D D"
check stripe 0,1
check stripe 1,0
check1 stripe 0 random

echo "=== a two-way mirror"
mk mirror files "mirror D D"
check mirror 0
check mirror 1
wipe mirror 0
check mirror 0,1
check mirror 0 wrong

echo "=== a mirror with one side left behind"
mk stale stalefiles "mirror D D"
check stale 1,0
check stale 0,1
check1 stale 1 after

echo "=== two two-way mirrors"
mk mirrors files "mirror D D mirror D D"
check mirrors 0,1,2,3
check mirrors 3,2,1,0
check mirrors 0,2
check1 mirrors 0,1 random

[ $fail -eq 0 ] && echo "=== all pools read back" || echo "=== FAILED"
exit $fail
