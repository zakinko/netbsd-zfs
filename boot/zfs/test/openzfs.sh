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
#
# Each file is read through the reader and compared with the SHA-256
# ZFS gave it, and each pool is then fuzzed with the checksums out of
# the way, as ci.sh does.  A gang pool that was not actually ganged
# would pass for the wrong reason, so zdb is asked to count them.
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

# mk <name> <what to write> [zpool create options]
mk() {
	name=zbt_$1; fill=$2; shift 2
	img=$W/$name.img
	truncate -s 128M "$img"
	zpool create -f -o ashift=12 -O compression=lz4 -O atime=off \
	    -O mountpoint=$W/mnt/$name "$@" "$name" "$img"
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

fail=0

check() {
	name=zbt_$1
	img=$W/$name.img
	n=$(( $(wc -c < "$img") / 512 ))
	while read -r want f; do
		case $f in
		*/*)	ds=${f%/*}; fp=/${f##*/} ;;
		*)	ds=; fp=/$f ;;
		esac
		rm -f "$W/out"
		msg=$(./t_cat "$img" 0 $n "$ds" "$fp" "$W/out" 2>&1 | tail -1)
		got=$(sha256sum "$W/out" 2>/dev/null | cut -d' ' -f1)
		if [ "$got" = "$want" ]; then
			echo "  $1 $f: ok"
		else
			echo "  $1 $f: WRONG ($msg)"
			fail=1
		fi
	done < "$W/$name.sha256"
	./fuzz_pool "$img" 0 /random 1000 7 | sed 's/^/  /'
}

ganged() {
	name=zbt_$1
	g=$(zdb -e -p "$W" -bb "$name" 2>/dev/null | awk '/ganged count:/ { print $3 }')
	echo "  $1: $g gang blocks"
	[ "${g:-0}" -gt 0 ] || { echo "  $1 was not ganged"; fail=1; }
}

echo "=== OpenZFS $(zfs version | head -1)"

echo "=== a pool as OpenZFS makes it"
mk plain files
check plain

echo "=== gang blocks with 512 byte headers"
mk gangold gangfiles -o feature@dynamic_gang_header=disabled
ganged gangold
check gangold

echo "=== gang blocks with dynamic headers"
mk gangnew gangfiles
ganged gangnew
s=$(cat "$W/zbt_gangnew.dgh")
echo "  dynamic_gang_header: $s"
[ "$s" = active ] || { echo "  the large headers were never written"; fail=1; }
check gangnew

[ $fail -eq 0 ] && echo "=== all pools read back" || echo "=== FAILED"
exit $fail
