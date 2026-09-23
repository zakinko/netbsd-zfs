#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "zfs_ondisk.h"
#include "zfsread.h"
#include "scratch.h"

struct img { int fd; uint64_t base; };
static int
img_read(void *c, uint64_t off, void *buf, size_t len)
{
	struct img *im = c;
	ssize_t n = pread(im->fd, buf, len, (off_t)(im->base + off));
	if (n < 0) return errno;
	return (size_t)n == len ? 0 : EIO;
}
static uint8_t mosbuf[131072], blk[131072];

int
main(int argc, char **argv)
{
	(void)argc;
	struct zfs_pool pool; struct img im;
	objset_phys_t *mos = (void *)mosbuf;
	dnode_phys_t dn;

	im.fd = open(argv[1], O_RDONLY);
	im.base = 16418ULL * 512;
	{
		static uint8_t arena[ZFS_SCRATCH_SIZE];

		zfs_scratch_init(arena, sizeof(arena));
	}
	memset(&pool, 0, sizeof(pool));
	pool.pool_read = img_read; pool.pool_cookie = &im;
	pool.pool_size = 1032125ULL * 512; pool.pool_ashift = 9;
	if (zfs_uberblock_find(&pool)) return 1;
	if (zfs_read_block(&pool, &pool.pool_ub.ub_rootbp, mosbuf, sizeof(mosbuf))) return 1;
	uint64_t which = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	if (zfs_read_dnode(&pool, &mos->os_meta_dnode, which, &dn)) return 1;
	printf("dnode type %u datablk %u nlevels %u nblkptr %u maxblkid %llu\n",
	    dn.dn_type, dn.dn_datablkszsec * 512, dn.dn_nlevels, dn.dn_nblkptr,
	    (unsigned long long)dn.dn_maxblkid);
	printf("bp[0] embedded %d comp %u cksum %u lsize %llu psize %llu\n",
	    BP_IS_EMBEDDED(&dn.dn_blkptr[0]), BP_GET_COMPRESS(&dn.dn_blkptr[0]),
	    BP_GET_CHECKSUM(&dn.dn_blkptr[0]),
	    (unsigned long long)BP_GET_LSIZE(&dn.dn_blkptr[0]),
	    (unsigned long long)BP_GET_PSIZE(&dn.dn_blkptr[0]));
	{
		int e = zfs_read_object_block(&pool, &dn, 0, blk, sizeof(blk));
		if (e) { printf("read block 0: %s\n", strerror(e)); return 1; }
	}
	uint64_t bt = *(uint64_t *)(void *)blk;
	printf("block type %016llx (micro %016llx header %016llx)\n",
	    (unsigned long long)bt, (unsigned long long)ZBT_MICRO,
	    (unsigned long long)ZBT_HEADER);
	if (bt == ZBT_MICRO) {
		mzap_phys_t *mz = (void *)blk;
		size_t blksize = dn.dn_datablkszsec * 512;
		size_t n = (blksize - 128) / 64 + 1;
		printf("salt %llx, %zu slots\n", (unsigned long long)mz->mz_salt, n);
		for (size_t i = 0; i < n; i++)
			if (mz->mz_chunk[i].mze_name[0])
				printf("  %-24s %llu\n", mz->mz_chunk[i].mze_name,
				    (unsigned long long)mz->mz_chunk[i].mze_value);
	} else {
		zap_phys_t *zp = (void *)blk;
		printf("fatzap magic %llx zt_shift %llu zt_blk %llu numblks %llu nentries %llu\n",
		    (unsigned long long)zp->zap_magic,
		    (unsigned long long)zp->zap_ptrtbl.zt_shift,
		    (unsigned long long)zp->zap_ptrtbl.zt_blk,
		    (unsigned long long)zp->zap_ptrtbl.zt_numblks,
		    (unsigned long long)zp->zap_num_entries);
	}
	return 0;
}
