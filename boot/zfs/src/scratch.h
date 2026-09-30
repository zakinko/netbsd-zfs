/*	$NetBSD$	*/

#ifndef _LIBSA_ZFS_SCRATCH_H_
#define	_LIBSA_ZFS_SCRATCH_H_

/*
 * The deepest a read goes, in whole blocks of the largest size [S] §2.6
 * allows, plus the label's name-value pairs:
 *
 *	zap_lookup	the ZAP's first block and one leaf	2
 *	  zfs_read_object_block
 *	    dnode_read_block	one indirect block		1
 *	      zfs_read_block	the compressed block		1
 *	zfs_mount	the MOS and the mounted object set	2
 *	zfs_read_dnode	the block of the dnode array		1
 *
 * plus [S] §1.3.3's 112K of name-value pairs while a pool is opened,
 * which does not overlap the rest.
 *
 * Under zfs_read_block, a gang block holds one header per level of
 * nesting while its members are read, each 1 << ashift and so at most
 * a block; ZFS_GANG_MAXDEPTH is the number of levels.  They are given
 * back before the block is expanded, and a zstd block then takes a
 * block of literals and ZSTD_STATE_MAX of tables in the same place --
 * less than the gang headers' share, so that share covers both.
 *
 * A block on a raidz takes a sector for the padding at its end, a word
 * for each row of its map, and, if it has to be rebuilt, up to three
 * parity columns; each column is at most a block, and they can be
 * taken under every gang header at once.  A map has at most a row per
 * sector of the block: the narrowest raidz, one data column beside its
 * parity, puts one data sector in each row, and a wider one fewer rows.
 * The smallest sector is [S] §2.1's 512 bytes.
 */
#define	ZFS_MAXBLOCKSIZE	(128 * 1024)
#define	ZFS_RAIDZ_MAXROWS	(ZFS_MAXBLOCKSIZE / 512)
#define	ZFS_SCRATCH_SIZE	((11 + ZFS_GANG_MAXDEPTH) * ZFS_MAXBLOCKSIZE + \
				 ZFS_RAIDZ_MAXROWS * 4)

int	zfs_scratch_init(void *, size_t);
void	*zfs_scratch_get(size_t);
void	zfs_scratch_put(void *, size_t);
void	zfs_scratch_heap(void *(*)(size_t), void (*)(void *, size_t));

#endif	/* _LIBSA_ZFS_SCRATCH_H_ */

#ifdef ZFS_SCRATCH_DEBUG
extern size_t	zfs_scratch_high;	/* high water mark, bytes */
extern int	zfs_scratch_misput;	/* returns out of order: a bug */
extern int	zfs_scratch_exhausted;
extern int	zfs_scratch_heaped;
#endif
