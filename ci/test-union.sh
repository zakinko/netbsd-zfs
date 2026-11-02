#!/bin/sh
#
# Install the release built with ci/union-readdir.diff, boot it and run
# the ATF tests that go through vn_readdir and the union hooks.
#
#   sh ci/test-union.sh path/to/rel/amd64/

set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
REL=$1
WORK=${WORK:-$TOP/work}
ANITA_VER=2.18
LOG=$WORK/anita.log
QEMU_ARGS=
[ -w /dev/kvm ] && QEMU_ARGS="-accel kvm"

mkdir -p "$WORK"
curl -sSfL -o "$WORK/anita-$ANITA_VER.tar.gz" \
    "https://www.gson.org/netbsd/anita/download/anita-$ANITA_VER.tar.gz"
tar xzf "$WORK/anita-$ANITA_VER.tar.gz" -C "$WORK"

T='fs/union fs/nullfs fs/tmpfs fs/vfs lib/libc/gen kernel'
cd "$WORK/anita-$ANITA_VER"
python3 ./anita --vmm qemu --workdir "$WORK/anita-wd" \
    --disk-size 8G --memory-size 2G \
    ${QEMU_ARGS:+--qemu-args "$QEMU_ARGS"} --run-timeout 10800 \
    --run "uname -v; for t in $T; do echo ATF-DIR \$t; (cd /usr/tests/\$t && atf-run 2>/dev/null | atf-report -o ticker:- -o xml:/tmp/r.xml | tail -12); done; echo ATF-\"END\"" \
    boot "$REL" 2>&1 | tee "$LOG"

grep -q 'ATF-END' "$LOG" || { echo "tests did not finish"; exit 1; }
