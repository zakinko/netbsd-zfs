/*	$NetBSD$	*/

/*
 * Zstandard decompression, for ZFS blocks compressed with zstd.
 *
 * Written from [R] RFC 8878, "Zstandard Compression and the
 * 'application/zstd' Media Type", read with its verified errata 6441,
 * 6442 and 7297; spec/notes-zstd.md is the transcription.  Only the
 * decompressor is here, and only what a ZFS block can hold: one frame,
 * no dictionary.
 *
 * Every read of the input and every write of the output is checked
 * against its buffer first.  The input is whatever is on the disk, and
 * a checksum only says that it is what was written.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "zfs_ondisk.h"	/* NULL, and what scratch.h sizes by */
#include "zstd.h"
#include "scratch.h"

#define	ZSTD_BLOCK_MAX		(128 * 1024)	/* [R] §3.1.1.2.4 */
#define	HUF_MAXBITS		11		/* [R] §4.2.1 */
#define	HUF_MAXWEIGHTS		255		/* [R] §4.2.1.2 */
#define	HUFW_MAXAL		6		/* [R] §4.2.1.2 */
#define	LL_MAXAL		9		/* [R] §3.1.1.3.2.1 */
#define	ML_MAXAL		9
#define	OF_MAXAL		8
#define	LL_NSYM			36		/* [R] Table 16: codes 0-35 */
#define	ML_NSYM			53		/* [R] Table 17: codes 0-52 */
/*
 * [R] §3.1.1.3.2.1.1: "A decoder is free to limit its maximum supported
 * value for N", with at least 22 recommended.  31 keeps an offset value
 * inside 32 bits; a ZFS block is 128K and needs 17.
 */
#define	OF_NSYM			32

static int
highbit32(uint32_t v)		/* index of the highest set bit; v != 0 */
{
	int n = 0;

	while (v >>= 1)
		n++;
	return (n);
}

/*
 * Bitstreams read backward: [R] §3.1.1.3.2.1.2 and §4.2.2.
 *
 * The writer went forward, filling each byte from its low bit, and
 * ended with a 1 bit and zeros up to the byte boundary.  The reader
 * starts below that 1 and goes down; n bits read are the n below the
 * current position, the highest of them first -- which is what makes
 * "reading the highest Max_Number_of_Bits bits" in §4.2.2 come out as
 * a table index.
 *
 * pos counts the bits not yet read, and goes negative when a read runs
 * past the start.  The bits there are read as zeros: §4.2.1.2 asks for
 * exactly that, and every other caller checks pos afterwards.
 */
struct bitr {
	const uint8_t	*p;
	int64_t		pos;
};

static int
bitr_init(struct bitr *b, const uint8_t *p, size_t len)
{

	/* "The last byte of the compressed bitstream cannot be zero." */
	if (len == 0 || p[len - 1] == 0)
		return (EINVAL);
	b->p = p;
	b->pos = (int64_t)(len - 1) * 8 + highbit32(p[len - 1]);
	return (0);
}

static uint32_t
bitr_peek(const struct bitr *b, unsigned n)	/* n <= 32 */
{
	int64_t lo = b->pos - n, i;
	uint64_t v = 0;
	unsigned shift = 0;

	if (n == 0 || b->pos <= 0)
		return (0);
	if (lo < 0) {
		/* Past the start: the missing low bits are zeros. */
		shift = (unsigned)-lo;
		lo = 0;
	}
	for (i = (b->pos - 1) >> 3; i >= (lo >> 3); i--)
		v = (v << 8) | b->p[i];
	v >>= lo & 7;
	v &= ((uint64_t)1 << (b->pos - lo)) - 1;
	return ((uint32_t)(v << shift));
}

static uint32_t
bitr_read(struct bitr *b, unsigned n)
{
	uint32_t v = bitr_peek(b, n);

	b->pos -= n;
	return (v);
}

/*
 * FSE: [R] §4.1.  A table of 1 << Accuracy_Log cells, each a symbol and
 * the rule for the next state.
 */
struct fse {
	uint16_t	base;
	uint8_t		sym;
	uint8_t		nb;
};

