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
#   unaligned	a disk whose size is not a multiple of 256K, with its
#		first two labels gone
#   zstd	zstd at four levels, over text, program text and runs
#   cksum	sha512, skein, edonr and blake3, the last three keyed
#		with the pool's salt
#   raidz1..3	3, 4 and 5 disks; read whole, with disks gone and with
#		disks zeroed, up to as many as there is parity
#   raidz3w	raidz3 of 7, three disks gone
#   raidzx1	raidz1 of 3 widened to 4, files written before and after;
#		whole, a disk gone and a disk zeroed
#   raidzx2	raidz2 of 4 widened twice, to 6; the same, two at a time
#   raidzxp	raidz1 of 3 being widened, the move paused part way
#   raidzxs	the same paused just past the scratch area, rewritten
#		into the state of a crash while the scratch copy was
#		the only good one
#
# The raidz_expansion cases need OpenZFS 2.3; with an older one they
# are skipped, and say so.
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
R=$P/raidz_expand_max_reflow_bytes
old_rf=$(cat $R 2>/dev/null || true)
cleanup() {
	echo "$old_fg" > $P/metaslab_force_ganging
	echo "$old_pct" > $P/metaslab_force_ganging_pct
	[ -z "$old_rf" ] || echo "$old_rf" > $R
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
	../src/zle.c ../src/zstd.c ../src/scratch.c ../src/sha512.c \
	../src/skein.c ../src/edonr.c ../src/blake3.c fuzz_pool.c -o fuzz_pool -lz

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
			truncate -s "${MKSIZE:-128M}" "$w"
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
	    > "$W/$name.dgh" 2>/dev/null || true
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

# zstd at the default level and at both ends, over data that exercises
# its different block types: text for Huffman literals and long
# sequences, machine code for everything at once, runs for RLE, and a
# small file for a single short block.
zstdfiles() {
	pool=${1##*/}
	for lv in zstd zstd-1 zstd-19 zstd-fast-10; do
		zfs create -o compression=$lv "$pool/$lv"
		d=$1/$lv
		seq 1 300000 > "$d/text"
		cat /usr/bin/* 2>/dev/null | head -c 3000000 > "$d/prog"
		yes 'the same line again' | head -c 1000000 > "$d/runs"
		printf 'short\n' > "$d/small"
	done
	zfs create -o compression=zstd -o recordsize=16k "$pool/zstd16k"
	cat /usr/bin/* 2>/dev/null | head -c 1000000 > "$1/zstd16k/prog"
}

# A dataset per checksum, each with a file of a few blocks.  Their
# metadata is checksummed the same way, so mounting one already reads
# blocks that need the salt.
ckfiles() {
	pool=${1##*/}
	for ck in sha512 skein edonr blake3; do
		zfs create -o checksum=$ck "$pool/$ck"
		cat /usr/bin/* 2>/dev/null | head -c 700000 > "$1/$ck/prog"
		seq 1 20000 > "$1/$ck/text"
	done
}

# How many block pointers zdb reports with one checksum.
ckcount() {
	zdb -e -p "$W" -ddddd -bbbbbb "zbt_$1" 2>/dev/null |
	    grep -c " $2 [a-z0-9-]* unencrypted" || true
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
	if [ "$expect" = ok ]; then
		./fuzz_pool "$img" 0 "${FUZZ:-/random}" 1000 7 |
		    sed 's/^/  /'
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

# How many block pointers zdb reports as zstd, across the pool.  Only
# six or more -b make zdb print a block pointer whole, with the name
# of its compression; the feature's description also says "zstd",
# hence the word after it.
zstdcount() {
	zdb -e -p "$W" -ddddd -bbbbbb "zbt_$1" 2>/dev/null |
	    grep -c ' zstd unencrypted' || true
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

# dynamic_gang_header arrived in OpenZFS 2.3.  An older one writes only
# [S]'s headers, and has no feature to turn off.
if zpool upgrade -v | grep -q dynamic_gang_header; then
	dgh=yes
	nodgh="-o feature@dynamic_gang_header=disabled"
else
	dgh=no
	nodgh=
fi

echo "=== gang blocks with 512 byte headers"
mk gangold gangfiles D $nodgh
ganged gangold
check gangold

echo "=== gang blocks with dynamic headers"
if [ $dgh = yes ]; then
	mk gangnew gangfiles D
	ganged gangnew
	s=$(cat "$W/zbt_gangnew.dgh")
	echo "  dynamic_gang_header: $s"
	[ "$s" = active ] ||
	    { echo "  the large headers were never written"; fail=1; }
	check gangnew
else
	echo "  not in this OpenZFS; skipped"
fi

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

# [Z] vdev.c rounds a device's size down to a whole number of labels
# before placing the last two, so on a device whose size is not a
# multiple of 256K they sit short of its end.  With the first two gone
# they are all there is.
echo "=== the trailing labels, on an unaligned device"
MKSIZE=$((128 * 1048576 + 100 * 1024)) mk unaligned files D
dd if=/dev/zero of="$W/zbt_unaligned-0.img" bs=256K count=2 \
    conv=notrunc status=none
check unaligned

echo "=== zstd"
mk zstd zstdfiles D
z=$(zstdcount zstd)
echo "  $z block pointers compressed with zstd"
[ "${z:-0}" -gt 0 ] || { echo "  nothing was written with zstd"; fail=1; }
FUZZ=zstd:/prog check zstd

echo "=== sha512, skein, edonr and blake3"
mk cksum ckfiles D
for ck in sha512 skein edonr blake3; do
	c=$(ckcount cksum $ck)
	echo "  $c block pointers checksummed with $ck"
	[ "${c:-0}" -gt 0 ] || { echo "  nothing was written with $ck"; fail=1; }
done
FUZZ=blake3:/prog check cksum

# A missing disk and a zeroed one both leave a column that only P can
# give back, one by I/O error and one by checksum.  The pool's blocks
# lie at offsets across many megabytes of the vdev, both sides of the
# 1MB boundaries where single parity swaps its first two columns.
for np in 1 2 3; do
	echo "=== raidz$np"
	layout=raidz$np
	i=0
	while [ $i -lt $((np + 2)) ]; do
		layout="$layout D"
		i=$((i + 1))
	done
	mk raidz$np files "$layout"
	all=$(seq 0 $((np + 1)) | paste -sd, -)
	check raidz$np "$all"
	check raidz$np "${all%,*}"
	wipe raidz$np 0
	check raidz$np "$all"
	if [ $np -ge 2 ]; then
		# np + 2 disks, disk 0 zeroed.
		#   1,2        disk 0 and np - 1 others gone: np bad
		#   0,1,2      disk 0 zeroed, np - 1 others gone: np bad
		#   all        disks 0 and 1 zeroed, none gone: two bad,
		#              and only the checksum says which
		check raidz$np 1,2
		check raidz$np 0,1,2
		wipe raidz$np 1
		check raidz$np "$all"
		# 0,1: both zeroed and np others gone, one past the parity.
		check1 raidz$np 0,1 random
	fi
done
# Two columns gone is past single parity.
check1 raidz1 0 random

# Five disks under triple parity leave a block two data columns, so
# the three column solve above is never reached.  Seven leave four:
# with three disks gone, some blocks lose three data columns and need
# P, Q and R together, and others one or two in each combination.
echo "=== raidz3 of seven, three gone"
mk raidz3w files "raidz3 D D D D D D D"
check raidz3w 0,1,2,3
check raidz3w 0,2,4,6
check1 raidz3w 0,1,2 random

# Widening a raidz ([Z] vdev_raidz.c, the block comment on expansion)
# leaves the blocks already written at the old width, moved row by row
# onto the new children, and later ones at the new width.  Each file set
# below is written at a different width, so reading them all back takes
# every width the raidz has had.
#
# widen <mountpoint> <widenings>: files, then that many new disks, each
# followed by files of its own.
widen() {
	pool=${1##*/}
	files "$1"
	zpool sync "$pool"
	vd=$(zpool status "$pool" | awk '$1 ~ /^raidz[0-9]-0$/ { print $1; exit }')
	n=$(ls "$W/$pool"-*.img | wc -l)
	k=0
	while [ $k -lt "$WIDEN" ]; do
		d=$W/$pool-$n.img
		truncate -s 128M "$d"
		zpool attach "$pool" "$vd" "$d"
		zpool wait -t raidz_expand "$pool"
		zfs create -o compression=off "$pool/w$k"
		head -c 1048576 /dev/urandom > "$1/w$k/random"
		seq 1 50000 > "$1/w$k/text"
		n=$((n + 1)); k=$((k + 1))
	done
}

