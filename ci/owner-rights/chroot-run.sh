#!/bin/sh
# Run a command with the patched tools, in the chroot.
set -eu
W=/var/tmp/w
mount -t devfs | grep -q "$W/dev" || mount -t devfs devfs $W/dev
mkdir -p $W/usr/local $W/var/tmp/or
mount | grep -q "$W/usr/local" || mount_nullfs -o ro /usr/local $W/usr/local
mount | grep -q "$W/var/tmp/or" || mount_nullfs /var/tmp/or $W/var/tmp/or
exec chroot $W /usr/bin/env PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin "$@"