/* Forward little endian bits, for §4.1.1; zeros past the end. */
static uint32_t
fwd_peek(const uint8_t *p, size_t len, size_t pos, unsigned n)
{
	uint32_t v = 0;
	unsigned i;

	for (i = 0; i < n; i++) {
		size_t b = pos + i;

		if (b / 8 < len && (p[b / 8] >> (b % 8)) & 1)
			v |= (uint32_t)1 << i;
	}
	return (v);
}

/*
 * [R] §4.1.1: read a distribution.  norm[] receives the probabilities,
 * -1 standing for "less than 1".
 *
 * The RFC says a table with other than "an expected number of symbols"
 * is corrupt, and also that a table describes the symbols "from 0 to
 * the last present one".  Both hold only if the expected number is the
 * most the context allows -- literal length tables routinely stop well
 * short of code 35 -- so that is how it is read here.
 */
static int
fse_read_dist(const uint8_t *src, size_t len, int16_t *norm, unsigned maxsym,
    unsigned maxal, unsigned *nsymp, unsigned *alp, size_t *usedp)
{
	size_t pos = 0;			/* in bits, forward */
	unsigned al, nsym = 0;
	int32_t remaining;

#define	PEEK(n)	(fwd_peek(src, len, pos, (n)))
	if (len < 1)
		return (EINVAL);
	al = (src[0] & 0xf) + 5;
	pos = 4;
	if (al > maxal)
		return (EINVAL);

	remaining = (int32_t)1 << al;
	while (remaining > 0) {
		uint32_t maxv, lowcount, v;
		unsigned nb;
		int32_t p;

		if (nsym >= maxsym)
			return (EINVAL);
		/*
		 * Values 0 .. remaining + 1 can be read.  nb bits hold
		 * them all; the lowcount smallest take one bit fewer
		 * ([R] Table 20).
		 */
		maxv = (uint32_t)remaining + 1;
		nb = highbit32(maxv) + 1;
		lowcount = ((uint32_t)1 << nb) - 1 - maxv;
		v = PEEK(nb - 1);
		if (v < lowcount) {
			pos += nb - 1;
		} else {
			v = PEEK(nb);
			if (v >= ((uint32_t)1 << (nb - 1)))
				v -= lowcount;
			pos += nb;
		}
		p = (int32_t)v - 1;
		norm[nsym++] = (int16_t)p;
		remaining -= (p < 0) ? 1 : p;
		if (remaining < 0)
			return (EINVAL);

		if (p == 0) {
			/* A run of zero probabilities, two bits at a time. */
			for (;;) {
				uint32_t r = PEEK(2), i;

				pos += 2;
				for (i = 0; i < r; i++) {
					if (nsym >= maxsym)
						return (EINVAL);
					norm[nsym++] = 0;
				}
				if (r != 3)
					break;
			}
		}
		if ((pos + 7) / 8 > len)
			return (EINVAL);
	}
#undef	PEEK
	*nsymp = nsym;
	*alp = al;
	*usedp = (pos + 7) / 8;
	return (0);
}

/*
 * [R] §4.1.1: spread the symbols over the table, then give each cell
 * its Num_Bits and Baseline.
 *
 * The RFC describes the second step by sorting each symbol's cells and
 * dividing the state space among them.  Counting instead: a symbol with
 * probability p is met p times going through the table in order, and
 * the k-th time (k from 0) its cell's next state lies in the range that
 * starts at x = p + k.  Num_Bits is Accuracy_Log minus the index of x's
 * highest bit, and Baseline is x shifted up by it, less the table size.
 * That is Table 21's example: p = 5, Accuracy_Log 7, gives x = 5..9,
 * Num_Bits 5, 5, 5, 4, 4 and Baseline 32, 64, 96, 0, 16.
 */