# paused <mountpoint>: data enough to reach past the scratch area when
# $BIG is set, then a new disk, with the move held at $REFLOW bytes, and
# files written while it is held.  The pool is exported mid-move, which
# keeps raidz_expanding in the label and the progress in the uberblock.
paused() {
	pool=${1##*/}
	files "$1"
	[ -z "${BIG:-}" ] || head -c 41943040 /dev/urandom > "$1/big"
	zpool sync "$pool"
	vd=$(zpool status "$pool" | awk '$1 ~ /^raidz[0-9]-0$/ { print $1; exit }')
	n=$(ls "$W/$pool"-*.img | wc -l)
	echo "$REFLOW" > $R
	truncate -s 128M "$W/$pool-$n.img"
	zpool attach "$pool" "$vd" "$W/$pool-$n.img"
	i=0
	until zpool status "$pool" | grep -q 'copied'; do
		i=$((i + 1)); [ $i -lt 60 ] || break; sleep 1
	done
	sleep 5
	zfs create -o compression=off "$pool/during"
	head -c 1048576 /dev/urandom > "$1/during/random"
}

# somewrong <pool>: at least one file has to come out wrong.  Each
# check runs in a subshell, so what it would count as a failure does
# not reach $fail; only the tally here does.
somewrong() {
	cp "$W/zbt_$1.sha256" "$W/all.sha256"
	nw=0
	while read -r line; do
		echo "$line" > "$W/zbt_$1.sha256"
		out=$(check "$1" "" wrong 2>&1)
		case $out in *"as it should be"*) nw=$((nw + 1)) ;; esac
	done < "$W/all.sha256"
	cp "$W/all.sha256" "$W/zbt_$1.sha256"
	echo "  $1: $nw of $(wc -l < "$W/all.sha256") files wrong"
	[ $nw -gt 0 ] || fail=1
}

