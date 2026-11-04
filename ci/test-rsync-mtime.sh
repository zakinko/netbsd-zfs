#!/bin/sh
# Scratch: does rsync -a skip a file only because size and mtime agree?
d=/var/tmp/rs; mkdir -p $d; cd $d
echo x > src; echo a > f; touch -r src f
rsync -a src f; echo "same size, same mtime: $(cat f)"
echo a > f; touch -t 202001010000 f
rsync -a src f; echo "same size, other mtime: $(cat f)"