static int
fse_build(struct fse *t, const int16_t *norm, unsigned nsym, unsigned al)
{
	uint16_t next[256];
	uint32_t size = (uint32_t)1 << al, high = size - 1, pos = 0, u;
	uint32_t step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
	unsigned s;
	int i;

	for (s = 0; s < nsym; s++) {
		if (norm[s] == -1) {
			/* "starting from the end of the table and retreating" */
			t[high--].sym = (uint8_t)s;
			next[s] = 1;
		} else
			next[s] = (uint16_t)norm[s];
	}
	for (s = 0; s < nsym; s++) {
		for (i = 0; i < norm[s]; i++) {
			t[pos].sym = (uint8_t)s;
			do
				pos = (pos + step) & mask;
			while (pos > high);
		}
	}
	/*
	 * step is odd, so it visits every cell once; ending anywhere but
	 * where it began means the probabilities did not fill the table.
	 */
	if (pos != 0)
		return (EINVAL);

	for (u = 0; u < size; u++) {
		uint32_t x = next[t[u].sym]++;
		unsigned nb = al - highbit32(x);

		t[u].nb = (uint8_t)nb;
		t[u].base = (uint16_t)((x << nb) - size);
	}
	return (0);
}

/* The FSE table of one symbol, with one entry: [R] Table 15's RLE_Mode. */
static void
fse_rle(struct fse *t, uint8_t sym)
{

	t[0].sym = sym;
	t[0].nb = 0;
	t[0].base = 0;
}

/*
 * Huffman: [R] §4.2.  A decoding table of 1 << maxbits cells, indexed
 * by the next maxbits bits of the stream.
 */
struct huf {
	uint8_t		sym;
	uint8_t		nb;
};

/*
 * [R] §4.2.1: the tree description, and the table built from it.
 */
static int
huf_read(const uint8_t *src, size_t len, struct huf *table,
    unsigned *maxbitsp, size_t *usedp)
{
	uint8_t w[HUF_MAXWEIGHTS + 1];
	uint32_t count[HUF_MAXBITS + 1], start[HUF_MAXBITS + 1];
	uint32_t total = 0, rest;
	unsigned n = 0, i, maxbits, hb;
	size_t used;

	if (len < 1)
		return (EINVAL);
	hb = src[0];
	if (hb >= 128) {
		/* [R] §4.2.1.1: direct, four bits a weight, high first. */
		n = hb - 127;
		used = 1 + (n + 1) / 2;
		if (used > len)
			return (EINVAL);
		for (i = 0; i < n; i++)
			w[i] = (i & 1) ? src[1 + i / 2] & 0xf :
			    src[1 + i / 2] >> 4;
	} else {
		/* [R] §4.2.1.2: FSE, two states taking turns. */
		int16_t norm[HUF_MAXBITS + 1];
		struct fse t[1 << HUFW_MAXAL];
		struct bitr b;
		unsigned nsym, al;
		size_t dused;
		uint32_t s1, s2;

		used = 1 + hb;
		if (used > len || hb == 0)
			return (EINVAL);
		if (fse_read_dist(src + 1, hb, norm, HUF_MAXBITS + 1,
		    HUFW_MAXAL, &nsym, &al, &dused) != 0)
			return (EINVAL);
		if (fse_build(t, norm, nsym, al) != 0)
			return (EINVAL);
		if (bitr_init(&b, src + 1 + dused, hb - dused) != 0)
			return (EINVAL);
		s1 = bitr_read(&b, al);
		s2 = bitr_read(&b, al);
		for (;;) {
			if (n >= HUF_MAXWEIGHTS)
				return (EINVAL);
			w[n++] = t[s1].sym;
			s1 = t[s1].base + bitr_read(&b, t[s1].nb);
			if (b.pos < 0) {
				if (n >= HUF_MAXWEIGHTS)
					return (EINVAL);
				w[n++] = t[s2].sym;
				break;
			}
			if (n >= HUF_MAXWEIGHTS)
				return (EINVAL);
			w[n++] = t[s2].sym;
			s2 = t[s2].base + bitr_read(&b, t[s2].nb);
			if (b.pos < 0) {
				if (n >= HUF_MAXWEIGHTS)
					return (EINVAL);
				w[n++] = t[s1].sym;
				break;
			}
		}
	}

	/*
	 * [R] §4.2.1: the last weight completes the sum of
	 * 2^(Weight-1) to the next power of two, which is
	 * 2^Max_Number_of_Bits.  The last symbol is present, so that
	 * power is strictly above the sum, and the difference has to be
	 * a power of two itself.
	 */
	for (i = 0; i < n; i++) {
		if (w[i] > HUF_MAXBITS)
			return (EINVAL);
		if (w[i] > 0)
			total += (uint32_t)1 << (w[i] - 1);
	}
	if (total == 0)
		return (EINVAL);
	maxbits = highbit32(total) + 1;
	if (maxbits > HUF_MAXBITS)
		return (EINVAL);
	rest = ((uint32_t)1 << maxbits) - total;
	if ((rest & (rest - 1)) != 0)
		return (EINVAL);
	w[n++] = (uint8_t)(highbit32(rest) + 1);

	/*
	 * [R] §4.2.1.3: by weight, lowest first, and in symbol order
	 * within a weight, each symbol takes 2^(Weight-1) consecutive
	 * cells.  That is Table 25.  Table 26 swaps the codes of symbols
	 * 4 and 5, which have the same weight; the prose sides with
	 * Table 25, and so does data OpenZFS wrote.
	 */
	for (i = 0; i <= HUF_MAXBITS; i++)
		count[i] = 0;
	for (i = 0; i < n; i++)
		count[w[i]]++;
	rest = 0;
	for (i = 1; i <= maxbits; i++) {
		start[i] = rest;
		rest += count[i] << (i - 1);
	}
	for (i = 0; i < n; i++) {
		uint32_t k, cells;

		if (w[i] == 0)
			continue;
		cells = (uint32_t)1 << (w[i] - 1);
		for (k = 0; k < cells; k++) {
			table[start[w[i]] + k].sym = (uint8_t)i;
			table[start[w[i]] + k].nb =
			    (uint8_t)(maxbits + 1 - w[i]);
		}
		start[w[i]] += cells;
	}
	*maxbitsp = maxbits;
	*usedp = used;
	return (0);
}

