/*
 * The dnode geometry check, exercised directly.
 *
 * dn_indblkshift, dn_nblkptr and dn_nlevels come off the disk and are
 * then used as a shift count, an array index and a loop bound.  A pool
 * that recomputes its checksums -- which nothing stops an attacker
 * doing -- can put anything in them, so the reader has to refuse them
 * rather than notice later.
 *
 * The file under test is included rather than linked, so that the check
 * can be called by name without exporting it for the sake of a test.
 *
 * Built with -fsanitize=undefined, a shift that should have been
 * refused traps instead of passing quietly.
 */
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "zfsread.c"

static int fails;

static void
expect(const char *what, int got, int want)
{

	if (got == want) {
		printf("  ok    %s\n", what);
	} else {
		printf("  FAIL  %s: got %d, want %d\n", what, got, want);
		fails++;
	}
}

/* A dnode that is fine except for what each case changes. */
static void
sane(dnode_phys_t *dn)
{

	memset(dn, 0, sizeof(*dn));
	dn->dn_type = DMU_OT_PLAIN_FILE_CONTENTS;
	dn->dn_datablkszsec = 256;		/* 128K */
	dn->dn_indblkshift = 17;		/* 128K */
	dn->dn_nblkptr = 1;
	dn->dn_nlevels = 1;
}

int
main(void)
{
	dnode_phys_t dn;

	printf("=== what the specification allows\n");
	sane(&dn);
	expect("the ordinary dnode", dnode_valid(&dn), 1);
	dn.dn_datablkszsec = 1;			/* 512 bytes */
	expect("512 byte blocks", dnode_valid(&dn), 1);
	sane(&dn); dn.dn_nblkptr = 3;
	expect("three block pointers", dnode_valid(&dn), 1);
	sane(&dn); dn.dn_nlevels = 6; dn.dn_indblkshift = 9;
	expect("six levels, 512 byte indirect", dnode_valid(&dn), 1);

	printf("=== what it does not\n");
	sane(&dn); dn.dn_datablkszsec = 0;
	expect("a zero block size", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_datablkszsec = 257;
	expect("blocks over 128K", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_nblkptr = 0;
	expect("no block pointers", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_nblkptr = 200;
	expect("two hundred block pointers", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_nlevels = 0;
	expect("no levels", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_nlevels = 7;
	expect("seven levels", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_nlevels = 2; dn.dn_indblkshift = 0;
	expect("an indirect shift of zero", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_nlevels = 2; dn.dn_indblkshift = 6;
	expect("an indirect shift under a blkptr", dnode_valid(&dn), 0);
	sane(&dn); dn.dn_nlevels = 2; dn.dn_indblkshift = 200;
	expect("an indirect shift of 200", dnode_valid(&dn), 0);

	/*
	 * And the same values through the door the loader uses.  Before
	 * the check these reached a shift by 200 and an index of 199
	 * into an array of three.
	 */
	printf("=== through zfs_read_object_block\n");
	{
		static uint8_t arena[ZFS_SCRATCH_SIZE];
		static uint8_t buf[ZFS_MAXBLOCKSIZE];
		struct zfs_pool pool;

		zfs_scratch_init(arena, sizeof(arena));
		memset(&pool, 0, sizeof(pool));

		sane(&dn); dn.dn_nlevels = 2; dn.dn_indblkshift = 200;
		expect("indirect shift of 200 is refused",
		    zfs_read_object_block(&pool, &dn, 0, buf, sizeof(buf)),
		    EINVAL);
		sane(&dn); dn.dn_nblkptr = 200; dn.dn_maxblkid = 199;
		expect("two hundred block pointers is refused",
		    zfs_read_object_block(&pool, &dn, 199, buf, sizeof(buf)),
		    EINVAL);
	}

	if (fails != 0) {
		printf("%d failed\n", fails);
		return (1);
	}
	printf("=== the dnode check holds\n");
	return (0);
}
