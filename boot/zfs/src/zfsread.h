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

/*
 * The other devices a pool may span.  The host is asked for device n,
 * counting from zero, and answers with a way to read it and its size
 * in bytes, or with ENOENT once there are no more.  Every device it can
 * reach is offered, including the one the pool was found on; the
 * reader reads each one's label and keeps those that belong.
 */
typedef int (*zfs_probefn_t)(void *, int, zfs_readfn_t *, void **,
	    uint64_t *);
/* A device offered that is not part of the pool, handed back. */
typedef void (*zfs_releasefn_t)(void *, void *);
#define	ZFS_MAX_PROBE		1024

/*
 * [S] §1.1: the vdevs form a tree, and the pool's top-level vdevs are
 * the ones a DVA names by number (§2.1).  Each is kept here with the
 * leaves below it.  How many of each is the reader's choice, not the
 * format's: a pool wider than this is refused when it is opened.
 */
#define	ZFS_MAX_TOPS		8
#define	ZFS_MAX_CHILDREN	16

#define	ZFS_VT_NONE	0		/* not seen on any device */
#define	ZFS_VT_LEAF	1		/* disk or file */
#define	ZFS_VT_MIRROR	2		/* mirror, replacing or spare */
#define	ZFS_VT_RAIDZ	3

struct zfs_leaf {
	uint64_t	lf_guid;
	zfs_readfn_t	lf_read;	/* NULL: not found */
	void		*lf_cookie;
	uint64_t	lf_size;	/* bytes, for the trailing labels */
};

struct zfs_top {
	int		tv_type;
	uint32_t	tv_ashift;
	uint32_t	tv_nparity;	/* raidz only */
	uint32_t	tv_nchildren;
	struct zfs_leaf	tv_child[ZFS_MAX_CHILDREN];
};

struct zfs_pool {
	/* The device the pool was found on; set by the caller. */
	zfs_readfn_t	pool_read;
	void		*pool_cookie;
	uint64_t	pool_size;	/* bytes, for the trailing labels */
	/* The rest of them, or NULL for a pool on that one device. */
	zfs_probefn_t	pool_probe;
	zfs_releasefn_t	pool_release;
	void		*pool_probe_cookie;

	uint64_t	pool_guid;
	uint32_t	pool_ashift;	/* of the found device's top vdev */
	uint32_t	pool_ntops;
	struct zfs_top	pool_top[ZFS_MAX_TOPS];
	struct uberblock pool_ub;	/* the active one */
	/*
	 * [Z] the salt skein, edonr and blake3 are keyed with, from the
	 * MOS; read when the pool is mounted.
	 */
	uint8_t		pool_salt[32];
	int		pool_salt_ok;
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

int	zap_lookup_bytes(struct zfs_pool *, const dnode_phys_t *,
	    const char *, void *, size_t);

int	zap_list(struct zfs_pool *, const dnode_phys_t *,
	    void (*)(void *, const char *, uint64_t), void *);

#endif	/* _LIBSA_ZFSREAD_H_ */
