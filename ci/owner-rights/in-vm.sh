#!/bin/sh
# Run inside the chroot.  chroot -u gives a process another uid without PAM.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
O=/var/tmp/want.out
ok()   { echo "PASS $*"; }
bad()  { echo "FAIL $*"; }
# want 0|x name command...: x means any nonzero status
want() { w=$1; n=$2; shift 2; "$@" >$O 2>&1; r=$?
	if { [ $w = 0 ] && [ $r = 0 ]; } || { [ $w = x ] && [ $r != 0 ]; }; then
		ok "$n (rc=$r)"
	else
		bad "$n (rc=$r want $w)"; sed 's/^/  | /' $O
	fi; }
has()  { n=$1; pat=$2; shift 2; if "$@" 2>&1 | grep -q -- "$pat"; then ok "$n"; else bad "$n: no $pat"; "$@" 2>&1 | sed 's/^/  | /'; fi; }
hasnt(){ n=$1; pat=$2; shift 2; if "$@" 2>&1 | grep -q -- "$pat"; then bad "$n: found $pat"; "$@" 2>&1 | sed 's/^/  | /'; else ok "$n"; fi; }
A()    { chroot -u alice / "$@"; }
B()    { chroot -u bob / "$@"; }
uname -a; uname -i
id alice >/dev/null 2>&1 || pw useradd alice -u 2001
id bob >/dev/null 2>&1 || pw useradd bob -u 2002

truncate -s 512m /var/tmp/p0.img
MD=$(mdconfig -a -t vnode -f /var/tmp/p0.img)
zpool create -f -m /t t /dev/$MD
chmod 777 /t; cd /t
has "feature enabled before use" enabled zpool get -H feature@acl_owner_rights t

# The ACL under test: owner@ without write_acl, nobody else with read_acl.
restrict() {
	A setfacl -m owner@:rw-p--aARW----:-------:allow,group@:--------------:-------:allow,everyone@:--------------:-------:allow $1
}

echo "--- no OWNER_RIGHTS@: same as before"
A touch /t/plain
restrict /t/plain
want 0 "owner keeps write_acl without an ACE" A setfacl -a0 u:bob:r::allow /t/plain
want 0 "owner keeps read_acl without an ACE" A getfacl /t/plain

echo "--- owner loses write_acl and read_acl"
A touch /t/f
restrict /t/f
want 0 "owner may still add OWNER_RIGHTS@ first" A setfacl -a0 owner_rights@:rw-p--aARW----:-------:allow /t/f
has "getfacl text" 'owner_rights@:rw-p--aARW----:-------:allow' getfacl -q /t/f
has "active after use" active zpool get -H feature@acl_owner_rights t
want x "owner refused setfacl" A setfacl -a0 u:bob:r::allow /t/f
want x "owner refused getfacl" A getfacl /t/f
want 0 "owner keeps stat" A stat /t/f
want 0 "owner keeps data" A sh -c 'echo x >/t/f; cat /t/f'
want 0 "owner keeps utimes" A touch -t 202001010000 /t/f
want x "bob, no ACE, refused setfacl" B setfacl -a0 u:bob:r::allow /t/f

echo "--- root"
want 0 "root reads" getfacl /t/f
want 0 "root writes" setfacl -a0 u:bob:r::allow /t/f
setfacl -x0 /t/f
touch /t/rootf; setfacl -a0 owner_rights@::allow /t/rootf
setfacl -m owner@:rw-p--aARW----:-------:allow /t/rootf
want 0 "root as the owner still writes" setfacl -a0 u:bob:r::allow /t/rootf

