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
# The users commands, and the disk ones, stand in for pw and gpart.
sed -i "s/^export SYSTEM_FILES_FREEBSD='chflags/export SYSTEM_FILES_FREEBSD='useradd userdel groupadd groupdel usermod vnconfig chflags/" \
    "$Z/zfs-tests/include/commands.cfg"
grep -q "FREEBSD='useradd" "$Z/zfs-tests/include/commands.cfg"
mkdir -p /usr/local/sbin
install -m 755 "$HERE/sysctl-wrapper" /usr/local/sbin/sysctl
# Volumes appear under dsk and rdsk, as on illumos.
cat >> "$Z/zfs-tests/include/default.cfg" <<'EOT'
ZVOL_DEVDIR=/dev/zvol/dsk
ZVOL_RDEVDIR=/dev/zvol/rdsk
EOT
sed -i 's/ls -U /ls -A /' "$Z/zfs-tests/tests/functional/cp_files/cp_files_001_pos.ksh"
sed -i 's/ dd if=/ \/usr\/pkg\/bin\/gdd if=/' "$Z/zfs-tests/tests/functional/suid/suid_write_zil_replay.ksh"
# GNU dd's K suffix and iflag=fullblock.
sed -i 's/| dd bs=10K/| \/usr\/pkg\/bin\/gdd bs=10K/' "$Z/zfs-tests/include/libtest.shlib"
grep -q 'gdd bs=10K' "$Z/zfs-tests/include/libtest.shlib"
cat "$HERE/netbsd-override.shlib" >> "$Z/zfs-tests/include/libtest.shlib"
