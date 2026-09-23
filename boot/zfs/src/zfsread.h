/*	$NetBSD$	*/

/*
 * A read-only ZFS reader, written from the ZFS On-Disk Specification.
 * See zfs_ondisk.h for what the citations in the source mean.
 */

#ifndef _LIBSA_ZFSREAD_H_
#define	_LIBSA_ZFSREAD_H_

/*
 * The one thing the reader needs from its host: bytes off the vdev,
 * counted from the start of the partition holding the pool.  libsa
 * supplies this with DEV_STRATEGY; the test driver supplies it with
 * pread.  Returns zero, or an errno.
 */
typedef int (*zfs_readfn_t)(void *, uint64_t, void *, size_t);

struct zfs_pool {
	zfs_readfn_t	pool_read;
	void		*pool_cookie;
	uint64_t	pool_size;	/* bytes, for the trailing labels */
	uint64_t	pool_guid;
	uint32_t	pool_ashift;
	struct uberblock pool_ub;	/* the active one */
	int		pool_label;	/* which label it came from */
};

int	zfs_pool_open(struct zfs_pool *, char *, size_t);
int	zfs_uberblock_find(struct zfs_pool *);
int	zfs_read_block(struct zfs_pool *, const blkptr_t *, void *, size_t);
int	zfs_read_dnode(struct zfs_pool *, const dnode_phys_t *, uint64_t,
	    dnode_phys_t *);
int	zfs_read_object_block(struct zfs_pool *, const dnode_phys_t *,
	    uint64_t, void *, size_t);

struct zfs_dataset {
	struct zfs_pool	*ds_pool;
	dnode_phys_t	ds_meta;	/* the object set's metadnode */
	uint64_t	ds_root;	/* [S] §6.1's ROOT */
	uint64_t	ds_version;	/* [S] §6.1's VERSION */
};

int	zfs_mount(struct zfs_pool *, const char *, struct zfs_dataset *);
int	zfs_lookup(struct zfs_dataset *, const char *, dnode_phys_t *);
/*
 * A file's last decompressed block.
 *
 * The loader reads a kernel in pieces far smaller than a ZFS block, and
 * without this each piece re-reads and re-expands the whole 128KB block
 * it falls in -- thirty times over for a 4KB read.  The caller owns the
 * buffer so that its lifetime is the open file's, which the reader's
 * own scratch arena, being a stack, cannot give it.
 */
struct zfs_blkcache {
	uint8_t		*bc_buf;	/* ZFS_MAXBLOCKSIZE, or NULL */
	uint64_t	bc_blkid;
	int		bc_valid;
};

int	zfs_read_file(struct zfs_dataset *, const dnode_phys_t *, uint64_t,
	    void *, size_t, size_t *, struct zfs_blkcache *);
uint64_t zfs_mode(const dnode_phys_t *);
uint64_t zfs_size(const dnode_phys_t *);

int	zap_lookup(struct zfs_pool *, const dnode_phys_t *, const char *,
	    uint64_t *);

int	zap_list(struct zfs_pool *, const dnode_phys_t *,
	    void (*)(void *, const char *, uint64_t), void *);

#endif	/* _LIBSA_ZFSREAD_H_ */
