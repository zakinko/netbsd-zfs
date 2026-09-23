#!/bin/sh
# Build one of the test drivers against the reader, on a host.
#
#	./build.sh t_cat.c -o t_cat
#
# NetBSD's <sys/types.h> defines the fixed width types and a host's may
# not, hence the -include; the reader itself includes neither, because
# libsa has no <stdint.h>.
set -e
S=../src
CFLAGS="-std=c99 -Wall -Wextra -O2 -I$S -include stdint.h -include stddef.h"
cc $CFLAGS -DZFS_SUPPORT_GZIP \
	$S/zfsread.c $S/zap.c $S/zfsfs.c $S/nvlist.c $S/sha256.c \
	$S/fletcher.c $S/lz4.c $S/gzip.c $S/scratch.c "$@" -lz
