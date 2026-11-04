#!/bin/sh
# Scratch: suiddir on FreeBSD with OpenZFS from zakinko/zfs.
set -u
W=/var/tmp/z; mkdir -p $W; cd $W || exit 1
fetch -q -o src.txz https://download.freebsd.org/releases/amd64/15.0-RELEASE/src.txz || exit 1
tar xJf src.txz -C / usr/src/sys usr/src/include 2>/dev/null; ls /usr/src/sys | head -2
git clone -q --depth 1 -b suiddir https://github.com/zakinko/zfs zfs; git -C zfs log -1 --format='%h %s'
cd zfs || exit 1
(sh autogen.sh && MAKE=gmake ./configure --with-config=all && gmake -j4 && gmake install) > $W/b.log 2>&1; echo "build rc=$?"; tail -3 $W/b.log
ls -l module/openzfs.ko || { grep -n ' error' $W/b.log | head; exit 1; }
mount | grep -c ' zfs' ; zpool list 2>&1 | head -3; kldstat | grep -E 'zfs|Id'
kldunload zfs; echo "unload rc=$?"
kldload ./module/openzfs.ko; rc=$?; echo "kld rc=$rc"; kldstat | grep -i zfs
[ $rc = 0 ] || exit 1
export PATH=/usr/local/sbin:/usr/local/bin:$PATH LD_LIBRARY_PATH=/usr/local/lib
cd /var/tmp; hash -r 2>/dev/null; which zfs zpool
truncate -s 1g /var/tmp/q.img
md=$(mdconfig -a -t vnode -f /var/tmp/q.img)
zpool create -f -m /tank tank $md || exit 1
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
echo x > /var/tmp/src; chmod 644 /var/tmp/src; (cd /var/tmp; rm -f t.tar; cp src f; tar cf t.tar f)
useradd_so() { pw useradd so -m -s /bin/sh; }
echo '##### suiddir and a gate directory'
zfs set suiddir=on tank/s; zfs get -H -o value suiddir tank/s
useradd_so
g=/tank/s/gate; mkdir $g; chown root:wheel $g; chmod 750 $g; setfacl -b $g
setfacl -m "g:everybody:rxaRcs::allow" $g; getfacl -q $g | head -4
d=$g/share; mkdir $d; chown so:wheel $d; chmod 4755 $d
setfacl -m "$base,g:teachers:rwxpDdaARWcs:fd:allow,g:students:rxaRcs:fd:allow" $d
stat -f '  mode %Sp owner %Su %N' $d
tools2() { f=$d/f; for t in \
 "vim -u NONE --cmd 'set backupcopy=no' -es -c 'normal Ay' -c wq $f" \
 "sh -c 'echo new > $d/t && mv $d/t $f'" "rsync -a /var/tmp/src $f" "tar -xpf /var/tmp/t.tar -C $d" \
 "install -m 640 /var/tmp/src $f" "cp -p /var/tmp/src $f" "chmod 666 $f" "setfacl -m g:students:rw::allow $f"; do
	rm -f $f; su -m te -c "umask 022; echo a > $f"; stat -f '  mode %Sp owner %Su' $f
	as t2 "$t"; stat -f '  mode %Sp owner %Su' $f
	echo "  content: $(tr '\n' '|' < $f | cut -c1-30)"; rights $f
done; }
tools2
echo '--- same tools, teachers with write_acl'
d0=$d; d=$g/sharewa; mkdir $d; chown so:wheel $d; chmod 4755 $d
setfacl -m "$base,g:teachers:rwxpDdaARWcCs:fd:allow,g:students:rxaRcs:fd:allow" $d
tools2; d=$d0
echo '--- creator cannot touch the ACL or mode of what they made'
rm -f $d/m; su -m te -c "echo a > $d/m"; as te "chmod 666 $d/m"; as te "setfacl -m g:students:rw::allow $d/m"; rights $d/m; getfacl -q $d/m | head -5
echo '--- execute and subdirectory'
su -m te -c "umask 022; echo a > $d/x; chmod 755 $d/x"; stat -f '  mode %Sp owner %Su' $d/x
su -m te -c "mkdir $d/sd"; stat -f '  mode %Sp owner %Su' $d/sd; su -m te -c "echo a > $d/sd/g"; stat -f '  mode %Sp owner %Su' $d/sd/g
echo '--- root directory is never given away to'
mkdir $g/r; chmod 4777 $g/r; setfacl -m g:teachers:rwxpDdaARWcs:fd:allow $g/r; su -m te -c "echo a > $g/r/f"; stat -f '  mode %Sp owner %Su' $g/r/f
echo '--- gate: outsiders of the group after chmod 777 below it'
chmod 777 $d/m; chmod 777 $d; rm -f $d/m2; echo a > $d/m2; chmod 666 $d/m2
as lo "cat $d/m2"; as lo "ls $g"; as lo "stat -f %Sp $d/m2"; as st "cat $d/m2"; as ou "cat $d/m2"
echo '--- off: the property really is what changed this'
zfs set suiddir=off tank/s; rm -f $d/o; su -m te -c "echo a > $d/o"; stat -f '  mode %Sp owner %Su' $d/o
echo '##### done'
