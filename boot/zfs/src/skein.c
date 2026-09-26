/*	$NetBSD$	*/

/*
 * Skein-512, from "The Skein Hash Function Family", version 1.3
 * (Ferguson, Lucks, Schneier, Whiting, Bellare, Kohno, Callas, Walker,
 * 1 October 2010), for blocks written with checksum=skein.  The
 * section and table numbers in the comments are that paper's.
 *
 * [Z] skein_zfs.c: ZFS computes Skein-512 with 256 bits of output and
 * the pool's 32 byte checksum salt as the key -- §3.5.5 with Nk = 32 --
 * and copies the output's bytes into the checksum's words as they are.
 */

#include <sys/types.h>

#include "skein.h"

#define	NB	64		/* state size in bytes: Skein-512 */
#define	NW	8		/* in words, Nw of §3.3 */
#define	NR	72		/* Table 2 */

/* §3.3.2 */
#define	C240	0x1BD11BDAA9FC1A22ULL

/* Table 6 */
#define	T_KEY	0
#define	T_CFG	4
#define	T_MSG	48
#define	T_OUT	63

/* Table 3, Nw = 8: word i of the next round is f_{d, PI[i]}. */
static const uint8_t PI[NW] = { 2, 1, 4, 7, 6, 5, 0, 3 };

/* Table 4, Nw = 8: R_{d mod 8, j} */
static const uint8_t R[8][4] = {
	{ 46, 36, 19, 37 },
	{ 33, 27, 14, 42 },
	{ 17, 49, 36, 39 },
	{ 44,  9, 54, 56 },
	{ 39, 30, 34, 24 },
	{ 13, 50, 10, 17 },
	{ 25, 29, 39, 43 },
	{  8, 35, 56, 22 }
};

#define	ROTL(x, n)	(((x) << (n)) | ((x) >> (64 - (n))))

/* §3.2: least significant byte first throughout. */
static uint64_t
le64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return (v);
}

/*
 * §3.3: Threefish-512 encryption of one block, key and plaintext as
 * words, tweak as its two words.
 */
static void
threefish(const uint64_t key[NW], const uint64_t tw[2], const uint64_t p[NW],
    uint64_t c[NW])
{
	uint64_t k[NW + 1], t[3], v[NW], e[NW];
	int d, i, j, s;

	/* §3.3.2 */
	k[NW] = C240;
	for (i = 0; i < NW; i++) {
		k[i] = key[i];
		k[NW] ^= key[i];
	}
	t[0] = tw[0];
	t[1] = tw[1];
	t[2] = tw[0] ^ tw[1];

	for (i = 0; i < NW; i++)
		v[i] = p[i];
	for (d = 0; d < NR; d++) {
		for (i = 0; i < NW; i++)
			e[i] = v[i];
		if (d % 4 == 0) {
			/* subkey s = d / 4 */
			s = d / 4;
			for (i = 0; i < NW; i++)
				e[i] += k[(s + i) % (NW + 1)];
			e[NW - 3] += t[s % 3];
			e[NW - 2] += t[(s + 1) % 3];
			e[NW - 1] += (uint64_t)s;
		}
		/* §3.3.1: MIX, then the permutation. */
		for (j = 0; j < NW / 2; j++) {
			uint64_t x0 = e[2 * j], x1 = e[2 * j + 1];

			e[2 * j] = x0 + x1;
			e[2 * j + 1] = ROTL(x1, R[d % 8][j]) ^ e[2 * j];
		}
		for (i = 0; i < NW; i++)
			v[i] = e[PI[i]];
	}
	s = NR / 4;
	for (i = 0; i < NW; i++)
		c[i] = v[i] + k[(s + i) % (NW + 1)];
	c[NW - 3] += t[s % 3];
	c[NW - 2] += t[(s + 1) % 3];
	c[NW - 1] += (uint64_t)s;
}

/*
 * §3.4: UBI(G, M, Ts), for byte strings (B = 0) and a Ts carrying only
 * a type, whose Position field starts at zero.
 */
static void
ubi(uint64_t g[NW], const uint8_t *m, size_t len, unsigned type)
{
	uint8_t blk[NB];
	uint64_t p[NW], tw[2], c[NW];
	size_t off = 0, n;
	int i, first = 1;

	do {
		n = len - off > NB ? NB : len - off;
		for (i = 0; i < NB; i++)
			blk[i] = (size_t)i < n ? m[off + i] : 0;
		for (i = 0; i < NW; i++)
			p[i] = le64(blk + i * 8);
		off += n;
		/*
		 * Tweak: Position (bits 0-95) is the bytes processed so
		 * far including this block; Type at 120; First at 126 in
		 * the first block; Final at 127 in the last.
		 */
		tw[0] = (uint64_t)off;
		tw[1] = ((uint64_t)type << 56) |
		    (first ? (uint64_t)1 << 62 : 0) |
		    (off == len ? (uint64_t)1 << 63 : 0);
		threefish(g, tw, p, c);
		for (i = 0; i < NW; i++)
			g[i] = c[i] ^ p[i];
		first = 0;
		/* "ensuring that we get at least one whole block" */
	} while (off < len);
}

/*
 * §3.5.5 with no parameters but the message and an optional key, and
 * outbits a multiple of 8 no larger than 512 -- one block of §3.5.3's
 * output.
 */
void
skein512(const uint8_t *key, size_t keylen, const void *msg, size_t len,
    unsigned outbits, uint8_t *out)
{
	uint64_t g[NW];
	uint8_t cfg[32], ctr[8];
	int i;

	for (i = 0; i < NW; i++)
		g[i] = 0;
	if (keylen != 0)
		ubi(g, key, keylen, T_KEY);

	/* Table 7, with the tree fields zero. */
	for (i = 0; i < 32; i++)
		cfg[i] = 0;
	cfg[0] = 'S'; cfg[1] = 'H'; cfg[2] = 'A'; cfg[3] = '3';
	cfg[4] = 1;
	for (i = 0; i < 8; i++)
		cfg[8 + i] = (uint8_t)((uint64_t)outbits >> (8 * i));
	ubi(g, cfg, sizeof(cfg), T_CFG);

	ubi(g, msg, len, T_MSG);

	/* §3.5.3: UBI(G, ToBytes(0, 8), Tout 2^120) */
	for (i = 0; i < 8; i++)
		ctr[i] = 0;
	ubi(g, ctr, sizeof(ctr), T_OUT);
	for (i = 0; i < (int)(outbits / 8); i++)
		out[i] = (uint8_t)(g[i / 8] >> (8 * (i % 8)));
}

/* What ZFS stores: see the top of the file, and sha512.c. */
void
skein_zfs(const uint8_t salt[32], const void *buf, size_t len,
    uint64_t out[4])
{
	uint8_t d[32];
	int i;

	skein512(salt, 32, buf, len, 256, d);
	for (i = 0; i < 4; i++)
		out[i] = le64(d + i * 8);
}

/* Appendix B's check: the chaining value after the configuration. */
void
skein512_iv(unsigned outbits, uint64_t iv[8])
{
	uint8_t cfg[32];
	int i;

	for (i = 0; i < NW; i++)
		iv[i] = 0;
	for (i = 0; i < 32; i++)
		cfg[i] = 0;
	cfg[0] = 'S'; cfg[1] = 'H'; cfg[2] = 'A'; cfg[3] = '3';
	cfg[4] = 1;
	for (i = 0; i < 8; i++)
		cfg[8 + i] = (uint8_t)((uint64_t)outbits >> (8 * i));
	ubi(iv, cfg, sizeof(cfg), T_CFG);
}
