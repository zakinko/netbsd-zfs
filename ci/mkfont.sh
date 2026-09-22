#!/bin/sh
#
# Generate sys/dev/wsfont/shnmk16.h, the kanji font the wscons UTF-8
# patch compiles into the kernel, from the Shinonome 16 dot font
# (public domain, the pkgsrc fonts/ja-shinonome distfile) and the
# Unicode consortium's JIS X 0208 mapping table.
#
#   sh ci/mkfont.sh path/to/src [cache-dir]

set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?src tree}
CACHE=${2:-$TOP/nbsrc/distfiles}

SHINONOME=shinonome-0.9.11p1.tar.bz2
SHINONOME_URL=https://cdn.netbsd.org/pub/pkgsrc/distfiles/$SHINONOME
SHINONOME_SHA256=95663c95c92ba5765f63ccbdf033eb93b707be01812a989c548db943479c838f
JIS0208_URL=https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/JIS/JIS0208.TXT
JIS0208_SHA256=1c571870457f19c97720631fa83ee491549a96ba1436da1296786a67d8632e87

fetch() {
	[ -f "$CACHE/$1" ] || curl -sSfL -o "$CACHE/$1" "$2"
	# no sha256sum on NetBSD or macOS, python is needed anyway
	python3 -c 'import hashlib, sys
sys.exit(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest() != sys.argv[2])' \
	    "$CACHE/$1" "$3" || { echo "$1: checksum mismatch" >&2; exit 1; }
}

mkdir -p "$CACHE"
fetch $SHINONOME $SHINONOME_URL $SHINONOME_SHA256
fetch JIS0208.TXT $JIS0208_URL $JIS0208_SHA256
tar xjf "$CACHE/$SHINONOME" -C "$CACHE" shinonome-0.9.11/bdf/shnmk16.bdf
python3 "$TOP/ci/bdf2wsfont.py" shnmk16 \
    "$CACHE/shinonome-0.9.11/bdf/shnmk16.bdf" "$CACHE/JIS0208.TXT" \
    > "$SRC/sys/dev/wsfont/shnmk16.h"
echo "wrote $SRC/sys/dev/wsfont/shnmk16.h"
