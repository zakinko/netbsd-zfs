#!/bin/sh
# The label parser against labels ZFS would not write.
#
#	sh nvlist.sh
set -eu
cd "$(dirname "$0")"
F="-std=c99 -Wall -Wextra -O1 -g -I../src -include stdint.h -include stddef.h
    -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all"
trap 'rm -rf t_nvbad *.dSYM' EXIT
cc $F t_nvbad.c ../src/nvlist.c -o t_nvbad
./t_nvbad