/* [R] §4.2.2: one stream, exactly used up. */
static int
huf_stream(const struct huf *t, unsigned maxbits, const uint8_t *src,
    size_t len, uint8_t *out, size_t n)
{
	struct bitr b;
	size_t i;

	if (bitr_init(&b, src, len) != 0)
		return (EINVAL);
	for (i = 0; i < n; i++) {
		const struct huf *e = &t[bitr_peek(&b, maxbits)];

		out[i] = e->sym;
		b.pos -= e->nb;
		if (b.pos < 0)
			return (EINVAL);
	}
	return (b.pos == 0 ? 0 : EINVAL);
}

/*
 * What a frame carries from one block to the next: [R] §3.1.1.3.
 */
struct zstd_dctx {
	struct fse	ll[1 << LL_MAXAL];
	struct fse	of[1 << OF_MAXAL];
	struct fse	ml[1 << ML_MAXAL];
	unsigned	ll_al, of_al, ml_al;
	int		ll_ok, of_ok, ml_ok;
	struct huf	huf[1 << HUF_MAXBITS];
	unsigned	huf_bits;
	int		huf_ok;
	uint32_t	rep[3];
	uint8_t		*lit;		/* ZSTD_BLOCK_MAX bytes */
};

/* scratch.h budgets ZSTD_STATE_MAX for this; a negative size fails. */
extern char zstd_dctx_fits[sizeof(struct zstd_dctx) <= ZSTD_STATE_MAX ? 1 : -1];

/*
 * [R] Tables 16 and 17: each code's baseline and the extra bits read
 * after it.
 */
static const uint32_t ll_base[LL_NSYM] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512,
	1024, 2048, 4096, 8192, 16384, 32768, 65536
};
static const uint8_t ll_bits[LL_NSYM] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9,
	10, 11, 12, 13, 14, 15, 16
};
static const uint32_t ml_base[ML_NSYM] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
	19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
	35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515,
	1027, 2051, 4099, 8195, 16387, 32771, 65539
};
static const uint8_t ml_bits[ML_NSYM] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9,
	10, 11, 12, 13, 14, 15, 16
};

/* [R] §3.1.1.3.2.2: the predefined distributions. */
static const int16_t ll_default[LL_NSYM] = {
	4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
	-1, -1, -1, -1
};
static const int16_t ml_default[ML_NSYM] = {
	1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1,
	-1, -1, -1, -1, -1
};
static const int16_t of_default[29] = {
	1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1
};

/*
 * [R] §3.1.1.3.2.1: one symbol type's table, by its Compression_Mode.
 */
