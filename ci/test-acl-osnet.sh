#!/bin/sh
# Scratch: the same cases on NetBSD's stock zfs (osnet), stock module.
set -u
export TERM=vt100
uname -a
modload zfs; echo "modload rc=$?"; modstat | grep -E '^(zfs|solaris) '
dd if=/dev/zero of=/var/tmp/q.img bs=1m count=1024 2>/dev/null
vndconfig vnd0 /var/tmp/q.img; echo "vnd rc=$?"
zpool create -f -m /tank tank /dev/vnd0d; echo "zpool rc=$?"
zfs create -o aclinherit=passthrough -o aclmode=passthrough tank/s; echo "zfs create rc=$?"
zpool get version tank | tail -1; zfs get -H aclmode,aclinherit tank/s; mount | grep tank
which vim rsync ex getfacl setfacl
for g in admins teachers students everybody; do groupadd $g; done
for u in ad te t2 st ou lo; do useradd -m -s /bin/sh $u; done
usermod -G admins,everybody ad; usermod -G teachers,everybody te; usermod -G teachers,everybody t2
usermod -G students,everybody st; usermod -G everybody ou
id te; id lo
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
	try $d $u "ex -s -c 'wq' $f"
	try $d $u "sh -c 'printf \"%%s/a/b/\\nwq\\n\" | ex -s $f'"
	try $d $u "cp /var/tmp/src $f"
	try $d $u "sed -i s/a/b/ $f"
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
echo '##### traversal: may ou pass a directory that denies it execute?'
echo pub > $a/p; chown root:wheel $a/p; setfacl -b $a/p; chmod 644 $a/p; getfacl -q $a/p | head -4
as ou "stat -f %Sp $a/p"; as ou "cat $a/p"; as ou "ls -l $a/p"; as ou "chmod 666 $a/o"
as ou "sh -c 'cd $a && pwd'"
echo '--- control: plain mode 700 directory, no ACL'
mkdir /tank/s/pl; chmod 700 /tank/s/pl; echo pub > /tank/s/pl/p; chmod 644 /tank/s/pl/p
as ou "stat -f %Sp /tank/s/pl/p"; as ou "cat /tank/s/pl/p"
echo '--- control: same with one deny ACE, no group trick'
mkdir /tank/s/pd; chmod 755 /tank/s/pd; setfacl -m u:ou:x::deny /tank/s/pd; echo pub > /tank/s/pd/p; chmod 644 /tank/s/pd/p; getfacl -q /tank/s/pd | head -3
as ou "stat -f %Sp /tank/s/pd/p"; as ou "cat /tank/s/pd/p"
echo '##### done'