# The reflow state of the newest uberblock, as zdb has it.
reflow() {
	zdb -lu "$W/zbt_$1-0.img" | awk '
	    $1 == "txg" { t = $3 }
	    /raidz_reflow/ && t + 0 >= best + 0 { best = t; s = $0 }
	    END { sub(/^[ \t]*/, "", s); print "txg " best ": " s }'
}

# The tunable alone is no sign: Ubuntu's 2.2 has it and still refuses to
# attach to a raidz.  The feature is what says zpool attach will widen.
if zpool upgrade -v | grep -q '^raidz_expansion'; then
	echo "=== raidz1 widened once"
	WIDEN=1 mk raidzx1 widen "raidz1 D D D"
	check raidzx1
	check raidzx1 0,1,2
	check raidzx1 1,2,3
	wipe raidzx1 2
	check raidzx1
	check1 raidzx1 0,1 random

	echo "=== raidz2 widened twice"
	WIDEN=2 mk raidzx2 widen "raidz2 D D D D"
	check raidzx2
	check raidzx2 0,1,2,3
	check raidzx2 2,3,4,5
	wipe raidzx2 1
	check raidzx2 0,1,2,3,4
	check1 raidzx2 0,1,2 random

	echo "=== raidz1 being widened, the move paused"
	BIG=1 REFLOW=20971520 mk raidzxp paused "raidz1 D D D"
	echo 0 > $R
	echo "  $(reflow raidzxp)"
	check raidzxp
	check raidzxp 1,2,3
	wipe raidzxp 3
	check raidzxp

	# [Z] raidz_reflow_scratch_sync() moves the first rows through a
	# copy in the boot area, VDEV_BOOT_OFFSET on each child, and marks
	# the uberblock RRSS_SCRATCH_VALID while the real place may be
	# half written.  Nothing outside ztest stops it there, so the
	# state is made by hand: pause as soon after that step as the
	# tunable allows, copy the rows' new place into the scratch area,
	# zero the new place, and mark the newest uberblocks SCRATCH_VALID
	# at the copy's end.  The rows the move reached beyond that are
	# then read from where they were before it, which holds only while
	# no block lies in them -- a small pool, and every file matching
	# its SHA-256 afterwards, is what says so.  With the old marks the
	# same disks must lose files: the rows under the zeroed place hold
	# the pool's first blocks, and a reader that does not take them
	# from the scratch area finds zeros.  A file written during the
	# pause lies beyond them and may still read.
	echo "=== raidz1 being widened, only the scratch copy good"
	REFLOW=1 mk raidzxs paused "raidz1 D D D"
	echo 0 > $R
	echo "  $(reflow raidzxs)"
	for i in 0 1 2 3; do
		python3 - "$W/zbt_raidzxs-$i.img" 4 12 <<'PY'
