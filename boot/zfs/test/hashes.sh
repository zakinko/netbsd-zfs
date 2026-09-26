#!/bin/sh
# The checksums past SHA-256 against their own documents' vectors.
#
#	sh hashes.sh
#
# Each is compared with what the algorithm's specification publishes,
# which ZFS's use of it does not change except by keying it; the keyed
# forms are tested on a pool, by openzfs.sh.
set -eu
cd "$(dirname "$0")"
F="-std=c99 -Wall -Wextra -O1 -g -I../src -include stdint.h -include stddef.h
    -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all"
trap 'rm -rf t_sha512 t_skein t_blake3 t_edonr t_edonr_sub *.dSYM' EXIT
cc $F t_sha512.c ../src/sha512.c -o t_sha512
cc $F t_skein.c ../src/skein.c -o t_skein
cc $F t_blake3.c ../src/blake3.c -o t_blake3
cc $F t_edonr.c ../src/edonr.c -o t_edonr
cc $F -DEDONR_SUBMISSION_ROTATIONS t_edonr.c ../src/edonr.c -o t_edonr_sub
./t_sha512
./t_skein
./t_blake3
./t_edonr_sub
./t_edonr
