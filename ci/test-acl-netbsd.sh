#!/bin/sh
# Scratch: aclmode=passthrough on the OpenZFS NetBSD port, same cases as FreeBSD.
# Builds zakinko/zfs bsd-kernel against 11.0 sys and runs on the stock kernel.
set -u
export TERM=vt100
W=/var/tmp/z; mkdir -p $W; cd $W || exit 1
ftp -o sys.tgz https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.0/source/sets/syssrc.tgz || exit 1
mkdir nb; tar xzf sys.tgz -C nb; ls nb/usr/src | head -3
git clone -q --depth 1 -b suiddir https://github.com/zakinko/zfs zfs; git -C zfs log -1 --format='%h %cs %s'
cd zfs || exit 1
(sh autogen.sh && PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig PKG_CONFIG_PATH= MAKE=gmake \
    ./configure --with-config=user --prefix=/usr --exec-prefix= --sysconfdir=/etc \
    --localstatedir=/var --disable-nls --with-mounthelperdir=/sbin &&
    gmake -j4 && gmake install DESTDIR=$W/inst) > $W/ub.log 2>&1; echo "userland rc=$?"; tail -5 $W/ub.log
[ -x $W/inst/sbin/zfs ] || { grep -nE ' error|Error ' $W/ub.log | head -20; exit 1; }
mkdir $W/kobj; cd module || exit 1
make -f Makefile.netbsd S=$W/nb/usr/src/sys MAKEOBJDIR=$W/kobj MKDEBUG=no MKCTF=no -j4 > $W/kb.log 2>&1
echo "module rc=$?"; tail -3 $W/kb.log; ls -l $W/kobj/zfs.kmod || { grep -n ' error' $W/kb.log | head -20; exit 1; }
cd /var/tmp
export PATH=$W/inst/sbin:$W/inst/usr/sbin:$PATH LD_LIBRARY_PATH=$W/inst/lib
uname -a
modload $W/kobj/zfs.kmod; echo "modload rc=$?"; modstat | grep -E '^(zfs|solaris) '
dd if=/dev/zero of=/var/tmp/q.img bs=1m count=1024 2>/dev/null
zpool create -f -m /tank tank /var/tmp/q.img; echo "zpool rc=$?"
zfs create -o aclinherit=passthrough -o aclmode=passthrough tank/s; echo "zfs create rc=$?"
zfs version; zfs get -H aclmode,aclinherit,acltype tank/s; mount | grep tank
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
useradd_so() { useradd -m -s /bin/sh so; }
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
