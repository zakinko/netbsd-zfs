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
 */
#define	ZFS_MAXBLOCKSIZE	(128 * 1024)
#define	ZFS_SCRATCH_SIZE	(7 * ZFS_MAXBLOCKSIZE)

int	zfs_scratch_init(void *, size_t);
void	*zfs_scratch_get(size_t);
void	zfs_scratch_put(void *, size_t);

#endif	/* _LIBSA_ZFS_SCRATCH_H_ */

#ifdef ZFS_SCRATCH_DEBUG
extern size_t	zfs_scratch_high;	/* high water mark, bytes */
extern int	zfs_scratch_misput;	/* returns out of order: a bug */
extern int	zfs_scratch_exhausted;
#endif
