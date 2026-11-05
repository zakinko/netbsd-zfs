#!/bin/sh
# The pool was written by the patched kernel and has the feature active.
zstd -dqf /var/tmp/pool-old.img.zst -o /var/tmp/old.img
MD=$(mdconfig -a -t vnode -f /var/tmp/old.img)
echo "== read-write import"; zpool import -f -d /dev/$MD old; echo "rc=$?"
echo "== read-only import"; zpool import -f -o readonly=on -d /dev/$MD old; echo "rc=$?"
ls -la /old 2>&1 | head; getfacl /old/kept 2>&1; echo "getfacl rc=$?"
zpool export old; mdconfig -d -u $MD
