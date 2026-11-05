#!/bin/sh
# No OWNER_RIGHTS@ here.  Runs on the stock 15.1 and on the patched system;
# the two outputs are diffed.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin
T=/var/tmp/or/acltests
id alice >/dev/null 2>&1 || pw useradd alice -u 2001
id bob >/dev/null 2>&1 || pw useradd bob -u 2002
truncate -s 256m /var/tmp/rg.img
MD=$(mdconfig -a -t vnode -f /var/tmp/rg.img)
zpool create -f -m /rg rg /dev/$MD
chmod 777 /rg
for m in discard groupmask passthrough restricted; do
	zfs set aclmode=$m rg
	cd /rg
	f=f.$m; d=d.$m
	chroot -u alice / touch /rg/$f
	chroot -u alice / mkdir /rg/$d
	setfacl -a0 u:bob:rw::allow $f; echo "$m set rc=$?"
	setfacl -a0 u:bob:r:fd:allow $d; echo "$m dset rc=$?"
	getfacl -qn $f
	chroot -u alice / chmod 640 /rg/$f; echo "$m chmod rc=$?"
	stat -f '%Lp' $f; getfacl -qn $f
	chroot -u alice / setfacl -a0 u:2002:w::deny /rg/$f; echo "$m owner setfacl rc=$?"
	touch $d/c; mkdir $d/s; getfacl -qn $d/c $d/s
	chmod 000 $d/c; stat -f '%Lp' $d/c; getfacl -qn $d/c
done
cd /
zpool destroy rg; mdconfig -d -u $MD; rm /var/tmp/rg.img
perl $T/run $T/tools-nfs4-psarc.test 2>&1 | tail -3
