#!/bin/sh
# Scratch: make an installed ZFS Test Suite run on NetBSD without touching the
# port.  NetBSD is treated as FreeBSD except in netbsd-override.shlib.
#   sh port-tests.sh <pkgdatadir>
set -eu
Z=$1; HERE=$(cd "$(dirname "$0")" && pwd)
# Case arms and is_freebsd, everywhere in the suite.
find "$Z/zfs-tests" -type f \( -name '*.ksh' -o -name '*.kshlib' -o -name '*.shlib' -o -name '*.cfg' \) \
    -exec sed -i -E \
    -e 's/^([[:space:]]*)FreeBSD\)/\1FreeBSD|NetBSD)/' \
    -e 's/\[ "\$UNAME" = "FreeBSD" \]/[ "$UNAME" = "FreeBSD" -o "$UNAME" = "NetBSD" ]/' {} +
# The tunable table has one column per system.
sed -i 's/^while read name FreeBSD Linux; do$/& NetBSD=$FreeBSD/' \
    "$Z/zfs-tests/include/tunables.cfg"
grep -q 'do NetBSD=' "$Z/zfs-tests/include/tunables.cfg"
sed -i -E \
    -e 's/\[ "\$UNAME" = "FreeBSD" \]/[ "$UNAME" = "FreeBSD" -o "$UNAME" = "NetBSD" ]/' \
    -e 's|/usr/local/bin/ksh93|/usr/pkg/bin/ksh93|g' \
    -e 's|^([[:space:]]*)SYSTEM_DIRS="\$SYSTEM_DIRS /usr/bin /usr/sbin /bin /sbin \$LIBEXEC_DIR"|\1SYSTEM_DIRS="$SYSTEM_DIRS /usr/bin /usr/sbin /bin /sbin /usr/pkg/bin /usr/pkg/sbin /usr/pkg/gnu/bin $LIBEXEC_DIR"|' \
    "$Z/zfs-tests.sh"
grep -q /usr/pkg/gnu/bin "$Z/zfs-tests.sh"
cat "$HERE/netbsd-override.shlib" >> "$Z/zfs-tests/include/libtest.shlib"
