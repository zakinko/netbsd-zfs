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
deny='g:everybody:full_set:fi:deny'
share() { # dir teacher-perms
	mkdir -p $1; chown root:wheel $1; chmod 755 $1; setfacl -b $1
	setfacl -m "$base,g:teachers:$2:fd:allow,g:students:read_set:fi:allow,$deny" $1
}
as() { su -m "$1" -c "$2" >/dev/null 2>&1; echo "  rc=$? $1: $2" | cut -c1-90; }
rights() { # file
	stat -f '  mode %Sp owner %Su %N' $1
	for u in ad te t2 st ou lo; do
		su -m $u -c "cat $1" >/dev/null 2>&1 && r=r || r=-
		su -m $u -c "sh -c ': >> $1'" >/dev/null 2>&1 && w=w || w=-
		printf '  %s:%s%s' $u $r $w
	done; echo
}
named() { getfacl -q $1 | grep -c 'group:\(admins\|teachers\|students\|everybody\)'; }
tools() { # dir label
	d=$1; echo "##### $2"
	su -m te -c "umask 022; echo a > $d/f"; rights $d/f
	echo '--- chmod by the other teacher'
	as t2 "chmod 600 $d/f"; rights $d/f
	as t2 "chmod 644 $d/f"; rights $d/f
	echo "--- save tools, by the other teacher (named ACEs now $(named $d/f))"
	as t2 "sh -c 'echo 1 >> $d/f'"
	as t2 "vim -u NONE -es -c 'normal Ax' -c wq $d/f"; rights $d/f
	as t2 "vim -u NONE --cmd 'set backupcopy=no' -es -c 'normal Ay' -c wq $d/f"; rights $d/f
	as t2 "ex -s -c 'normal Ax' -c wq $d/f"
	as t2 "sed -i '' s/a/b/ $d/f"; rights $d/f
	echo x > /var/tmp/src; chmod 644 /var/tmp/src
	as t2 "cp -p /var/tmp/src $d/f"; rights $d/f
	as t2 "cp -pf /var/tmp/src $d/n1"; rights $d/n1
	as t2 "install -m 640 /var/tmp/src $d/n2"; rights $d/n2
	(cd /var/tmp; rm -f t.tar; tar cf t.tar src)
	as t2 "tar -xpf /var/tmp/t.tar -C $d"; rights $d/src
	as t2 "rsync -a /var/tmp/src $d/n3"; rights $d/n3
	as t2 "rsync -a /var/tmp/src $d/f"; rights $d/f
	as t2 "sh -c 'echo new > $d/t && mv $d/t $d/f'"; rights $d/f
	echo "--- final ACL of f"; getfacl -q $d/f
}
a=/tank/s/nowa; b=/tank/s/wa
share $a modify_set
share $b modify_set,write_acl
tools $a 'teachers without write_acl'
tools $b 'teachers with write_acl'
echo "--- with write_acl a teacher can also rewrite the ACL itself"
as t2 "setfacl -m g:everybody:full_set::allow $b/f"
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
as ou "ls $a/sub"; as ou "touch $a/sub/x"
echo '##### NFSv4 over loopback'
sysctl vfs.nfs.enable_uidtostring=1 vfs.nfsd.enable_stringtouid=1
sysrc -q rpcbind_enable=YES nfs_server_enable=YES nfsv4_server_enable=YES nfsuserd_enable=YES mountd_enable=YES nfscbd_enable=YES >/dev/null
printf 'V4: /tank\n/tank/s -maproot=root -alldirs localhost\n' > /etc/exports
for s in rpcbind nfsuserd mountd nfsd nfscbd; do service $s start 2>&1 | tail -1; done
python=0; mkdir /mnt/n; sleep 3
mount -t nfs -o nfsv4 localhost:/s /mnt/n; echo "  mount rc=$?"; mount | grep /mnt/n
n=/mnt/n/nowa
su -m te -c "umask 022; echo a > $n/v"; rights $n/v
getfacl -q $n/v; rights $a/v
as te "chmod 777 $n/v"; rights $n/v; getfacl -q $a/v
as te "chmod 000 $n/v"; rights $n/v
as te "vim -u NONE -es -c 'normal Ax' -c wq $n/v"; rights $n/v
as t2 "chmod 644 $n/v"; rights $n/v
as t2 "cp -p /var/tmp/src $n/v"; rights $n/v
umount /mnt/n
echo '##### done'
