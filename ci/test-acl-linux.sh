#!/bin/bash
# Scratch: aclmode=passthrough on zakinko/zfs linux-decoupled, same cases as BSD.
set -u
export TERM=vt100
uname -r
truncate -s 1G /var/tmp/q.img
zpool create -f -m /tank tank /var/tmp/q.img; echo "zpool rc=$?"
zfs create -o acltype=nfsv4 -o aclmode=passthrough -o aclinherit=passthrough -o xattr=sa tank/s; echo "create rc=$?"
zfs get -H -o property,value acltype,aclmode,aclinherit tank/s
i=2001; for g in admins teachers students everybody; do groupadd -g $i $g; i=$((i+1)); done
i=3001; for u in ad te t2 st ou lo; do useradd -m -u $i -s /bin/sh $u; i=$((i+1)); done
usermod -aG admins,everybody ad; usermod -aG teachers,everybody te; usermod -aG teachers,everybody t2
usermod -aG students,everybody st; usermod -aG everybody ou
id te; id lo
A=rwaDdxtTnNcCoy; S=raxtTnNcy
as() { o=$(su -s /bin/sh "$1" -c "$2" 2>&1 </dev/null); r=$?; echo "  rc=$r $1: $2" | cut -c1-90; [ -z "$o" ] || echo "$o" | head -4 | sed 's/^/    out: /' | cut -c1-140; }
pst() { stat -c '  mode %A owner %U %n' "$1"; }
rights() { pst "$1"
	for v in ad te t2 st ou lo; do
		su -s /bin/sh $v -c "cat $1" >/dev/null 2>&1 && r=r || r=-
		su -s /bin/sh $v -c ": >> $1" >/dev/null 2>&1 && w=w || w=-
		printf '  %s:%s%s' $v $r $w
	done; echo; }
named() { nfs4_getfacl $1 | grep -cE ':200[1-4]:'; }
share() { # dir teacher-perms
	mkdir -p $1; chown root:root $1; chmod 755 $1
	cat > /var/tmp/acl <<X
A:fdg:2001:$A
A:fdg:2002:$2
A:fdg:2003:$S
D:fdg:2004:$A
A::OWNER@:rwaDxtTnNcCoy
A:g:GROUP@:rxtncy
A::EVERYONE@:rxtncy
X
	nfs4_setfacl -S /var/tmp/acl $1; echo "  setfacl rc=$?"; nfs4_getfacl $1 | grep -v '^#' | head -8
}
fresh() { rm -f $1; su -s /bin/sh te -c "umask 022; echo a > $1"; }
try() { d=$1; u=$2; shift 2; f=$d/f; fresh $f
	as $u "$*"; echo "  content: $(tr '\n' '|' < $f 2>/dev/null | cut -c1-40)"; rights $f; [ "$(named $f)" = 4 ] || echo "  ACL CHANGED: $(named $f) named ACEs"; }
tools() { d=$1; f=$d/f; echo "##### $2"
	echo x > /var/tmp/src; chmod 644 /var/tmp/src
	(cd /var/tmp; rm -f t.tar; cp src f; tar cf t.tar f)
	fresh $f; rights $f; nfs4_getfacl $f | grep -v '^#'
	u=t2
	try $d $u "chmod 600 $f"
	try $d $u "sh -c 'echo 1 >> $f'"
	try $d $u "vim -u NONE -es -c 'normal Ax' -c wq $f"
	try $d $u "vim -u NONE --cmd 'set backupcopy=no' -es -c 'normal Ay' -c wq $f"
	try $d $u "vim -u NONE --cmd 'set backupcopy=yes' -es -c 'normal Ay' -c wq $f"
	try $d $u "sh -c 'printf \"%%s/a/b/\\nwq\\n\" | ex -s $f'"
	try $d $u "cp /var/tmp/src $f"
	try $d $u "sed -i s/a/b/ $f"
	try $d $u "cp -p /var/tmp/src $f"
	try $d $u "install -m 640 /var/tmp/src $f"
	try $d $u "tar -xpf /var/tmp/t.tar -C $d"
	try $d $u "rsync -a /var/tmp/src $f"
	try $d $u "sh -c 'echo new > $d/t && mv $d/t $f'"
	echo "--- final ACL of f"; nfs4_getfacl $f | grep -v '^#'
}
a=/tank/s/nowa; b=/tank/s/wa
share $a rwaDdxtTnNcy
share $b rwaDdxtTnNcCy
tools $a 'teachers without write_acl'
tools $b 'teachers with write_acl'
echo '--- with write_acl a teacher can also rewrite the ACL itself'
fresh $b/f; as t2 "nfs4_setfacl -a A:g:2003:rw $b/f"; rights $b/f
echo '--- so can any owner'
fresh $a/f; as te "nfs4_setfacl -a A:g:2003:rw $a/f"; rights $a/f
echo '##### user outside the all-users group'
fresh $a/l; rights $a/l; as te "chmod 666 $a/l"; rights $a/l
echo '##### directories'
su -s /bin/sh te -c "mkdir $a/sub"; as te "chmod 700 $a/sub"; su -s /bin/sh te -c "umask 022; echo a > $a/sub/f"; rights $a/sub/f
as te "chmod 777 $a/sub"; rights $a/sub/f
as ou "ls $a/sub"; as ou "touch $a/sub/x"; as lo "ls $a/sub"; as st "ls $a/sub"
echo '##### traversal'
echo pub > $a/p; chown root:root $a/p; chmod 644 $a/p; cat $a/p >/dev/null
as ou "stat -c %A $a/p"; as ou "cat $a/p"
mkdir /tank/s/pl; chmod 700 /tank/s/pl; echo pub > /tank/s/pl/p; chmod 644 /tank/s/pl/p; cat /tank/s/pl/p >/dev/null
as ou "stat -c %A /tank/s/pl/p"; as ou "cat /tank/s/pl/p"
echo '##### suiddir and chmodwrite'
useradd -m -u 3007 -s /bin/sh so
zfs set suiddir=on tank/s
g=/tank/s/gate; mkdir $g; chmod 755 $g
d=$g/share; mkdir $d; chown so:so $d; chmod 4755 $d
sed 's/A:fdg:2002:.*/A:fdg:2002:rwaDdxtTnNcy/; /^D:/d' /var/tmp/acl > /var/tmp/acl2; nfs4_setfacl -S /var/tmp/acl2 $d; echo "  setfacl rc=$?"
t2() { f=$d/f; for t in \
 "vim -u NONE --cmd 'set backupcopy=no' -es -c 'normal Ay' -c wq $f" \
 "sh -c 'echo new > $d/t && mv $d/t $f'" "rsync -a /var/tmp/src $f" "tar -xpf /var/tmp/t.tar -C $d" \
 "install -m 640 /var/tmp/src $f" "cp -p /var/tmp/src $f" "chmod 666 $f" "nfs4_setfacl -a A:g:2003:rw $f"; do
	rm -f $f; su -s /bin/sh te -c "umask 022; echo a > $f"; pst $f
	as t2 "$t"; echo "  content: $(tr '\n' '|' < $f 2>/dev/null | cut -c1-30)"; rights $f
done; }
echo '--- chmodwrite off'; t2
zfs set chmodwrite=on tank/s
echo '--- chmodwrite on'; t2
su -s /bin/sh te -c "echo a > $d/h"; a0=$(nfs4_getfacl $d/h | wc -l); as t2 "chmod 6777 $d/h"; pst $d/h; rights $d/h; echo "  acl lines $a0 -> $(nfs4_getfacl $d/h | wc -l)"
as st "chmod 666 $d/h"
echo '##### done'
