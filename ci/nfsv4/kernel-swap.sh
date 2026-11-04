#!/bin/sh
#
# Replace /netbsd in a running NetBSD VM, reboot it and prove the new kernel
# is the one running: the reboot by kern.boottime, the kernel by uname -v and
# by the digest of what landed on disk.  Then load zfs from the release's own
# modules, which nfsserver cannot show since GENERIC has that built in, and
# start nfsd.  The NFS work relies on both.
#
#   kernel-swap.sh KERNEL MARK LOG ssh-command...

set -eu

K=$1
MARK=$2
LOG=$3
shift 3
TMO=${TMO:-300}

exec >"$LOG.tmp" 2>&1
trap 'cat "$LOG.tmp" >>"$LOG"; rm -f "$LOG.tmp"; cat "$LOG"' EXIT
set -x

"$@" 'uname -a; sysctl -n kern.boottime; sysctl -n kern.version; cksum -a sha256 /netbsd; cat /boot.cfg; df -h /'
old=$("$@" 'sysctl -n kern.boottime')
"$@" 'cat >/netbsd.new' <"$K"
"$@" 'cp -p /netbsd /netbsd.old && mv /netbsd.new /netbsd && sync'
"$@" '(sleep 2; shutdown -r now) >/dev/null 2>&1 &' || :

t0=$(date +%s)
new=
while [ $(($(date +%s) - t0)) -lt "$TMO" ]; do
	sleep 5
	new=$("$@" 'sysctl -n kern.boottime' 2>/dev/null) || new=
	if [ -n "$new" ] && [ "$new" != "$old" ]; then
		break
	fi
	new=
done
echo "back after $(($(date +%s) - t0))s, boottime [$old] -> [$new]"
[ -n "$new" ]

v=$("$@" 'uname -v')
echo "uname -v: $v"
case $v in
*"($MARK)"*) ;;
*) echo "running kernel is not $MARK"; exit 1 ;;
esac
want=$(sha256sum "$K" | cut -d' ' -f1)
"$@" "cksum -a sha256 /netbsd" | grep -q "$want"

"$@" 'uname -a; dmesg | head -5; ifconfig -a | grep inet; modstat | head -5'
"$@" 'modstat | grep -i nfs; modload zfs && modstat | grep -E "^(zfs|solaris) "'
"$@" 'echo "/var/tmp -noresvport -network 0.0.0.0 -mask 0.0.0.0" >/etc/exports
      /etc/rc.d/rpcbind onestart; /etc/rc.d/mountd onestart; /usr/sbin/nfsd -t -u -n 4; sleep 2
      sockstat -l 2>/dev/null | grep -E ":2049|nfsd" || netstat -an | grep 2049
      rpcinfo -p | grep nfs' || echo "nfsd did not start"
echo "SWAP OK"