echo "--- explicit allow brings it back"
setfacl -a0 owner_rights@:write_acl::allow /t/f
want 0 "OWNER_RIGHTS@ allow write_acl" A setfacl -a1 u:bob:r::allow /t/f
setfacl -x1 /t/f; setfacl -x0 /t/f
setfacl -a0 owner_rights@:read_acl::allow /t/f
want 0 "OWNER_RIGHTS@ allow read_acl" A getfacl /t/f
setfacl -x0 /t/f
setfacl -m owner@:rw-p--aARWcC--:-------:allow /t/f
want 0 "OWNER@ allow is explicit too" A setfacl -a1 u:bob:r::allow /t/f
setfacl -x1 /t/f
setfacl -a0 owner_rights@:write_acl::deny /t/f
want x "OWNER_RIGHTS@ deny beats the later allow" A setfacl -a1 u:bob:r::allow /t/f
setfacl -x0 /t/f
setfacl -m owner@:rw-p--aARW----:-------:allow /t/f
setfacl -a0 u:alice:write_acl::allow /t/f
want 0 "a named allow is explicit too" A setfacl -a1 u:bob:r::allow /t/f

echo "--- chmod"
for m in discard groupmask passthrough; do
	zfs set aclmode=$m t
	f=/t/c.$m
	A touch $f; restrict $f
	setfacl -a0 owner_rights@:rwxp--aARW----:-------:allow $f
	setfacl -a0 u:bob:r::allow $f
	want 0 "$m: owner still chmods" A chmod 640 $f
	[ "$(stat -f %Lp $f)" = 640 ] && ok "$m: mode is what was set" || bad "$m: mode $(stat -f %Lp $f)"
	has "$m: ACE stays" owner_rights@ getfacl -q $f
	hasnt "$m: ACE gave up the mode bits" 'owner_rights@:rwx' getfacl -q $f
	want x "$m: chmod did not bring write_acl back" A setfacl -a0 u:bob:w::deny $f
	want 0 "$m: chmod 000 by the owner" A chmod 000 $f
	[ "$(stat -f %Lp $f)" = 0 ] && ok "$m: mode 000" || bad "$m: mode $(stat -f %Lp $f)"
	want x "$m: owner cannot read data at 000" A cat $f
	getfacl -q $f | sed 's/^/  : /'
done
zfs set aclmode=restricted t
f=/t/c.restricted
A touch $f; setfacl -a0 owner_rights@::allow $f
A chmod 640 $f; echo "restricted: chmod rc=$? (recorded, not judged)"
zfs set aclmode=discard t

echo "--- inheritance"
zfs set aclinherit=passthrough t
mkdir /t/d; chown alice /t/d; chmod 777 /t/d
setfacl -a0 owner_rights@:read_acl:fd:allow /t/d
A touch /t/d/child
has "inherited" 'owner_rights@' getfacl -q /t/d/child
[ "$(stat -f %Lp /t/d/child)" != 0 ] && ok "create mode applied: $(stat -f %Lp /t/d/child)" || bad "child mode 000"
want x "child's owner refused" A setfacl -a0 u:bob:r::allow /t/d/child
mkdir /t/d2; chown alice /t/d2; setfacl -a0 owner_rights@::fdi:allow /t/d2
want 0 "inherit-only does not bind the directory" A setfacl -a0 u:bob:r::allow /t/d2
zfs set aclinherit=restricted t

echo "--- persistence and send"
A touch /t/kept; restrict /t/kept; setfacl -a0 owner_rights@::allow /t/kept
sync; cd /; zpool export t; zpool import -d /dev/$MD t; cd /t
has "survives export" owner_rights@ getfacl -q /t/kept
want x "still enforced after import" A setfacl -a0 u:bob:r::allow /t/kept

