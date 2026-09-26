/*	$NetBSD$	*/

/*
 * BLAKE3, from "BLAKE3: one function, fast everywhere" (J. O'Connor,
 * J.-P. Aumasson, S. Neves, Z. Wilcox-O'Hearn, 2020), for blocks
 * written with checksum=blake3.  The section and table numbers in the
 * comments are that paper's.
 *
 * [Z] blake3_zfs.c: ZFS uses the keyed_hash mode (§2.3), keyed with the
 * pool's 32 byte checksum salt, takes the default 32 bytes of output,
 * and copies those bytes into the checksum's words as they are.
 *
 * One call over a whole block, as sha256.c does: the tree of §2.1 is
 * built by recursion over the buffer rather than with the incremental
 * chaining value stack of §5.1.2, which is only needed when the input
 * arrives in pieces.
 */

#include <sys/types.h>

#include "blake3.h"

#define	CHUNK_LEN	1024		/* §2.1 */
#define	BLOCK_LEN	64		/* §2.4 */

/* Table 3 */
#define	CHUNK_START	(1u << 0)
#define	CHUNK_END	(1u << 1)
#define	PARENT		(1u << 2)
#define	ROOT		(1u << 3)
#define	KEYED_HASH	(1u << 4)

/* Table 1 */
static const uint32_t IV[8] = {
	0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

/* Table 2: after a round, word i of the message is the old word P[i]. */
static const uint8_t P[16] = {
	2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8
};

#define	ROTR(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))

/* §2.2, the quarter-round G_i */
#define	G(a, b, c, d, x, y) do {				\
	v[a] = v[a] + v[b] + (x); v[d] = ROTR(v[d] ^ v[a], 16);	\
	v[c] = v[c] + v[d];       v[b] = ROTR(v[b] ^ v[c], 12);	\
	v[a] = v[a] + v[b] + (y); v[d] = ROTR(v[d] ^ v[a], 8);	\
	v[c] = v[c] + v[d];       v[b] = ROTR(v[b] ^ v[c], 7);	\
} while (0)

/*
 * §2.2: the compression function, truncated to the 256 bit chaining
 * value, which is all this needs.
 */
static void
compress(const uint32_t h[8], const uint32_t mw[16], uint64_t t,
    uint32_t b, uint32_t d, uint32_t out[8])
{
	uint32_t v[16], m[16], tmp[16];
	int r, i;

	for (i = 0; i < 8; i++)
		v[i] = h[i];
	for (i = 0; i < 4; i++)
		v[8 + i] = IV[i];
	v[12] = (uint32_t)t;
	v[13] = (uint32_t)(t >> 32);
	v[14] = b;
	v[15] = d;
	for (i = 0; i < 16; i++)
		m[i] = mw[i];

	for (r = 0; r < 7; r++) {
		G(0, 4, 8, 12, m[0], m[1]);
		G(1, 5, 9, 13, m[2], m[3]);
		G(2, 6, 10, 14, m[4], m[5]);
		G(3, 7, 11, 15, m[6], m[7]);
		G(0, 5, 10, 15, m[8], m[9]);
		G(1, 6, 11, 12, m[10], m[11]);
		G(2, 7, 8, 13, m[12], m[13]);
		G(3, 4, 9, 14, m[14], m[15]);
		if (r == 6)
			break;		/* "except the last one" */
		for (i = 0; i < 16; i++)
			tmp[i] = m[P[i]];
		for (i = 0; i < 16; i++)
			m[i] = tmp[i];
	}
	for (i = 0; i < 8; i++)
		out[i] = v[i] ^ v[i + 8];
}

/* §2.4: a block, parsed little endian and padded with zeros. */
static void
load_block(const uint8_t *p, size_t len, uint32_t m[16])
{
	uint8_t blk[BLOCK_LEN];
	size_t i;

	for (i = 0; i < BLOCK_LEN; i++)
		blk[i] = i < len ? p[i] : 0;
	for (i = 0; i < 16; i++)
		m[i] = (uint32_t)blk[i * 4] | ((uint32_t)blk[i * 4 + 1] << 8) |
		    ((uint32_t)blk[i * 4 + 2] << 16) |
		    ((uint32_t)blk[i * 4 + 3] << 24);
}

/* §2.4: a chunk's chaining value. */
static void
chunk_cv(const uint32_t key[8], const uint8_t *p, size_t len, uint64_t idx,
    uint32_t flags, int root, uint32_t cv[8])
{
	uint32_t h[8], m[16];
	size_t off = 0;
	int i;

	for (i = 0; i < 8; i++)
		h[i] = key[i];
	do {
		size_t n = len - off > BLOCK_LEN ? BLOCK_LEN : len - off;
		uint32_t d = flags;

		if (off == 0)
			d |= CHUNK_START;
		if (off + n == len) {
			d |= CHUNK_END;
			if (root)
				d |= ROOT;
		}
		load_block(p + off, n, m);
		compress(h, m, idx, (uint32_t)n, d, h);
		off += n;
	} while (off < len);
	for (i = 0; i < 8; i++)
		cv[i] = h[i];
}

/*
 * §2.1: more than one chunk splits into a full left subtree of the
 * largest power of two chunks that leaves something on the right, and
 * the rest; §2.5: a parent's message is its children's chaining
 * values, left then right.
 */
static void
node_cv(const uint32_t key[8], const uint8_t *p, size_t len, uint64_t idx,
    uint32_t flags, int root, uint32_t cv[8])
{
	uint32_t m[16];
	size_t left;

	if (len <= CHUNK_LEN) {
		chunk_cv(key, p, len, idx, flags, root, cv);
		return;
	}
	for (left = CHUNK_LEN; left * 2 < len; left *= 2)
		continue;
	node_cv(key, p, left, idx, flags, 0, m);
	node_cv(key, p + left, len - left, idx + left / CHUNK_LEN, flags, 0,
	    m + 8);
	compress(key, m, 0, BLOCK_LEN, flags | PARENT | (root ? ROOT : 0), cv);
}

/*
 * keyed_hash (§2.3) of a buffer with a 32 byte key, 32 bytes out.  The
 * digest is returned both as bytes and as ZFS lays it in a checksum:
 * the bytes loaded little endian (see sha512.c).
 */
void
blake3_keyed(const uint8_t k[32], const void *buf, size_t len,
    uint8_t *digest, uint64_t out[4])
{
	uint32_t key[8], cv[8];
	int i;

	/* "parsed in little-endian order from the 256-bit key" */
	for (i = 0; i < 8; i++)
		key[i] = (uint32_t)k[i * 4] | ((uint32_t)k[i * 4 + 1] << 8) |
		    ((uint32_t)k[i * 4 + 2] << 16) |
		    ((uint32_t)k[i * 4 + 3] << 24);
	node_cv(key, buf, len, 0, KEYED_HASH, 1, cv);
	for (i = 0; i < 4; i++)
		out[i] = (uint64_t)cv[i * 2] | ((uint64_t)cv[i * 2 + 1] << 32);
	if (digest != 0)
		for (i = 0; i < 32; i++)
			digest[i] = (uint8_t)(cv[i / 4] >> (8 * (i % 4)));
}

/* The hash mode, for the test vectors: the key words are the IV. */
void
blake3_hash(const void *buf, size_t len, uint8_t digest[32])
{
	uint32_t cv[8];
	int i;

	node_cv(IV, buf, len, 0, 0, 1, cv);
	for (i = 0; i < 32; i++)
		digest[i] = (uint8_t)(cv[i / 4] >> (8 * (i % 4)));
}
