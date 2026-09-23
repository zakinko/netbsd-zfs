/*	$NetBSD$	*/

/*
 * Scratch space for the reader.
 *
 * [S] §2.6 lets a block be up to 128KB, and a lookup has several of
 * them in hand at once: the ZAP's first block, the leaf it sent us to,
 * the indirect block being descended, and the compressed block being
 * expanded.  That is most of a megabyte, and putting it in bss makes
 * every loader that links this carry it whether or not a pool is ever
 * read.
 *
 * The lifetimes are strictly nested -- each of those buffers is taken
 * before a call and dropped after it returns -- so the space is a stack
 * rather than a heap, and this is that stack.  The arena itself comes
 * from wherever the host says; libsa's alloc(), or malloc under the
 * test driver.
 *
 * Nothing here can fail at the point of use: a buffer that will not fit
 * is a bug in the size of the arena, not a condition to be handled
 * halfway down a traversal, so it is caught at the boundary and the
 * read fails there.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "zfs_ondisk.h"
#include "scratch.h"

static uint8_t *arena;
static size_t arena_size;
static size_t arena_used;
#ifdef ZFS_SCRATCH_DEBUG
size_t zfs_scratch_high;
int zfs_scratch_misput;
int zfs_scratch_exhausted;
#endif

int
zfs_scratch_init(void *buf, size_t len)
{

	if (len < ZFS_SCRATCH_SIZE)
		return (ENOMEM);
	arena = buf;
	arena_size = len;
	arena_used = 0;
	return (0);
}

void *
zfs_scratch_get(size_t len)
{
	void *p;

	len = (len + 7) & ~(size_t)7;
	if (arena == NULL || len > arena_size - arena_used) {
#ifdef ZFS_SCRATCH_DEBUG
		zfs_scratch_exhausted++;
#endif
		return (NULL);
	}
	p = arena + arena_used;
	arena_used += len;
#ifdef ZFS_SCRATCH_DEBUG
	if (arena_used > zfs_scratch_high)
		zfs_scratch_high = arena_used;
#endif
	return (p);
}

/*
 * Buffers come back in the order they were taken.  Passing the pointer
 * is not needed to free it, but it is asked for so that a mismatched
 * pair is a visible mistake rather than a silently shifted arena.
 */
void
zfs_scratch_put(void *p, size_t len)
{

	len = (len + 7) & ~(size_t)7;
	if (p != arena + arena_used - len) {
#ifdef ZFS_SCRATCH_DEBUG
		zfs_scratch_misput++;
#endif
		return;		/* out of order: leave the arena alone */
	}
	arena_used -= len;
}
