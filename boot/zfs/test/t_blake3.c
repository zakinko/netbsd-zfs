/*
 * t_blake3
 * t_blake3 hash|keyed <length>
 *
 * With no arguments, checks the cases below, taken from the BLAKE3
 * team's test_vectors/test_vectors.json: one chunk, a chunk boundary,
 * parents two and more levels up, in both the hash and keyed_hash
 * modes.  With arguments,
 * prints the digest of <length> bytes of the BLAKE3 team's test pattern --
 * 0, 1, ..., 250 repeating -- in the hash mode, or in keyed_hash with
 * their key, for comparing with test_vectors.json in the BLAKE3
 * repository.  Their outputs are longer than 32 bytes; the default
 * length is a prefix (§2.6).
 */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blake3.h"

static const struct { size_t len; const char *hash, *keyed; } vec[] = {
	{      0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
	    "92b2b75604ed3c761f9d6f62392c8a9227ad0ea3f09573e783f1498a4ed60d26" },
	{      1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213",
	    "6d7878dfff2f485635d39013278ae14f1454b8c0a3a2d34bc1ab38228a80c95b" },
	{     64, "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98",
	    "ba8ced36f327700d213f120b1a207a3b8c04330528586f414d09f2f7d9ccb7e6" },
	{     65, "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee",
	    "c0a4edefa2d2accb9277c371ac12fcdbb52988a86edc54f0716e1591b4326e72" },
	{   1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11",
	    "c951ecdf03288d0fcc96ee3413563d8a6d3589547f2c2fb36d9786470f1b9d6e" },
	{   1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7",
	    "75c46f6f3d9eb4f55ecaaee480db732e6c2105546f1e675003687c31719c7ba4" },
	{   1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444",
	    "357dc55de0c7e382c900fd6e320acc04146be01db6a8ce7210b7189bd664ea69" },
	{   2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a",
	    "879cf1fa2ea0e79126cb1063617a05b6ad9d0b696d0d757cf053439f60a99dd1" },
	{   2049, "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030",
	    "9f29700902f7c86e514ddc4df1e3049f258b2472b6dd5267f61bf13983b78dd5" },
	{   8193, "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b",
	    "954a2a75420c8d6547e3ba5b98d963e6fa6491addc8c023189cc519821b4a1f5" },
	{  31744, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47",
	    "efa53b389ab67c593dba624d898d0f7353ab99e4ac9d42302ee64cbf9939a419" },
	{ 102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085",
	    "1c35d1a5811083fd7119f5d5d1ba027b4d01c0c6c49fb6ff2cf75393ea5db4a7" },
};

int
main(int argc, char **argv)
{
	static const uint8_t key[] = "whats the Elvish word for friend";
	uint8_t d[32], *buf;
	uint64_t w[4];
	size_t len, i;

	if (argc == 1) {
		int bad = 0;

		for (i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
			char h[65], k[65];
			size_t j;

			if ((buf = malloc(vec[i].len ? vec[i].len : 1)) == NULL)
				return (1);
			for (j = 0; j < vec[i].len; j++)
				buf[j] = (uint8_t)(j % 251);
			blake3_hash(buf, vec[i].len, d);
			for (j = 0; j < 32; j++)
				snprintf(h + j * 2, 3, "%02x", d[j]);
			blake3_keyed(key, buf, vec[i].len, d, w);
			for (j = 0; j < 32; j++)
				snprintf(k + j * 2, 3, "%02x", d[j]);
			if (strcmp(h, vec[i].hash) != 0 ||
			    strcmp(k, vec[i].keyed) != 0) {
				printf("FAIL len %zu\n", vec[i].len);
				bad = 1;
			}
			free(buf);
		}
		printf(bad ? "=== BLAKE3 FAILED\n" : "=== BLAKE3: %zu of "
		    "the team's vectors, hash and keyed\n",
		    sizeof(vec) / sizeof(vec[0]));
		return (bad);
	}
	if (argc != 3)
		return (2);
	len = strtoul(argv[2], NULL, 0);
	if ((buf = malloc(len ? len : 1)) == NULL)
		return (1);
	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(i % 251);
	if (strcmp(argv[1], "keyed") == 0)
		blake3_keyed(key, buf, len, d, w);
	else
		blake3_hash(buf, len, d);
	for (i = 0; i < 32; i++)
		printf("%02x", d[i]);
	printf("\n");
	free(buf);
	return (0);
}