static int
seq_table(struct fse *t, unsigned *alp, int *okp, int mode,
    const int16_t *def, unsigned ndef, unsigned defal, unsigned nsym,
    unsigned maxal, const uint8_t **pp, const uint8_t *end)
{
	int16_t norm[ML_NSYM];
	unsigned n, al;
	size_t used;

	switch (mode) {
	case 0:		/* Predefined_Mode */
		if (fse_build(t, def, ndef, defal) != 0)
			return (EINVAL);
		*alp = defal;
		break;
	case 1:		/* RLE_Mode */
		if (*pp >= end || **pp >= nsym)
			return (EINVAL);
		fse_rle(t, **pp);
		(*pp)++;
		*alp = 0;
		break;
	case 2:		/* FSE_Compressed_Mode */
		if (fse_read_dist(*pp, (size_t)(end - *pp), norm, nsym, maxal,
		    &n, &al, &used) != 0 || fse_build(t, norm, n, al) != 0)
			return (EINVAL);
		*pp += used;
		*alp = al;
		break;
	default:	/* Repeat_Mode */
		if (!*okp)
			return (EINVAL);
		return (0);
	}
	*okp = 1;
	return (0);
}

/*
 * [R] §3.1.1.3.1: the literals section.  On return *litp is lit bytes
 * of literals and *usedp the size of the section.
 */
static int
lit_read(struct zstd_dctx *z, const uint8_t *src, size_t len,
    const uint8_t **litp, size_t *litn, size_t *usedp)
{
	unsigned type, sf, hdr, bits, nstreams;
	uint64_t h = 0;
	size_t regen, csize, i;

	if (len < 1)
		return (EINVAL);
	type = src[0] & 3;
	sf = (src[0] >> 2) & 3;

	if (type == 0 || type == 1) {
		/* [R] §3.1.1.3.1.1: Raw and RLE. */
		switch (sf) {
		case 0:
		case 2:
			hdr = 1;
			regen = src[0] >> 3;
			break;
		case 1:
			hdr = 2;
			if (len < hdr)
				return (EINVAL);
			regen = (src[0] >> 4) + ((size_t)src[1] << 4);
			break;
		default:
			hdr = 3;
			if (len < hdr)
				return (EINVAL);
			regen = (src[0] >> 4) + ((size_t)src[1] << 4) +
			    ((size_t)src[2] << 12);
			break;
		}
		if (regen > ZSTD_BLOCK_MAX)
			return (EINVAL);
		if (type == 0) {
			if (regen > len - hdr)
				return (EINVAL);
			*litp = src + hdr;
			*usedp = hdr + regen;
		} else {
			if (len - hdr < 1)
				return (EINVAL);
			for (i = 0; i < regen; i++)
				z->lit[i] = src[hdr];
			*litp = z->lit;
			*usedp = hdr + 1;
		}
		*litn = regen;
		return (0);
	}

	/* [R] §3.1.1.3.1.1: Compressed and Treeless. */
	switch (sf) {
	case 0:
		nstreams = 1; bits = 10; hdr = 3;
		break;
	case 1:
		nstreams = 4; bits = 10; hdr = 3;
		break;
	case 2:
		nstreams = 4; bits = 14; hdr = 4;
		break;
	default:
		nstreams = 4; bits = 18; hdr = 5;
		break;
	}
	if (len < hdr)
		return (EINVAL);
	for (i = hdr; i-- > 0; )
		h = (h << 8) | src[i];
	regen = (size_t)(h >> 4) & (((size_t)1 << bits) - 1);
	csize = (size_t)(h >> (4 + bits)) & (((size_t)1 << bits) - 1);
	if (regen > ZSTD_BLOCK_MAX || csize > len - hdr)
		return (EINVAL);
	*usedp = hdr + csize;
	src += hdr;

	if (type == 2) {
		size_t tused;

		if (huf_read(src, csize, z->huf, &z->huf_bits, &tused) != 0)
			return (EINVAL);
		z->huf_ok = 1;
		src += tused;
		csize -= tused;
	} else if (!z->huf_ok)
		return (EINVAL);	/* Treeless with no tree before it */

	if (nstreams == 1) {
		if (huf_stream(z->huf, z->huf_bits, src, csize, z->lit,
		    regen) != 0)
			return (EINVAL);
	} else {
		/*
		 * [R] §3.1.1.3.1.6.  Errata 7297: four streams need six
		 * bytes of each size at least, or Stream4_Size goes
		 * negative -- and so, on the regenerated side, does the
		 * last stream's share.
		 */
		size_t sz[4], per, off = 6, done = 0;
		int k;

		if (csize < 6 || regen < 6)
			return (EINVAL);
		sz[0] = src[0] | ((size_t)src[1] << 8);
		sz[1] = src[2] | ((size_t)src[3] << 8);
		sz[2] = src[4] | ((size_t)src[5] << 8);
		if (sz[0] + sz[1] + sz[2] > csize - 6)
			return (EINVAL);
		sz[3] = csize - 6 - sz[0] - sz[1] - sz[2];
		per = (regen + 3) / 4;
		for (k = 0; k < 4; k++) {
			size_t n = (k < 3) ? per : regen - done;

			if (huf_stream(z->huf, z->huf_bits, src + off, sz[k],
			    z->lit + done, n) != 0)
				return (EINVAL);
			off += sz[k];
			done += n;
		}
	}
	*litp = z->lit;
	*litn = regen;
	return (0);
}

