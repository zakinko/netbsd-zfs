/*
 * Labels that are wrong in ways ZFS never writes, each of which must be
 * refused rather than read past.  Run under ASan: a refusal that
 * happens only after an out-of-bounds read still fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvlist.h"

static size_t
w32(uint8_t *p, size_t o, uint32_t v)
{

	p[o] = v >> 24; p[o + 1] = v >> 16; p[o + 2] = v >> 8; p[o + 3] = v;
	return (o + 4);
}

/*
 * One pair "vdev_tree", a nested list, whose encoded size is 8: less
 * than its own header.  The nested list's length used to come out as
 * next - pos with next < pos.
 */
static int
short_pair(void)
{
	uint8_t b[64], *c;
	size_t o;
	struct nvpair_value v, t;
	int e;

	memset(b, 0, sizeof(b));
	b[0] = 1;			/* XDR */
	o = w32(b, 4, 0);
	o = w32(b, o, 1);
	o = w32(b, o, 8);		/* encsize */
	o = w32(b, o, 0);		/* decsize */
	o = w32(b, o, 9);
	memcpy(b + o, "vdev_tree", 9);
	o += 12;
	o = w32(b, o, 19);		/* DATA_TYPE_NVLIST */
	o = w32(b, o, 1);
	/* An exact-size heap copy, so ASan sees any read past the end. */
	c = malloc(o);
	memcpy(c, b, o);
	e = nvlist_find(c, o, "vdev_tree", NV_WANT_NVLIST, &v);
	if (e == 0)
		(void)nvlist_find_nested(v.nv_list, v.nv_listlen, "type",
		    NV_WANT_STRING, &t);
	free(c);
	return (e != 0);
}

int
main(void)
{
	int fail = 0;

	if (short_pair())
		printf("  ok    a pair shorter than its header\n");
	else {
		printf("  FAIL  a pair shorter than its header\n");
		fail = 1;
	}
	return (fail);
}
