/*	$NetBSD$	*/

/*
 * The label's name-value pairs.
 *
 * [S] §1.3.3 says only that "All name-value pairs are stored in XDR
 * encoded nvlists", and names the pairs a label holds.  It does not
 * describe the encoding, so the shape of the stream is taken from
 * [Z] module/nvpair/nvpair.c (nvs_xdr_nvpair, nvs_xdr_nvl_fini and
 * nvs_operation), and the encoding of each field from XDR itself,
 * RFC 4506: integers are four bytes big endian, and anything shorter
 * than a multiple of four is padded up with zeros.
 *
 * A stream is
 *
 *	1 byte	encoding (0 native, 1 XDR)
 *	1 byte	endianness of the writer
 *	2 bytes	reserved
 *	4 bytes	nvl_version
 *	4 bytes	nvl_nvflag
 *	then nvpairs, and two zero words to end
 *
 * and an nvpair is
 *
 *	4 bytes	encoded size, including these two words; zero ends the list
 *	4 bytes	decoded size
 *	4 bytes	name length, then the name padded to four
 *	4 bytes	type
 *	4 bytes	element count
 *	the value
 *
 * Only the encoded size is needed to step over a pair, so a type this
 * reader does not know is skipped rather than guessed at.
 *
 * Every read goes through one of the three accessors below, and each of
 * them checks the whole of what it is about to read against the end of
 * the buffer before touching it.  This is deliberate: a parser that
 * checks at the call sites instead gets one of them wrong, and a label
 * is attacker-controlled in the sense that matters -- it is whatever is
 * on the disk that was plugged in.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include "zfs_ondisk.h"
#include "nvlist.h"

#define	NV_ENCODE_XDR		1	/* [Z] nvpair.h */

#define	DATA_TYPE_UINT64	8	/* [Z] nvpair.h */
#define	DATA_TYPE_STRING	9
#define	DATA_TYPE_NVLIST	19
#define	DATA_TYPE_NVLIST_ARRAY	20

struct nvs {
	const uint8_t	*base;
	size_t		len;
	size_t		pos;
};

static int
nv_u32(struct nvs *s, uint32_t *out)
{

	if (s->pos + 4 > s->len)
		return (EINVAL);
	*out = ((uint32_t)s->base[s->pos] << 24) |
	    ((uint32_t)s->base[s->pos + 1] << 16) |
	    ((uint32_t)s->base[s->pos + 2] << 8) |
	    s->base[s->pos + 3];
	s->pos += 4;
	return (0);
}

static int
nv_u64(struct nvs *s, uint64_t *out)
{
	uint32_t hi, lo;

	if (nv_u32(s, &hi) != 0 || nv_u32(s, &lo) != 0)
		return (EINVAL);
	*out = ((uint64_t)hi << 32) | lo;
	return (0);
}

/* An XDR string: a length, then that many bytes padded up to four. */
static int
nv_string(struct nvs *s, const char **p, uint32_t *lenp)
{
	uint32_t n, pad;

	if (nv_u32(s, &n) != 0)
		return (EINVAL);
	pad = (4 - (n & 3)) & 3;
	if (n > s->len - s->pos || pad > s->len - s->pos - n)
		return (EINVAL);
	*p = (const char *)s->base + s->pos;
	*lenp = n;
	s->pos += n + pad;
	return (0);
}

static int
nv_name_is(const char *p, uint32_t len, const char *name)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		if (p[i] != name[i] || name[i] == '\0')
			return (0);
	return (name[len] == '\0');
}

/*
 * Walk a list that has already had its four byte header stepped over,
 * and return the pair named.  The value is returned as whichever of the
 * three shapes its type calls for; a type that was not asked for is an
 * error rather than a silent zero.
 */
static int
nv_walk(struct nvs *s, const char *name, int want, struct nvpair_value *out)
{
	uint32_t version, nvflag;

	if (nv_u32(s, &version) != 0 || nv_u32(s, &nvflag) != 0)
		return (EINVAL);

	for (;;) {
		uint32_t encsize, decsize, namelen, type, nelem;
		const char *nm;
		size_t next;

		if (nv_u32(s, &encsize) != 0)
			return (EINVAL);
		if (encsize == 0)
			return (ENOENT);	/* [Z] the two zero words */
		if (nv_u32(s, &decsize) != 0)
			return (EINVAL);

		/*
		 * encsize counts from the start of the pair, so the next
		 * one is found without understanding this one's type.
		 */
		if (encsize < 8 || encsize - 8 > s->len - s->pos)
			return (EINVAL);
		next = s->pos - 8 + encsize;

		if (nv_string(s, &nm, &namelen) != 0 ||
		    nv_u32(s, &type) != 0 || nv_u32(s, &nelem) != 0)
			return (EINVAL);

		if (!nv_name_is(nm, namelen, name)) {
			s->pos = next;
			continue;
		}

		switch (type) {
		case DATA_TYPE_UINT64:
			if (want != NV_WANT_UINT64 || nelem != 1)
				return (EINVAL);
			return (nv_u64(s, &out->nv_u64));
		case DATA_TYPE_STRING:
			if (want != NV_WANT_STRING || nelem != 1)
				return (EINVAL);
			return (nv_string(s, &out->nv_string,
			    &out->nv_strlen));
		case DATA_TYPE_NVLIST:
		case DATA_TYPE_NVLIST_ARRAY:
			if (want != NV_WANT_NVLIST)
				return (EINVAL);
			/*
			 * [Z] nvs_embedded: a nested list is the same
			 * stream again without the four byte header, so
			 * its version and flag words come first and
			 * nvlist_find_nested is what reads it.  An array
			 * of lists is the lists one after another, which
			 * the same walk handles because each ends with
			 * its own two zero words.
			 */
			out->nv_list = s->base + s->pos;
			out->nv_listlen = next - s->pos;
			out->nv_nelem = nelem;
			return (0);
		default:
			return (EINVAL);
		}
	}
}

/* A whole stream: the four byte header, then the list. */
int
nvlist_find(const void *buf, size_t len, const char *name, int want,
    struct nvpair_value *out)
{
	struct nvs s;

	if (len < 4)
		return (EINVAL);
	if (((const uint8_t *)buf)[0] != NV_ENCODE_XDR)
		return (ENOTSUP);

	s.base = buf;
	s.len = len;
	s.pos = 4;
	return (nv_walk(&s, name, want, out));
}

/* A list nested inside another, which carries no header of its own. */
int
nvlist_find_nested(const void *buf, size_t len, const char *name, int want,
    struct nvpair_value *out)
{
	struct nvs s;

	s.base = buf;
	s.len = len;
	s.pos = 0;
	return (nv_walk(&s, name, want, out));
}