/*
 * [R] §3.1.1.3: one compressed block, whose output goes at out, with
 * *op bytes of the frame already before it and room up to outend.
 */
static int
block_decode(struct zstd_dctx *z, const uint8_t *src, size_t len,
    uint8_t *out, size_t *op, size_t outend)
{
	const uint8_t *p, *end = src + len, *lit;
	size_t litn, used, o = *op, blockstart = *op;
	uint32_t nseq, s_ll, s_of, s_ml, i;
	struct bitr b;
	unsigned modes;

	if (lit_read(z, src, len, &lit, &litn, &used) != 0)
		return (EINVAL);
	p = src + used;

	/* [R] §3.1.1.3.2.1: Number_of_Sequences. */
	if (p >= end)
		return (EINVAL);
	if (p[0] == 0) {
		nseq = 0;
		p++;
	} else if (p[0] < 128) {
		nseq = p[0];
		p++;
	} else if (p[0] < 255) {
		if (end - p < 2)
			return (EINVAL);
		nseq = ((uint32_t)(p[0] - 128) << 8) + p[1];
		p += 2;
	} else {
		if (end - p < 3)
			return (EINVAL);
		nseq = p[1] + ((uint32_t)p[2] << 8) + 0x7f00;
		p += 3;
	}

	if (nseq == 0) {
		/*
		 * "Decompressed content is defined entirely as
		 * Literals_Section content", and the tables for
		 * Repeat_Mode are left as they were.
		 */
		if (p != end || litn > outend - o)
			return (EINVAL);
		for (i = 0; i < litn; i++)
			out[o++] = lit[i];
		*op = o;
		return (0);
	}

	if (p >= end)
		return (EINVAL);
	modes = *p++;
	if ((modes & 3) != 0)		/* Table 14: Reserved */
		return (EINVAL);
	if (seq_table(z->ll, &z->ll_al, &z->ll_ok, (modes >> 6) & 3,
	    ll_default, LL_NSYM, 6, LL_NSYM, LL_MAXAL, &p, end) != 0 ||
	    seq_table(z->of, &z->of_al, &z->of_ok, (modes >> 4) & 3,
	    of_default, 29, 5, OF_NSYM, OF_MAXAL, &p, end) != 0 ||
	    seq_table(z->ml, &z->ml_al, &z->ml_ok, (modes >> 2) & 3,
	    ml_default, ML_NSYM, 6, ML_NSYM, ML_MAXAL, &p, end) != 0)
		return (EINVAL);

	/* [R] §3.1.1.3.2.1.2 */
	if (bitr_init(&b, p, (size_t)(end - p)) != 0)
		return (EINVAL);
	s_ll = bitr_read(&b, z->ll_al);
	s_of = bitr_read(&b, z->of_al);
	s_ml = bitr_read(&b, z->ml_al);

	for (i = 0; i < nseq; i++) {
		unsigned llc = z->ll[s_ll].sym, ofc = z->of[s_of].sym;
		unsigned mlc = z->ml[s_ml].sym;
		uint32_t ll, ml, ov, off, k;

		/* Only reachable through a table that allowed them. */
		if (llc >= LL_NSYM || mlc >= ML_NSYM || ofc >= OF_NSYM)
			return (EINVAL);

		/* Offset first, then match length, then literals length. */
		ov = ((uint32_t)1 << ofc) + bitr_read(&b, ofc);
		ml = ml_base[mlc] + bitr_read(&b, ml_bits[mlc]);
		ll = ll_base[llc] + bitr_read(&b, ll_bits[llc]);

		/* [R] §3.1.1.5 */
		if (ov > 3) {
			off = ov - 3;
			z->rep[2] = z->rep[1];
			z->rep[1] = z->rep[0];
			z->rep[0] = off;
		} else {
			unsigned idx = ov - 1 + (ll == 0);

			if (idx == 0) {
				off = z->rep[0];
			} else if (idx == 3) {
				off = z->rep[0] - 1;
				z->rep[2] = z->rep[1];
				z->rep[1] = z->rep[0];
				z->rep[0] = off;
			} else {
				off = z->rep[idx];
				if (idx == 2)
					z->rep[2] = z->rep[1];
				z->rep[1] = z->rep[0];
				z->rep[0] = off;
			}
		}

		/* [R] §3.1.1.4 */
		if (ll > litn || ll > outend - o)
			return (EINVAL);
		for (k = 0; k < ll; k++)
			out[o++] = lit[k];
		lit += ll;
		litn -= ll;
		/*
		 * The window is what this frame has produced so far, and
		 * a ZFS block is one frame.  A match reaching before it,
		 * or an offset of zero, is corrupt.
		 */
		if (off == 0 || off > o || ml > outend - o)
			return (EINVAL);
		for (k = 0; k < ml; k++, o++)
			out[o] = out[o - off];

		if (i + 1 < nseq) {
			/* Literals length, then match length, then offset. */
			s_ll = z->ll[s_ll].base + bitr_read(&b, z->ll[s_ll].nb);
			s_ml = z->ml[s_ml].base + bitr_read(&b, z->ml[s_ml].nb);
			s_of = z->of[s_of].base + bitr_read(&b, z->of[s_of].nb);
		}
		if (b.pos < 0)
			return (EINVAL);
	}
	/* "the bitstream shall be entirely consumed" */
	if (b.pos != 0)
		return (EINVAL);

	if (litn > outend - o)
		return (EINVAL);
	for (i = 0; i < litn; i++)
		out[o++] = lit[i];

	/* [R] §3.1.1.2.4: the block's output is bounded too. */
	if (o - blockstart > ZSTD_BLOCK_MAX)
		return (EINVAL);
	*op = o;
	return (0);
}

