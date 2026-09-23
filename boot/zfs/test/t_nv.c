#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "zfs_ondisk.h"
#include "nvlist.h"

static uint8_t nv[VDEV_LABEL_NVLIST_SIZE];

static void
try64(const void *b, size_t l, const char *n, int nested)
{
	struct nvpair_value v;
	int e = nested ? nvlist_find_nested(b, l, n, NV_WANT_UINT64, &v)
	    : nvlist_find(b, l, n, NV_WANT_UINT64, &v);
	printf("  %-12s %s %llu\n", n, e ? "err" : "=",
	    e ? (unsigned long long)e : (unsigned long long)v.nv_u64);
}

int
main(int argc, char **argv)
{
	struct nvpair_value v, tree, ch;
	unsigned long long lba;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s image [start-lba]\n", argv[0]);
		return (2);
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror(argv[1]); return (1); }
	lba = argc > 2 ? strtoull(argv[2], NULL, 0) : 0;

	pread(fd, nv, sizeof(nv), (off_t)(lba * 512 + VDEV_LABEL_NVLIST_OFF));
	printf("encoding %u endian %u\n", nv[0], nv[1]);
	try64(nv, sizeof(nv), "version", 0);
	try64(nv, sizeof(nv), "state", 0);
	try64(nv, sizeof(nv), "pool_guid", 0);
	if (nvlist_find(nv, sizeof(nv), "name", NV_WANT_STRING, &v) == 0)
		printf("  name         = %.*s\n", (int)v.nv_strlen, v.nv_string);
	if (nvlist_find(nv, sizeof(nv), "vdev_tree", NV_WANT_NVLIST, &tree) == 0) {
		printf("  vdev_tree    = %zu bytes\n", tree.nv_listlen);
		try64(tree.nv_list, tree.nv_listlen, "ashift", 1);
		try64(tree.nv_list, tree.nv_listlen, "asize", 1);
		if (nvlist_find_nested(tree.nv_list, tree.nv_listlen, "type",
		    NV_WANT_STRING, &v) == 0)
			printf("  type         = %.*s\n", (int)v.nv_strlen,
			    v.nv_string);
		if (nvlist_find_nested(tree.nv_list, tree.nv_listlen,
		    "children", NV_WANT_NVLIST, &ch) == 0) {
			printf("  children     = %u, %zu bytes\n", ch.nv_nelem,
			    ch.nv_listlen);
			try64(ch.nv_list, ch.nv_listlen, "ashift", 1);
			if (nvlist_find_nested(ch.nv_list, ch.nv_listlen,
			    "type", NV_WANT_STRING, &v) == 0)
				printf("  child type   = %.*s\n",
				    (int)v.nv_strlen, v.nv_string);
			if (nvlist_find_nested(ch.nv_list, ch.nv_listlen,
			    "path", NV_WANT_STRING, &v) == 0)
				printf("  child path   = %.*s\n",
				    (int)v.nv_strlen, v.nv_string);
		}
	} else
		printf("  vdev_tree    not found\n");
	return 0;
}
