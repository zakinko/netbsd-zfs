#!/bin/sh
# Scratch: everything left open about aclmode=passthrough for a share
# whose rights are in named groups, closed with a deny on an all-users group.
set -u
uname -r
kldload zfs 2>/dev/null
truncate -s 1g /var/tmp/q.img
md=$(mdconfig -a -t vnode -f /var/tmp/q.img)
zpool create -f -m /tank tank $md
zfs create -o aclinherit=passthrough -o aclmode=passthrough tank/s
pw groupadd admins; pw groupadd teachers; pw groupadd students; pw groupadd everybody
for u in ad te t2 st ou lo; do pw useradd $u -m -s /bin/sh; done
pw groupmod admins -m ad; pw groupmod teachers -m te,t2; pw groupmod students -m st
pw groupmod everybody -m ad,te,t2,st,ou
# lo is deliberately in no group of the share, not even everybody.
base='g:admins:full_set:fd:allow'
deny='g:everybody:full_set:fd:deny'
share() { # dir teacher-perms
	mkdir -p $1; chown root:wheel $1; chmod 755 $1; setfacl -b $1
	setfacl -m "$base,g:teachers:$2:fd:allow,g:students:rxaRcs:fd:allow,$deny" $1
}
as() { o=$(su -m "$1" -c "$2" 2>&1 </dev/null); r=$?; echo "  rc=$r $1: $2" | cut -c1-90; [ -z "$o" ] || echo "$o" | head -5 | sed 's/^/    out: /' | cut -c1-140; }
rights() { # file
	stat -f '  mode %Sp owner %Su %N' $1
	for v in ad te t2 st ou lo; do
		su -m $v -c "cat $1" >/dev/null 2>&1 && r=r || r=-
		su -m $v -c "sh -c ': >> $1'" >/dev/null 2>&1 && w=w || w=-
		printf '  %s:%s%s' $v $r $w
	done; echo
}
named() { getfacl -q $1 | grep -c 'group:\(admins\|teachers\|students\|everybody\)'; }
fresh() { rm -f $1; su -m te -c "umask 022; echo a > $1"; }
try() { # dir user tool...
	d=$1; u=$2; shift 2; f=$d/f; fresh $f
	as $u "$*"; echo "  content: $(tr '\n' '|' < $f | cut -c1-40)"; rights $f; [ "$(named $f)" = 4 ] || echo "  ACL CHANGED: $(named $f) named ACEs"
}
tools() { # dir label
	d=$1; f=$d/f; echo "##### $2"
	echo x > /var/tmp/src; chmod 644 /var/tmp/src
	(cd /var/tmp; rm -f t.tar; cp src f; tar cf t.tar f)
	fresh $f; rights $f
	u=t2
	try $d $u "chmod 600 $f"
	try $d $u "sh -c 'echo 1 >> $f'"
	try $d $u "vim -u NONE -es -c 'normal Ax' -c wq $f"
	try $d $u "vim -u NONE --cmd 'set backupcopy=no' -es -c 'normal Ay' -c wq $f"
	try $d $u "vim -u NONE --cmd 'set backupcopy=yes' -es -c 'normal Ay' -c wq $f"
	try $d $u "ex -s -c '%s/a/b/' -c wq $f"
	try $d $u "sh -c 'printf \"%%s/a/b/\\nwq\\n\" | ex -s $f'"
	try $d $u "cp /var/tmp/src $f"
	try $d $u "sed -i '' s/a/b/ $f"
	try $d $u "cp -p /var/tmp/src $f"
	try $d $u "install -m 640 /var/tmp/src $f"
	try $d $u "tar -xpf /var/tmp/t.tar -C $d"
	try $d $u "rsync -a /var/tmp/src $f"
	try $d $u "sh -c 'echo new > $d/t && mv $d/t $f'"
	echo "--- final ACL of f"; getfacl -q $f
}
a=/tank/s/nowa; b=/tank/s/wa
share $a rwxpDdaARWcs
share $b rwxpDdaARWcCs
tools $a 'teachers without write_acl'
tools $b 'teachers with write_acl'
echo "--- with write_acl a teacher can also rewrite the ACL itself"
fresh $b/f; as t2 "setfacl -m g:students:rw::allow $b/f"; rights $b/f; getfacl -q $b/f | head -3
echo "--- so can any owner, write_acl or not"
fresh $a/f; as te "setfacl -m g:students:rw::allow $a/f"; rights $a/f
echo '##### owner outside the named groups'
touch $a/o; chown ou $a/o; rights $a/o
as ou "chmod 666 $a/o"; rights $a/o
echo '##### user outside the all-users group'
su -m te -c "umask 022; echo a > $a/l"; rights $a/l
as te "chmod 666 $a/l"; rights $a/l
echo '##### directories'
su -m te -c "mkdir $a/sub"; stat -f '  mode %Sp' $a/sub; getfacl -q $a/sub
as te "chmod 700 $a/sub"; stat -f '  mode %Sp' $a/sub
su -m te -c "umask 022; echo a > $a/sub/f"; rights $a/sub/f
as te "chmod 777 $a/sub"; rights $a/sub/f
as ou "ls $a/sub"; as ou "touch $a/sub/x"; as lo "ls $a/sub"; as st "ls $a/sub"
echo '--- chmod of the share itself by root'
chmod 777 $a; as ou "ls $a"; as ou "touch $a/x"; chmod 755 $a
echo '##### NFSv4 over loopback'
sysctl vfs.nfs.enable_uidtostring=1 vfs.nfsd.enable_stringtouid=1
sysrc -q rpcbind_enable=YES nfs_server_enable=YES nfsv4_server_enable=YES nfsuserd_enable=YES mountd_enable=YES nfscbd_enable=YES >/dev/null
printf 'V4: /tank\n/tank/s -maproot=root -alldirs localhost\n' > /etc/exports
for s in rpcbind nfsuserd mountd nfsd nfscbd; do service $s start 2>&1 | tail -1; done
mkdir /mnt/n; sleep 3
mount -t nfs -o nfsv4 localhost:/s /mnt/n; echo "  mount rc=$?"; mount | grep /mnt/n
n=/mnt/n/nowa
tools $n 'NFSv4, teachers without write_acl'
tools /mnt/n/wa 'NFSv4, teachers with write_acl'
fresh $n/v; as te "chmod 777 $n/v"; rights $n/v; as ou "ls $n"
umount /mnt/n
echo '##### done'
