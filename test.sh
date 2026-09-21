#!/bin/sh
#
# Boot the image under qemu with a serial console, log in as root and
# check that / really is ZFS.  Needs qemu-system-x86_64 and expect.
#
#   sh test.sh out/netbsd-zfs.img

set -eu

IMG=${1:-out/netbsd-zfs.img}
LOG=${LOG:-boot.log}
ACCEL=${ACCEL:-}
[ -n "$ACCEL" ] || { [ -w /dev/kvm ] && ACCEL="-enable-kvm" || ACCEL=""; }
export ACCEL

# boot.cfg says consdev=auto, so with a serial port present the loader,
# the kernel and getty all end up on com0.
expect -f - "$IMG" <<'EXP' | tee "$LOG"
set img [lindex $argv 0]
set timeout 600
log_user 1
spawn sh -c "exec qemu-system-x86_64 $env(ACCEL) -m 2048 -smp 2 \
    -drive file=$img,format=raw,if=ide \
    -display none -serial stdio -no-reboot"
expect {
    -re "login: $" { send "root\r" }
    timeout { puts "\nTIMEOUT waiting for login prompt"; exit 1 }
    eof { puts "\nqemu exited before login prompt"; exit 1 }
}
expect -re "# $"
send "mount; echo; zpool status; echo; zfs list; echo; df -h /; echo CHECK-\"START\"; mount | grep ' on / type zfs' && echo ZFS-ROOT-\"OK\"; echo CHECK-\"END\"\r"
expect {
    "CHECK-END" {}
    timeout { puts "\nTIMEOUT running checks"; exit 1 }
}
send "shutdown -p now\r"
expect {
    eof {}
    timeout { puts "\nTIMEOUT waiting for poweroff"; exit 1 }
}
EXP

grep -q 'ZFS-ROOT-OK' "$LOG" || { echo "root is not on ZFS"; exit 1; }
echo "root on ZFS: OK"
