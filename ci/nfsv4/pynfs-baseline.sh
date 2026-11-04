#!/bin/sh
#
# Export a directory from the Linux knfsd on this machine and run the pynfs
# server tests for one minor version against it.  Runs as root.
#
#   pynfs-baseline.sh 4.0|4.1 OUTDIR
#
# pynfs's failures are the measurement, so its exit status does not fail the
# job.  The rig failing does: knfsd not offering the version, a mount that
# does not work, or no JSON at the end.

set -eu

MINOR=$1
OUT=$2
PYNFS_URL=git://git.linux-nfs.org/projects/cdmackay/pynfs.git
PYNFS_REV=cd4701827a8261fedbfb4c6e39029fb9671321a6
EXPORT=/export/pynfs
LIMIT=${LIMIT:-6000}

mkdir -p "$OUT"
chmod 777 "$OUT"

apt-get update -q
apt-get install -y -q nfs-kernel-server nfs-common libkrb5-dev python3-dev \
    swig python3-gssapi python3-ply python3-setuptools >/dev/null

git clone -q "$PYNFS_URL" /opt/pynfs
git -C /opt/pynfs checkout -q "$PYNFS_REV"
(cd /opt/pynfs && python3 setup.py build) >"$OUT/pynfs-build.log" 2>&1

mkdir -p "$EXPORT"
chmod 1777 "$EXPORT"
# insecure: pynfs connects from a high port (pynfs README).
echo "$EXPORT *(rw,sync,insecure,no_root_squash,no_subtree_check)" >/etc/exports
systemctl restart nfs-server
exportfs -ra

{
	echo "date            $(date -u +%FT%TZ)"
	echo "kernel          $(uname -a)"
	echo "os              $(. /etc/os-release; echo "$PRETTY_NAME")"
	echo "nfs-utils       $(dpkg-query -W -f '${Version}' nfs-kernel-server)"
	echo "pynfs           $PYNFS_REV"
	echo "python          $(python3 --version)"
	echo "versions        $(cat /proc/fs/nfsd/versions)"
	echo "lease           $(cat /proc/fs/nfsd/nfsv4leasetime)"
	echo "grace           $(cat /proc/fs/nfsd/nfsv4gracetime)"
	echo "threads         $(cat /proc/fs/nfsd/threads)"
	echo "export fs       $(stat -f -c %T "$EXPORT") on $(df --output=source "$EXPORT" | tail -1)"
	echo "exports         $(exportfs -v | tr -s ' \t' ' ')"
	echo "suite           $MINOR, arguments: --maketree --rundeps all"
} | tee "$OUT/environment-$MINOR.txt"

# Prove the server speaks this version before blaming anything on what pynfs
# reports.  Ask a mount, not /proc/fs/nfsd/versions, which listed "+3 +4 +4.1
# +4.2" and no "+4.0" on the kernel of this runner.
mkdir -p /mnt/probe
mount -t nfs -o vers="$MINOR" 127.0.0.1:"$EXPORT" /mnt/probe
echo probe >/mnt/probe/probe
test "$(cat "$EXPORT/probe")" = probe
rm /mnt/probe/probe
grep ' /mnt/probe ' /proc/mounts | tee -a "$OUT/environment-$MINOR.txt"
grep ' /mnt/probe ' /proc/mounts | grep -q "vers=$MINOR"
umount /mnt/probe

# Started straight after this point, RPLY5 to RPLY8 got NFS4ERR_GRACE from LOCK
# although the kernel logged that it skipped the NFSv4 grace period.  Wait a
# whole grace period, so that those tests measure replay and not the startup.
sleep $(($(cat /proc/fs/nfsd/nfsv4gracetime) + 10))

case $MINOR in
4.0) DIR=nfs4.0 ;;
4.1) DIR=nfs4.1 ;;
esac

cd "/opt/pynfs/$DIR"
rc=0
timeout "$LIMIT" python3 ./testserver.py 127.0.0.1:"$EXPORT" --maketree \
    --rundeps --json="$OUT/pynfs-$MINOR.json" all \
    >"$OUT/pynfs-$MINOR.log" 2>&1 || rc=$?
echo "testserver.py exit status $rc" | tee -a "$OUT/environment-$MINOR.txt"
dmesg >"$OUT/dmesg-$MINOR.txt" 2>&1 || :
python3 ./testserver.py --showcodesflags >"$OUT/codes-$MINOR.txt" 2>&1 || :
chmod -R a+rX "$OUT"
test -s "$OUT/pynfs-$MINOR.json"
