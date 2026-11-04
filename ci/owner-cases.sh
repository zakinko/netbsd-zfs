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