truncate -s 256m /var/tmp/p1.img /var/tmp/p2.img
M1=$(mdconfig -a -t vnode -f /var/tmp/p1.img); M2=$(mdconfig -a -t vnode -f /var/tmp/p2.img)
zpool create -f -m /r1 r1 /dev/$M1
zpool create -f -d -m /r2 -o feature@extensible_dataset=enabled r2 /dev/$M2
zfs snapshot t@s
want 0 "send to a pool with the feature" sh -c 'zfs send t@s | zfs recv r1/c'
has "received active" active zpool get -H feature@acl_owner_rights r1
has "received ACE" owner_rights@ getfacl -q /r1/c/kept
want x "send to a pool without it refused" sh -c 'zfs send t@s | zfs recv r2/c'
touch /r2/f
want x "pool without the feature refuses the ACE" setfacl -a0 owner_rights@::allow /r2/f
has "..with ENOTSUP" 'not supported' setfacl -a0 owner_rights@::allow /r2/f
zpool destroy r1; zpool destroy r2; mdconfig -d -u $M1; mdconfig -d -u $M2

echo "--- UFS has no owner rights to cancel"
U=$(mdconfig -a -t swap -s 32m); newfs /dev/$U >/dev/null; mkdir -p /u; mount -o nfsv4acls /dev/$U /u
touch /u/f
want x "UFS refuses the tag" setfacl -a0 owner_rights@::allow /u/f
want 0 "UFS unchanged otherwise" setfacl -a0 u:bob:r::allow /u/f
umount /u; mdconfig -d -u $U

echo "--- text"
want x "qualifier refused" setfacl -a0 owner_rights@:bob:r::allow /t/plain
want 0 "two ACEs round trip" sh -c 'getfacl -q /t/kept | tail -n +1 >/var/tmp/k.acl; touch /t/rt; setfacl -M /var/tmp/k.acl /t/rt'
has "round trip kept the ACE" owner_rights@ getfacl -q /t/rt

echo "--- NFSv4 with the FreeBSD client"
sysctl vfs.nfsd.enable_stringtouid=1 vfs.nfsd.enable_uidtostring=1 vfs.nfs.enable_uidtostring=1 >/dev/null
cat >/etc/exports <<X
V4: /t -sec=sys
/t -alldirs -maproot=root
X
ifconfig lo0 up 127.0.0.1
mkdir -p /var/run /var/db
rpcbind; nfsuserd -domain or.test; mountd -r /etc/exports; nfsd -t -u -n 4
sleep 3
mkdir -p /n
tcpdump -i lo0 -s0 -w /var/tmp/nfs.pcap port 2049 >/dev/null 2>&1 &
TD=$!
sleep 2
mount -t nfs -o nfsv4,minorversion=2 127.0.0.1:/ /n; echo "mount rc=$?"
A touch /n/nf; restrict /n/nf
want 0 "client: owner sets OWNER_RIGHTS@" A setfacl -a0 owner_rights@:rw-p--aARW----:-------:allow /n/nf
has "client: root reads it back" owner_rights@ getfacl -q /n/nf
has "server sees it" owner_rights@ getfacl -q /t/nf
want x "client: owner refused setfacl" A setfacl -a0 u:bob:r::allow /n/nf
want x "client: owner refused getfacl" A getfacl /n/nf
want 0 "client: root can" setfacl -a1 u:bob:r::allow /n/nf
A chmod 640 /n/nf; echo "client chmod rc=$? (recorded)"
sleep 2; kill $TD; sleep 1
[ "$(strings -a /var/tmp/nfs.pcap | grep -c 'OWNER_RIGHTS@')" -gt 0 ] && ok "OWNER_RIGHTS@ on the wire" || bad "not on the wire"
umount /n; killall nfsd mountd nfsuserd rpcbind 2>/dev/null

cd /
zpool export t
mdconfig -d -u $MD
truncate -s 128m /var/tmp/old.img; P=$(mdconfig -a -t vnode -f /var/tmp/old.img)
zpool create -f -m /old old /dev/$P
touch /old/kept; setfacl -a0 owner_rights@::allow /old/kept; sync
zpool export old; mdconfig -d -u $P
zstd -q -f -o /var/tmp/pool-old.img.zst /var/tmp/old.img; echo "zstd rc=$?"
echo DONE
