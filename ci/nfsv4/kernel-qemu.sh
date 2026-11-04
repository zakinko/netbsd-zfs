#!/bin/sh
#
# Fetch the image vmactions uses, boot it under qemu directly with a serial
# console in a file, and hand it to kernel-swap.sh.  What this form has over
# the action: a kernel that panics leaves its message in console.log.
#
#   kernel-qemu.sh KERNEL MARK OUTDIR

set -eu

K=$1
MARK=$2
OUT=$3
B=https://github.com/anyvm-org/netbsd-builder/releases/download/v2.2.9
PORT=2225
W=$PWD/vm
mkdir -p "$W"

sudo apt-get update -q
sudo apt-get install -y -q qemu-system-x86 qemu-utils zstd >/dev/null
sudo chmod 666 /dev/kvm

curl -fsSL "$B/netbsd-11.0.qcow2.zst" | zstd -dc >"$W/netbsd.qcow2"
curl -fsSL "$B/netbsd-11.0-host.id_rsa" >"$W/id"
chmod 600 "$W/id"

qemu-system-x86_64 -name netbsd -m 4096 -smp 2 \
    -machine pc,accel=kvm,hpet=off -cpu host \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
    -device e1000,netdev=net0 \
    -drive file="$W/netbsd.qcow2",format=qcow2,if=virtio \
    -display none -serial file:"$OUT/console.log" -monitor none &
QEMU=$!
trap 'kill $QEMU 2>/dev/null || :' EXIT

set -- ssh -q -i "$W/id" -p "$PORT" -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 root@127.0.0.1
i=0
until "$@" true 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -lt 60 ] || { echo "no ssh"; cat "$OUT/console.log"; exit 1; }
	sleep 5
done

# The image boots to the VGA console; send the next boot to the serial port.
"$@" 'grep -q consdev /boot.cfg || sed -i.orig "1i\\
consdev=com0
" /boot.cfg; head -3 /boot.cfg'

sh "$(dirname "$0")/kernel-swap.sh" "$K" "$MARK" "$OUT/swap-qemu.log" "$@"
