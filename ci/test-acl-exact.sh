#!/bin/sh
set -u
export TERM=vt100
# Scratch: the mail's ACL, exactly, on NetBSD (port) and FreeBSD (stock).
if [ "$(uname -s)" = NetBSD ]; then
W=/var/tmp/z; mkdir -p $W; cd $W || exit 1
ftp -o sys.tgz https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.0/source/sets/syssrc.tgz || exit 1
mkdir nb; tar xzf sys.tgz -C nb; ls nb/usr/src | head -3
git clone -q --depth 1 -b bsd-kernel https://github.com/zakinko/zfs zfs; git -C zfs log -1 --format='%h %cs %s'
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

fi

os=$(uname -s)
if [ $os = NetBSD ]; then
	modload $W/kobj/zfs.kmod; echo "modload rc=$?"
	dd if=/dev/zero of=/var/tmp/q.img bs=1m count=1024 2>/dev/null
	zpool create -f -m /tank tank /var/tmp/q.img; echo "zpool rc=$?"
	wheel=wheel
	for g in admins teachers students everybody outsiders; do groupadd $g; done
	for u in ad te t2 st ou lo; do useradd -m -s /bin/sh $u; done
	addg() { u=$1; shift; usermod -G "$(echo $* | tr ' ' ,)" $u; }
else
	kldload zfs 2>/dev/null
	truncate -s 1g /var/tmp/q.img; md=$(mdconfig -a -t vnode -f /var/tmp/q.img)
	zpool create -f -m /tank tank $md; echo "zpool rc=$?"
	for g in admins teachers students everybody outsiders; do pw groupadd $g; done
	for u in ad te t2 st ou lo; do pw useradd $u -m -s /bin/sh; done
	addg() { u=$1; shift; for g in $*; do pw groupmod $g -m $u; done; }
fi
zfs create -o aclinherit=passthrough -o aclmode=passthrough tank/s; echo "create rc=$?"
uname -a; zfs get -H -o property,value aclmode,aclinherit tank/s
addg ad admins everybody; addg te teachers everybody; addg t2 teachers everybody
addg st students everybody; addg ou everybody outsiders
as() { o=$(su -m "$1" -c "$2" 2>&1 </dev/null); r=$?; echo "  rc=$r $1: $2" | cut -c1-90; [ -z "$o" ] || echo "$o" | head -3 | sed 's/^/    out: /' | cut -c1-120; }
rights() { stat -f '  mode %Sp owner %Su %N' $1
	for v in ad te t2 st ou lo; do
		su -m $v -c "cat $1" >/dev/null 2>&1 </dev/null && r=r || r=-
		su -m $v -c "sh -c ': >> $1'" >/dev/null 2>&1 </dev/null && w=w || w=-
		printf '  %s:%s%s' $v $r $w
	done; echo; }
named() { getfacl -q $1 | grep -c 'group:\(admins\|teachers\|students\|everybody\|outsiders\)'; }
fresh() { rm -f $1; su -m te -c "umask 022; echo a > $1" </dev/null; }
echo x > /var/tmp/src; chmod 644 /var/tmp/src; touch -t 202001010000 /var/tmp/src
(cd /var/tmp; rm -f t.tar f; cp -p src f; tar cf t.tar f)
echo '##### the command exactly as written'
d=/tank/s/share; mkdir $d; chmod 755 $d
setfacl -m 'g:admins:full_set:fd:allow,g:teachers:rwxpDdaARWcs:fd:allow,g:students:rxaRcs:fd:allow,g:everybody:full_set:fd:deny' $d; echo "  setfacl rc=$?"
getfacl -q $d
f=$d/f
echo '##### chmod by the owner, each mode'
for m in 600 666 777 000 644; do fresh $f; as te "chmod $m $f"; echo "  named $(named $f)"; rights $f; done
echo '##### tools, teacher who does not own the file'
for t in "sh -c 'echo 1 >> $f'" "vim -u NONE -es -c 'normal Ax' -c wq $f" "vim -u NONE --cmd 'set backupcopy=no' -es -c 'normal Ay' -c wq $f" "vim -u NONE --cmd 'set backupcopy=yes' -es -c 'normal Ay' -c wq $f" "cp /var/tmp/src $f" "sed -i.b s/a/b/ $f" "cp -p /var/tmp/src $f" "install -m 640 /var/tmp/src $f" "tar -xpf /var/tmp/t.tar -C $d" "rsync -a /var/tmp/src $f" "sh -c 'echo new > $d/t && mv $d/t $f'"; do
	fresh $f; as t2 "$t"; echo "  content: $(tr '\n' '|' < $f | cut -c1-20) named $(named $f)"; stat -f '  mode %Sp owner %Su' $f
done
rm -f $f.b
echo '##### directory'
su -m te -c "mkdir $d/sub" </dev/null; as te "chmod 777 $d/sub"; su -m te -c "echo a > $d/sub/f; chmod 666 $d/sub/f" </dev/null; echo "  named $(named $d/sub/f)"; rights $d/sub/f
as ou "ls $d/sub"; as ou "touch $d/sub/x"; as st "ls $d/sub"; as lo "ls $d/sub"
echo '##### the file deny alone: parent lets ou through'
o=/tank/s/open; mkdir $o; chmod 755 $o; fresh $o/f
setfacl -m 'g:everybody:full_set::deny' $o/f; setfacl -m 'g:teachers:rw::allow' $o/f; getfacl -q $o/f | head -3
for m in 600 666 777; do as te "chmod $m $o/f"; rights $o/f; done
echo '##### deny for the outsiders alone'
e=/tank/s/out; mkdir $e; chmod 755 $e
setfacl -m 'g:admins:full_set:fd:allow,g:teachers:rwxpDdaARWcs:fd:allow,g:students:rxaRcs:fd:allow,g:outsiders:full_set:fd:deny' $e; getfacl -q $e | head -5
for m in 644 666 777; do fresh $e/f; as te "chmod $m $e/f"; rights $e/f; done
echo '##### a directory above alone, no deny'
g=/tank/s/gate; mkdir $g; chmod 750 $g; setfacl -m 'g:admins:rxaRcs::allow,g:teachers:rxaRcs::allow,g:students:rxaRcs::allow,g:everybody:rxaRcs::allow' $g; getfacl -q $g | head -8
h=$g/share; mkdir $h; chmod 755 $h
setfacl -m 'g:admins:full_set:fd:allow,g:teachers:rwxpDdaARWcs:fd:allow,g:students:rxaRcs:fd:allow' $h
fresh $h/f; as te "chmod 666 $h/f"; rights $h/f; as lo "ls $g"; as lo "stat -f %Sp $h/f"
echo '##### done'
