#!/bin/sh
# Scratch: run the subset on the stock 11.0 kernel with the port's zfs.kmod.
#   sh run-netbsd.sh <ci/zts dir> <cache dir> [runfile name]
set -u
HERE=$1; C=$2; RUN=${3:-acl-lookup}
uname -a; id -un; df -h /var/tmp | tail -1; sysctl -n hw.physmem64
tar xzpf "$C/nb-inst.tgz" -C / || exit 1
grep -q '^/lib$' /etc/ld.so.conf 2>/dev/null || echo /lib >> /etc/ld.so.conf
modload "$C/zfs.kmod"; echo "modload rc=$?"
modstat | grep -E '^(zfs|solaris) '
sysctl vfs.zfs.version.module 2>&1 | head -2
hash -r 2>/dev/null; which zfs zpool; zfs version; ldd /sbin/zfs | grep libzfs
sysctl -w ddb.onpanic=0 >/dev/null
ln -fs /usr/pkg/bin/ksh93 /bin/ksh; /bin/ksh --version 2>&1 | head -1
ln -fs /usr/pkg/bin/python3.13 /usr/pkg/bin/python3
Z=/usr/share/zfs
sh "$HERE/port-tests.sh" $Z || exit 1
cp "$HERE"/*.run $Z/runfiles/
grep -q '^zts:' /etc/passwd || useradd -m -g wheel -s /bin/sh zts
printf 'zts ALL=(ALL) NOPASSWD:SETENV: ALL\n' > /usr/pkg/etc/sudoers.d/zts
P=/usr/bin:/bin:/usr/sbin:/sbin:/usr/pkg/bin:/usr/pkg/sbin:/usr/pkg/gnu/bin
# Sections that take the machine down run last, one runfile each, so that
# what ran before them is kept.
: "${LATE:=mv_files}"
awk -v late="$LATE" 'BEGIN { RS=""; ORS="\n\n"; n=split(late, l, " ") }
	{ for (i = 1; i <= n; i++) if ($0 ~ "tests/functional/" l[i] "\\]") next; print }' \
    "$HERE/$RUN.run" > $Z/runfiles/early.run
one() {
	echo "##### runfile $1 $(date)"
	su zts -c "cd /var/tmp && PATH=$P $Z/zfs-tests.sh -v -f -x -s 4G -r $1"
	echo "##### zfs-tests.sh $1 rc=$?"
	tar czf /var/tmp/res.tgz -C /var/tmp test_results; sync
}
one early
for t in $LATE; do
	f=$(echo "$t" | tr / _)
	awk -v t="$t" 'BEGIN { RS=""; ORS="\n\n" } /^\[DEFAULT\]|^# ZFS/ || $0 ~ "tests/functional/" t "\\]"' \
	    "$HERE/$RUN.run" > "$Z/runfiles/late-$f.run"
	one "late-$f"
done
exit 0
