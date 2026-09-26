/*
 * Skein-512 against the Skein 1.3 paper: the three Skein-512-512 test
 * vectors of Appendix C.2, and Appendix B.8's chaining value after the
 * configuration block of Skein-512-256, which is the output length ZFS
 * uses.  The keyed form ZFS uses has no published vector here; a pool
 * written with checksum=skein is its test (openzfs.sh).
 */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "skein.h"

static int
vec(size_t len, const char *want)
{
	uint8_t msg[128], out[64];
	char got[129];
	size_t i;

	for (i = 0; i < len; i++)
		msg[i] = (uint8_t)(0xff - i);
	skein512(NULL, 0, msg, len, 512, out);
	for (i = 0; i < 64; i++)
		snprintf(got + i * 2, 3, "%02x", out[i]);
	if (strcmp(got, want) != 0) {
		printf("FAIL len %zu\n  got  %s\n  want %s\n", len, got, want);
		return (1);
	}
	return (0);
}

int
main(void)
{
	static const uint64_t b8[8] = {
		0xCCD044A12FDB3E13ULL, 0xE83590301A79A9EBULL,
		0x55AEA0614F816E6FULL, 0x2A2767A4AE9B94DBULL,
		0xEC06025E74DD7683ULL, 0xE7A436CDC4746251ULL,
		0xC36FBAF9393AD185ULL, 0x3EEDBA1833EDFC13ULL
	};
	uint64_t iv[8];
	int bad = 0, i;

	bad |= vec(1, "71b7bce6fe6452227b9ced6014249e5bf9a9754c3ad618ccc4e0aae16b316cc8"
	    "ca698d864307ed3e80b6ef1570812ac5272dc409b5a012df2a579102f340617a");
	bad |= vec(64, "45863ba3be0c4dfc27e75d358496f4ac9a736a505d9313b42b2f5eada79fc17f"
	    "63861e947afb1d056aa199575ad3f8c9a3cc1780b5e5fa4cae050e989876625b");
	bad |= vec(128, "91cca510c263c4ddd010530a33073309628631f308747e1bcbaa90e451cab92e"
	    "5188087af4188773a332303e6667a7a210856f742139000071f48e8ba2a5adb7");
	skein512_iv(256, iv);
	for (i = 0; i < 8; i++)
		if (iv[i] != b8[i]) {
			printf("FAIL B.8 word %d: %016llx\n", i,
			    (unsigned long long)iv[i]);
			bad = 1;
		}
	printf(bad ? "=== Skein FAILED\n" :
	    "=== Skein-512: Appendix C.2's three vectors and B.8's IV\n");
	return (bad);
}