/*
 * [R] §3.1.1: one frame, without its Magic_Number -- see
 * zfs_zstd_decompress() -- which must decode to exactly dstlen bytes.
 */
static int
frame_decode(struct zstd_dctx *z, const uint8_t *src, size_t len,
    uint8_t *dst, size_t dstlen)
{
	const uint8_t *p = src, *end = src + len;
	unsigned fhd, fcsflag, single, cksum, didflag, fcslen, i;
	uint64_t window = 0, fcs = 0;
	size_t blockmax, o = 0;
	static const uint8_t fcssize[4] = { 0, 2, 4, 8 };	/* Table 4 */

	/* [R] §3.1.1.1.1 */
	if (p >= end)
		return (EINVAL);
	fhd = *p++;
	fcsflag = fhd >> 6;
	single = (fhd >> 5) & 1;
	if ((fhd >> 3) & 1)			/* §3.1.1.1.1.4 */
		return (EINVAL);
	cksum = (fhd >> 2) & 1;
	didflag = fhd & 3;
	/*
	 * [R] §5: a frame naming a dictionary cannot be decoded without
	 * it, and ZFS never names one.
	 */
	if (didflag != 0)
		return (ENOTSUP);

	/* [R] §3.1.1.1.2 */
	if (!single) {
		unsigned wl;

		if (p >= end)
			return (EINVAL);
		wl = 10 + (*p >> 3);
		window = ((uint64_t)1 << wl) + (((uint64_t)1 << wl) / 8) *
		    (*p & 7);
		p++;
	}

	/* [R] §3.1.1.1.4 */
	fcslen = (fcsflag == 0) ? (single ? 1 : 0) : fcssize[fcsflag];
	if ((size_t)(end - p) < fcslen)
		return (EINVAL);
	for (i = fcslen; i-- > 0; )
		fcs = (fcs << 8) | p[i];
	if (fcslen == 2)
		fcs += 256;
	p += fcslen;
	if (single)
		window = fcs;
	if (fcslen != 0 && fcs != dstlen)
		return (EINVAL);

	blockmax = (window < ZSTD_BLOCK_MAX) ? (size_t)window :
	    ZSTD_BLOCK_MAX;

	/* [R] §3.1.1.5: the starting offset history. */
	z->rep[0] = 1;
	z->rep[1] = 4;
	z->rep[2] = 8;
	z->ll_ok = z->of_ok = z->ml_ok = z->huf_ok = 0;

	/* [R] §3.1.1.2 */
	for (;;) {
		uint32_t bh, last, type, bsize, k;

		if (end - p < 3)
			return (EINVAL);
		bh = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
		p += 3;
		last = bh & 1;
		type = (bh >> 1) & 3;
		bsize = bh >> 3;
		if (bsize > blockmax)
			return (EINVAL);

		switch (type) {
		case 0:		/* Raw_Block */
			if (bsize > (size_t)(end - p) || bsize > dstlen - o)
				return (EINVAL);
			for (k = 0; k < bsize; k++)
				dst[o++] = p[k];
			p += bsize;
			break;
		case 1:		/* RLE_Block */
			if (p >= end || bsize > dstlen - o)
				return (EINVAL);
			for (k = 0; k < bsize; k++)
				dst[o++] = *p;
			p++;
			break;
		case 2:		/* Compressed_Block */
			if (bsize > (size_t)(end - p) ||
			    block_decode(z, p, bsize, dst, &o, dstlen) != 0)
				return (EINVAL);
			p += bsize;
			break;
		default:	/* "a compliant decoder must reject it" */
			return (EINVAL);
		}
		if (last)
			break;
	}

	/*
	 * [R] §3.1.1: Content_Checksum.  It is skipped, not checked:
	 * the block's own checksum in its block pointer has already
	 * covered every byte of the frame.
	 */
	if (cksum && end - p < 4)
		return (EINVAL);
	return (o == dstlen ? 0 : EINVAL);
}