import hashlib, struct, sys
path, children, ashift = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
f = open(path, 'r+b')
M = 1 << 20
# [Z] raidz_reflow_scratch_sync(): VDEV_BOOT_SIZE aligned to a sector,
# written at VDEV_BOOT_OFFSET, and the logical size it covers.
wsize = (7 << 19) & ~((1 << ashift) - 1)
f.seek(4 * M); new = f.read(wsize)
f.seek(M // 2); f.write(new)
f.seek(4 * M); f.write(b'\0' * wsize)
f.close()
PY
	done
	somewrong raidzxs
	for i in 0 1 2 3; do
		python3 - "$W/zbt_raidzxs-$i.img" 4 12 <<'PY'
import hashlib, os, struct, sys
path, children, ashift = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
f = open(path, 'r+b')
size = os.path.getsize(path) & ~(256 * 1024 - 1)
K = 1024
slot = 1 << max(ashift, 10)
wsize = (7 << 19) & ~((1 << ashift) - 1)
L = wsize * children
# [Z] uberblock_impl.h: ub_txg is word 2, ub_raidz_reflow_info follows
# the 128 byte rootbp and five more words; RRSS offset is in 512 byte
# units in the low 55 bits, the state in the 9 above.
labels = [0, 256 * K, size - 512 * K, size - 256 * K]
ubs = []
for lo in labels:
    for off in range(lo + 128 * K, lo + 256 * K, slot):
        f.seek(off); b = f.read(slot)
        if struct.unpack_from('<Q', b, 0)[0] == 0x00bab10c:
            ubs.append((struct.unpack_from('<Q', b, 16)[0], off, bytearray(b)))
top = max(t for t, _, _ in ubs)
n = 0
for t, off, b in ubs:
    if t != top:
        continue
    struct.pack_into('<Q', b, 208, (1 << 55) | (L >> 9))
    # [Z] vdev_label.c: the embedded checksum is SHA-256 of the slot
    # with the checksum field holding the slot's offset and three zeros.
    struct.pack_into('<4Q', b, slot - 32, off, 0, 0, 0)
    d = hashlib.sha256(bytes(b)).digest()
    struct.pack_into('<4Q', b, slot - 32, *struct.unpack('>4Q', d))
    f.seek(off); f.write(b); n += 1
print('  %s: %d uberblocks of txg %d marked SCRATCH_VALID at %d'
      % (path.split('/')[-1], n, top, L))
PY
	done
	echo "  $(reflow raidzxs)"
	check raidzxs
else
	echo "=== raidz expansion: this OpenZFS has none, skipped"
fi

# A label claiming as much parity as there are disks leaves no data
# column, and the column arithmetic divides by the difference.  Random
# corruption rarely makes that one value and nothing else wrong, so it
# is set on purpose, in every label of every disk, and the pool has to
# be refused rather than crash the reader.
echo "=== a raidz label with no room for data"
for i in 0 1 2; do
	python3 - "$W/zbt_raidz1-$i.img" <<'PY'
import sys
f = open(sys.argv[1], 'r+b')
d = f.read()
# XDR: name length 7, "nparity" padded to 8, type 8 (uint64), 1 element
pat = b'\0\0\0\x07nparity\0\0\0\0\x08\0\0\0\x01'
at = d.find(pat)
n = 0
while at >= 0:
    f.seek(at + len(pat))
    f.write((3).to_bytes(8, 'big'))
    n += 1
    at = d.find(pat, at + 1)
print('  nparity set to 3 in %d labels of %s' % (n, sys.argv[1].split('/')[-1]))
PY
done
rc=0
./t_cat "$W/zbt_raidz1-0.img,$W/zbt_raidz1-1.img,$W/zbt_raidz1-2.img" \
    0 $(( $(wc -c < "$W/zbt_raidz1-0.img") / 512 )) "" /random "$W/out" \
    > "$W/msg" 2>&1 || rc=$?
msg=$(tail -1 "$W/msg")
if [ $rc -eq 1 ] && grep -q '^pool: ' "$W/msg"; then
	echo "  refused at open: $msg"
else
	echo "  not refused at open (exit $rc): $msg"
	fail=1
fi

echo "=== two two-way mirrors"
mk mirrors files "mirror D D mirror D D"
check mirrors 0,1,2,3
check mirrors 3,2,1,0
check mirrors 0,2
check1 mirrors 0,1 random

[ $fail -eq 0 ] && echo "=== all pools read back" || echo "=== FAILED"
exit $fail
