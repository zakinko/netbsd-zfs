#!/bin/sh
#
# The four changes outside libsa, as a diff rather than as sed: the
# hunks are small but they sit in the middle of files nobody here owns,
# and patch(1) will refuse rather than half-apply if the tree has moved.
#
#	sh patch-tree.sh /path/to/usr/src
#
# Applying twice is not an error: -N skips a hunk that is already in,
# which is what makes install.sh safe to run again after a fetch.
#
# -f and </dev/null on every patch(1): without them, a diff that names a
# file patch cannot find turns into a "File to patch:" prompt, and a
# prompt with no terminal behind it spins.  -f also stops patch quietly
# deciding a diff is reversed.
#
# The diff therefore goes in with -i and not on stdin.  Both at once --
# "patch < diff </dev/null" -- is two redirections of the same
# descriptor, the second wins, and patch reads /dev/null and says "I
# can't seem to find a patch in there anywhere", which sounds like the
# diff is malformed.
#
# -C is NetBSD patch's check-only run.  It is not --dry-run: that is
# GNU's spelling, and NetBSD's patch answers it with "unknown option"
# and then "I can't seem to find a patch in there anywhere", which
# reads like the diff being at fault.
#
# The diff is in the ordinary a/ b/ shape, hence -p1.
set -e

SRC=$1
[ -n "$SRC" ] || { echo "usage: $0 <srcdir>" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
DIFF=$HERE/netbsd-side.diff

cd "$SRC"
if patch -p1 -f -N -s -C -i "$DIFF" </dev/null 2>/dev/null; then
	patch -p1 -f -N -s -i "$DIFF" </dev/null
	echo "tree patched"
elif patch -p1 -f -R -s -C -i "$DIFF" </dev/null 2>/dev/null; then
	echo "tree already carries the changes"
else
	echo "netbsd-side.diff does not apply to $SRC" >&2
	exit 1
fi
