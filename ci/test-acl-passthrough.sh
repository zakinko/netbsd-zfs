#!/bin/sh
# Scratch: how far the stock aclmode values go for a share whose rights
# are all in named groups, with programs that chmod behind its back.
set -u
uname -a
kldload zfs 2>/dev/null
truncate -s 512m /var/tmp/p.img
md=$(mdconfig -a -t vnode -f /var/tmp/p.img)
zpool create -f -m /tank tank $md
zfs create -o aclinherit=passthrough -o aclmode=passthrough tank/s
pw groupadd admins; pw groupadd teachers; pw groupadd students; pw groupadd everybody
for u in ad te st ou; do pw useradd $u -m -s /bin/sh; done
pw groupmod admins -m ad; pw groupmod teachers -m te; pw groupmod students -m st
pw groupmod everybody -m ad,te,st,ou
as() { su -m "$1" -c "$2" >/dev/null 2>&1 && echo "  $1: ok   ($2)" || echo "  $1: FAIL ($2)"; }
check() {
	f=$1
	stat -f '  mode %Sp %N' $f
	getfacl -q $f | sed 's/^/    /'
	for u in ad te st ou; do as $u "cat $f"; done
	for u in te st ou; do as $u "sh -c 'echo x >> $f'"; done
}
acl() {
	setfacl -b $1
	setfacl -m g:admins:full_set:fd:allow,g:teachers:modify_set:fd:allow,g:students:read_set:fd:allow,g:everybody:full_set:fd:deny $1
}
d=/tank/s/share
mkdir $d; chown root:wheel $d; chmod 755 $d
acl $d
echo "=== directory"; getfacl -q $d
echo "=== new file by teacher"
su -m te -c "umask 022; echo a > $d/f"
check $d/f
for m in 600 644 666 777 000; do
	echo "=== chmod $m by owner"
	su -m te -c "chmod $m $d/f" || echo chmod-failed
	check $d/f
done
echo "=== cp -p then rename, as an editor saves"
su -m te -c "cp -p $d/f $d/f.new && mv $d/f.new $d/f"
check $d/f
echo "=== cp from outside with umask 022"
echo b > /var/tmp/o; chmod 644 /var/tmp/o
su -m te -c "umask 022; cp /var/tmp/o $d/g"
check $d/g
echo "=== chmod to the same mode under restricted"
zfs set aclmode=restricted tank/s
su -m te -c "umask 022; echo r > $d/r"
m=$(stat -f %Lp $d/r); echo "  mode $m"
su -m te -c "chmod $m $d/r"; echo "  same-mode rc=$?"
su -m te -c "chmod 600 $d/r"; echo "  other-mode rc=$?"
zpool destroy tank
