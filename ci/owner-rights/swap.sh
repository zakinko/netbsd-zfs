#!/bin/sh
# Put the kernel in /boot and the world in a chroot.
set -eu
S=/var/tmp/or/sets
mv /boot/kernel /boot/kernel.old
tar --zstd -xpf $S/kernel.tar.zst -C /
kldxref /boot/kernel
mkdir -p /var/tmp/w
tar --zstd -xpf $S/world.tar.zst -C /var/tmp/w
tar -C / -cf - etc | tar -C /var/tmp/w -xf -
