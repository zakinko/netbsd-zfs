#!/bin/sh
#
# Install NetBSD from the release tree choosing the "root on ZFS"
# layout in sysinst, boot the result and check that / is ZFS.  Uses
# anita (https://www.gson.org/netbsd/anita/) with the small patch in
# ci/anita-zfs-root.patch that knows about the new menu entry.
#
#   sh ci/test-install.sh [path/to/rel/amd64/]

set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
REL=${1:-$TOP/nbsrc/rel/amd64/}
WORK=${WORK:-$TOP/nbsrc}
ANITA_VER=2.18
LOG=${LOG:-$WORK/anita.log}
QEMU_ARGS=${QEMU_ARGS:-}
[ -n "$QEMU_ARGS" ] || { [ -w /dev/kvm ] && QEMU_ARGS="-accel kvm"; } || true

if [ ! -d "$WORK/anita-$ANITA_VER" ]; then
	mkdir -p "$WORK"
	curl -sSfL -o "$WORK/anita-$ANITA_VER.tar.gz" \
	    "https://www.gson.org/netbsd/anita/download/anita-$ANITA_VER.tar.gz"
	tar xzf "$WORK/anita-$ANITA_VER.tar.gz" -C "$WORK"
	patch -f -p1 -d "$WORK/anita-$ANITA_VER" < "$TOP/ci/anita-zfs-root.patch"
fi

rm -rf "$WORK/anita-wd"
cd "$WORK/anita-$ANITA_VER"
python3 ./anita --vmm qemu --workdir "$WORK/anita-wd" \
    --disk-size 4G --memory-size 1G --zfs-root \
    ${QEMU_ARGS:+--qemu-args "$QEMU_ARGS"} \
    --run 'mount; echo; zpool status; echo; df -h /; echo; cat /etc/fstab; echo; mount /altroot && ls -l /altroot; cat /altroot/boot.cfg; echo CHECK-"START"; mount | grep " on / type zfs" && echo ZFS-ROOT-"OK"; echo CHECK-"END"' \
    boot "$REL" 2>&1 | tee "$LOG"

grep -q 'ZFS-ROOT-OK' "$LOG" || { echo "root is not on ZFS"; exit 1; }
echo "root on ZFS: OK"