int
zstd_decompress(const void *src, size_t srclen, void *dst, size_t dstlen)
{
	struct zstd_dctx *z;
	int err;

	if ((z = zfs_scratch_get(ZSTD_STATE_MAX)) == NULL)
		return (ENOMEM);
	if ((z->lit = zfs_scratch_get(ZSTD_BLOCK_MAX)) == NULL) {
		zfs_scratch_put(z, ZSTD_STATE_MAX);
		return (ENOMEM);
	}
	err = frame_decode(z, src, srclen, dst, dstlen);
	zfs_scratch_put(z->lit, ZSTD_BLOCK_MAX);
	zfs_scratch_put(z, ZSTD_STATE_MAX);
	return (err);
}

/*
 * [Z] zfs_zstd.c: ZFS puts eight bytes in front of the frame, both big
 * endian: the frame's length, and the version and level it was written
 * with, which decoding does not need.  And the frame is "magicless":
 * ZFS writes it with ZSTD_f_zstd1_magicless, a format of the reference
 * library that leaves out [R] §3.1.1's four byte Magic_Number.  RFC
 * 8878 does not define it; the frame simply starts at its header.
 */
int
zfs_zstd_decompress(const void *src, void *dst, size_t srclen, size_t dstlen)
{
	const uint8_t *in = src;
	uint32_t clen;

	if (srclen < 8)
		return (EINVAL);
	clen = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
	    ((uint32_t)in[2] << 8) | in[3];
	if (clen > srclen - 8)
		return (EINVAL);
	return (zstd_decompress(in + 8, clen, dst, dstlen));
}
