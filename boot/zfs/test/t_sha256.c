/* FIPS 180-4 B.1 and B.2 test vectors, plus the one million 'a' case. */
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sha256.h"

static int
check(const char *msg, size_t len, const char *want)
{
	uint64_t h[4];
	char got[65];
	int i;

	sha256(msg, len, h);
	for (i = 0; i < 4; i++)
		snprintf(got + i * 16, 17, "%016llx",
		    (unsigned long long)h[i]);
	if (strcmp(got, want) != 0) {
		printf("FAIL len=%zu\n  got  %s\n  want %s\n", len, got, want);
		return 1;
	}
	return 0;
}

int
main(void)
{
	char *big;
	int bad = 0;

	bad |= check("", 0,
	    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	bad |= check("abc", 3,
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	bad |= check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
	    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	big = malloc(1000000);
	memset(big, 'a', 1000000);
	bad |= check(big, 1000000,
	    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	free(big);
	if (!bad)
		printf("sha256: all vectors pass\n");
	return bad;
}
