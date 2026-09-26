#!/bin/sh
# Decode frames the zstd command writes, at many levels and over data
# chosen to reach each part of [R] RFC 8878, and compare with the input.
#
#	sh zstd.sh
#
# ZFS alone never reaches some of the format: it keeps a block that
# compresses badly uncompressed, so there are no Raw or RLE blocks; its
# blocks are 128K and one zstd block each, so nothing repeats a table
# from a block before; and it writes no content size.  A frame that
# does any of that is still a frame this has to read, and the zstd
# command writes them.  It is the reference implementation, so it is
# used here as a source of test data and not of code.
set -eu
cd "$(dirname "$0")"
command -v zstd >/dev/null || { echo "no zstd command; skipped"; exit 0; }
W=$(mktemp -d)
trap 'rm -rf "$W" t_zstd.dSYM fuzz_zstd.dSYM; rm -f t_zstd fuzz_zstd' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
cc -std=c99 -Wall -Wextra -O1 -g -I../src -include stdint.h \
	-include stddef.h -D_DEFAULT_SOURCE $SAN \
	t_zstd.c ../src/scratch.c -o t_zstd
cc -std=c99 -Wall -Wextra -O1 -g -I../src -include stdint.h \
	-include stddef.h -D_DEFAULT_SOURCE $SAN \
	../src/zstd.c ../src/scratch.c fuzz_zstd.c -o fuzz_zstd

echo "=== the predefined tables"
# RFC 8878 Appendix A, as corrected by errata 6441, is what these have
# to be; a checksum of it is kept rather than the tables themselves.
got=$(./t_zstd tables | cksum)
want="1457902726 2035"
if [ "$got" = "$want" ]; then
	echo "  160 cells as in Appendix A"
else
	echo "  tables differ from Appendix A: $got"
	exit 1
fi

# The inputs.
seq 1 50000 > "$W/text"
cat /bin/* 2>/dev/null | head -c 700000 > "$W/prog"
head -c 300000 /dev/urandom > "$W/random"
head -c 300000 /dev/zero > "$W/zeros"
printf 'a' > "$W/one"
printf 'hello' > "$W/five"
: > "$W/empty"
# A few letters only, so that a Huffman table has few weights and may
# be written directly rather than with FSE.
yes 'abcabdabe' | head -c 200000 > "$W/letters"
# Many short sequences in one block: bytes that match two back.
awk 'BEGIN { srand(7); for (i = 0; i < 60000; i++)
    printf "%c%c", 65 + int(rand() * 20), 65 + int(rand() * 20) }' \
    > "$W/pairs"
# A thousand bytes: a two byte Frame_Content_Size, which adds 256.
seq 1 300 | head -c 1000 > "$W/k1"
# Three letter words from a vocabulary of forty: a sequence every three
# bytes, more than the 0x7F00 that a two byte count can say.
awk 'BEGIN { srand(11); for (i = 0; i < 40; i++)
    w[i] = sprintf("%c%c%c", 97 + int(rand() * 26), 97 + int(rand() * 26),
    97 + int(rand() * 26)); for (i = 0; i < 60000; i++)
    printf "%s", w[int(rand() * 40)] }' > "$W/words3"
# Bytes 1 to 15 only: so few weights that the Huffman table is written
# four bits a weight rather than with FSE.
awk 'BEGIN { srand(3); for (i = 0; i < 100000; i++)
    printf "%c", 1 + int(rand() * rand() * 15) }' > "$W/small"

n=0 bad=0 fz=
for f in text prog random zeros one five empty letters pairs k1 words3 \
    small; do
	size=$(wc -c < "$W/$f" | tr -d ' ')
	for opt in -1 -3 -9 -19 --fast=3 "--ultra -22" --no-check \
	    --no-content-size "-19 --long"; do
		# shellcheck disable=SC2086
		zstd -q -f $opt "$W/$f" -o "$W/z" 2>/dev/null || continue
		n=$((n + 1))
		# Keep a few of each for the fuzzer below.
		case $opt in
		-1|-19)
			cp "$W/z" "$W/fz.$f$opt"
			fz="$fz $W/fz.$f$opt $size"
			;;
		esac
		if ./t_zstd dec "$W/z" "$size" "$W/out" > "$W/msg" &&
		    cmp -s "$W/$f" "$W/out"; then
			:
		else
			echo "  $f $opt: WRONG ($(cat "$W/msg"))"
			bad=$((bad + 1))
		fi
	done
done
echo "=== $n frames, $bad wrong"

# Frames put together by hand, for what the zstd command will not
# write.  The zstd command's decoder is the judge of what each one
# means, so they test this reader against it and not against itself.
#
# 32600 sequences in one block, more than a two byte count holds, so
# the count takes the three byte form ([R] 3.1.1.3.2.1).  An 8 byte Raw
# block first gives the matches something to copy.  All three symbol
# types in RLE_Mode, codes 0: literals length 0, match length 3,
# Offset_Value 1 -- which with no literals means Repeated_Offset2, so
# the offset alternates between 4 and 1 ([R] 3.1.1.5).  No bits are
# read at all, so the bitstream is its end mark alone.
echo "=== frames made by hand"
printf '\050\265\057\375\000\070\100\000\000ABCDEFGH' > "$W/hand"
printf '\115\000\000\000\377\130\000\124\000\000\000\001' >> "$W/hand"
if zstd -d -q -c "$W/hand" > "$W/hand.ref" &&
    ./t_zstd dec "$W/hand" "$(wc -c < "$W/hand.ref")" "$W/out" > "$W/msg" &&
    cmp -s "$W/hand.ref" "$W/out"; then
	echo "  a three byte sequence count: as zstd -d reads it"
else
	echo "  a three byte sequence count: WRONG ($(cat "$W/msg"))"
	bad=$((bad + 1))
fi
# Two that have to be refused: a block of the reserved type 3, and a
# frame header with its reserved bit set.
printf '\050\265\057\375\000\070\007\000\000' > "$W/r1"
printf '\050\265\057\375\010\070\001\000\000' > "$W/r2"
for r in r1 r2; do
	if zstd -d -q -c "$W/$r" > /dev/null 2>&1; then
		echo "  $r: zstd -d accepts it, so it does not test what it meant to"
		bad=$((bad + 1))
	elif ./t_zstd dec "$W/$r" 0 "$W/out" > /dev/null; then
		echo "  $r: accepted, and should not have been"
		bad=$((bad + 1))
	else
		echo "  $r: refused, as zstd -d refuses it"
	fi
done

# The frames above, corrupted, each copied into a buffer of exactly its
# own length so that reading one byte past it is caught.  Taking out
# the Raw_Block length check makes this report a heap overflow.
echo "=== the decoder under ASan and UBSan"
# shellcheck disable=SC2086
./fuzz_zstd 20000 7 $fz "$W/hand" 97808 | sed 's/^/  /'

[ $bad -eq 0 ]
