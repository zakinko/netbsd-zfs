#!/bin/sh
set -u
uname -a; kldstat | grep -i zfs; strings /boot/kernel/zfs.ko | grep -c suiddir
export PATH=/usr/local/sbin:/usr/local/bin:$PATH LD_LIBRARY_PATH=/usr/local/lib
cd /var/tmp; hash -r 2>/dev/null; which zfs zpool; zfs version
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
echo x > /var/tmp/src; chmod 644 /var/tmp/src; (cd /var/tmp; rm -f t.tar; cp src f; tar cf t.tar f)
echo '##### aclownerdeny'
d=/tank/s/od; mkdir $d; chown root:wheel $d; chmod 755 $d
setfacl -m "$base,g:teachers:rwxpDdaARWcs:fd:allow,g:students:rxaRcs:fd:allow,$deny" $d
for v in off on; do echo "--- aclownerdeny=$v"; zfs set aclownerdeny=$v tank/s; zfs get -H -o value aclownerdeny tank/s
 f=$d/f; rm -f $f; su -m te -c "umask 022; echo a > $f"
 stat -f '  mode %Sp owner %Su' $f
 as te "setfacl -m g:students:rw::allow $f"; rights $f
 rm -f $f; su -m te -c "umask 022; echo a > $f"
 as te "chmod 600 $f"; stat -f '  mode %Sp owner %Su' $f; echo "  named $(named $f)"; rights $f
 as te "chmod 644 $f"
 for t in "rsync -a /var/tmp/src $f" "tar -xpf /var/tmp/t.tar -C $d" "install -m 640 /var/tmp/src $f" "cp -p /var/tmp/src $f" "sh -c 'echo new > $d/t && mv $d/t $f'"; do
  rm -f $f; su -m te -c "umask 022; echo a > $f"; as t2 "$t"; stat -f '  mode %Sp owner %Su' $f; echo "  content: $(tr '\n' '|' < $f | cut -c1-20) named $(named $f)"
 done
 echo '  -- renamed-in owner t2 tries the ACL'
 rm -f $f; su -m te -c "umask 022; echo a > $f"; as t2 "sh -c 'echo n > $d/t && mv $d/t $f'"; stat -f '  mode %Sp owner %Su' $f; as t2 "setfacl -m g:students:rw::allow $f"; rights $f
 as ad "setfacl -m g:students:r::allow $f"; setfacl -m g:students:r::allow $f; echo "  root rc=$?"
done
echo '--- deny on everyone@ does not survive chmod (expected bypass)'
zfs set aclownerdeny=on tank/s
b2=/tank/s/ev; mkdir $b2; chmod 777 $b2; rm -f $b2/f; su -m te -c "umask 022; echo a > $b2/f"
setfacl -m "everyone@:C::deny" $b2/f; getfacl -q $b2/f | head -3
as te "setfacl -m g:students:rw::allow $b2/f"; as te "chmod 644 $b2/f"; as te "setfacl -m g:students:rw::allow $b2/f"; getfacl -q $b2/f | head -3
zfs set aclownerdeny=off tank/s
echo '##### done'
