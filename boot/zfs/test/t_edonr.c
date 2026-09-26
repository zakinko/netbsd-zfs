/*
 * Edon-R512.  Built with EDONR_SUBMISSION_ROTATIONS, against the October
 * 2008 submission: three of the byte aligned answers in its
 * ShortMsgKAT_512.txt (0, 128 and 255 bytes: empty, exactly one block
 * so that the padding takes a second, and just short of two), and the
 * documentation's four Edon-R512-MAC examples of §3.16, which are HMAC
 * (RFC 2104) over Edon-R512 with its 128 byte block.  Built without,
 * as ZFS computes it -- Table 2.2's rotations and the 2009 feedback --
 * against the two digests OpenZFS's own tests hold.  edonr.c says why
 * there are two.  The keyed form ZFS stores is tested by a pool
 * written with checksum=edonr (openzfs.sh).
 */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edonr.h"

#ifdef EDONR_SUBMISSION_ROTATIONS
static void
hmac(const uint8_t *key, size_t klen, const char *msg, uint8_t out[64])
{
	uint8_t k[128], buf[128 + 512], inner[64];
	size_t i, mlen = strlen(msg);

	memset(k, 0, sizeof(k));
	if (klen > 128)
		edonr512(key, klen, k);
	else
		memcpy(k, key, klen);
	for (i = 0; i < 128; i++)
		buf[i] = k[i] ^ 0x36;
	memcpy(buf + 128, msg, mlen);
	edonr512(buf, 128 + mlen, inner);
	for (i = 0; i < 128; i++)
		buf[i] = k[i] ^ 0x5c;
	memcpy(buf + 128, inner, 64);
	edonr512(buf, 128 + 64, out);
}

static int
check(const uint8_t *key, size_t klen, const char *msg, const char *want)
{
	uint8_t d[64];
	char got[129];
	int i;

	hmac(key, klen, msg, d);
	for (i = 0; i < 64; i++)
		snprintf(got + i * 2, 3, "%02X", d[i]);
	if (strcmp(got, want) != 0) {
		printf("FAIL key %zu data %zu\n  got  %s\n  want %s\n", klen,
		    strlen(msg), got, want);
		return (1);
	}
	return (0);
}

static const struct { size_t len; const char *msg, *md; } kat[] = {
	{ 0,
	    "",
	    "C7AFBDF3E5B4590EB0B25000BF83FB16D4F9B722EE7F9A2DC2BD3820"
	    "35E8EE38D6F6F15C7B8EEC85355AC59AF989799950C64557EAB0E687"
	    "D0FCBDBA90AE9704" },
	{ 128,
	    "2B6DB7CED8665EBE9DEB080295218426BDAA7C6DA9ADD2088932CDFF"
	    "BAA1C14129BCCDD70F369EFB149285858D2B1D155D14DE2FDB680A8B"
	    "027284055182A0CAE275234CC9C92863C1B4AB66F304CF0621CD5456"
	    "5F5BFF461D3B461BD40DF28198E3732501B4860EADD503D26D6E6933"
	    "8F4E0456E9E9BAF3D827AE685FB1D817",
	    "41CDCB65694CED1D056FE8802E94517D05D80B2261A035C5C64FA483"
	    "38361F58D3DB180D8EB1DB0CD5BBB6271F527A4B5915FE7A13C6BD3B"
	    "4B2607C1F12687AF" },
	{ 255,
	    "3A3A819C48EFDE2AD914FBF00E18AB6BC4F14513AB27D0C178A188B6"
	    "1431E7F5623CB66B23346775D386B50E982C493ADBBFC54B9A3CD383"
	    "382336A1A0B2150A15358F336D03AE18F666C7573D55C4FD181C29E6"
	    "CCFDE63EA35F0ADF5885CFC0A3D84A2B2E4DD24496DB789E663170CE"
	    "F74798AA1BBCD4574EA0BBA40489D764B2F83AADC66B148B4A0CD952"
	    "46C127D5871C4F11418690A5DDF01246A0C80A43C70088B6183639DC"
	    "FDA4125BD113A8F49EE23ED306FAAC576C3FB0C1E256671D817FC253"
	    "4A52F5B439F72E424DE376F4C565CCA82307DD9EF76DA5B7C4EB7E08"
	    "5172E328807C02D011FFBF33785378D79DC266F6A5BE6BB0E4A92ECE"
	    "EBAEB1",
	    "34B77C7BD8E0A930C87A00D359C205DE7EAE358F85653A61E958B39B"
	    "D9953CDE91AD4625C1EC3128CCBFC0E9BDAE3DC817BA40E609C16AF8"
	    "01096EAC1EBF9551" },
};

static int
hex(const char *h, uint8_t *out)
{
	size_t i, n = strlen(h) / 2;

	for (i = 0; i < n; i++)
		sscanf(h + 2 * i, "%2hhx", &out[i]);
	return ((int)n);
}

int
main(void)
{
	uint8_t k1[64], k2[20], k3[200], k4[100];
	int i, bad = 0;

	for (i = 0; i < 64; i++)
		k1[i] = (uint8_t)i;
	for (i = 0; i < 20; i++)
		k2[i] = (uint8_t)(0x30 + i);
	for (i = 0; i < 200; i++)
		k3[i] = (uint8_t)(0x50 + i % 100);
	for (i = 0; i < 100; i++)
		k4[i] = (uint8_t)(0x50 + i);
	bad |= check(k1, 64, "Sample #1",
	    "8C646525C9845428125A7909D502263AF9FA28E269412CD0BF1E8F09E3C895B1"
	    "A09A1A1CD7B31A715CC1B29C6411FC10333A335102734A6DCF228D8845992117");
	bad |= check(k2, 20, "Sample #2",
	    "BA9F26F6272F7CD25C9426B6C122D687CFEEB1F00ED824BB28BF5EBDEF00F4AE"
	    "BA327936CEB78C86F83E0F8332380B1D370EE15CA4EF2292236CF712F73D4DDB");
	bad |= check(k3, 200, "The successful verification of a MAC does not "
	    "completely guarantee that the accompanying message is authentic.",
	    "91E00AA9FF2586AAF83398C0DB2DE92ED5DCD8B36FC4B85CC69B93CB4B364F6A"
	    "6D0F9889E0F420BCD32C63D73F48EE7C45F7AC24A1276878B307F713B4D82272");
	bad |= check(k4, 100, "The successful verification of a MAC does not "
	    "completely guarantee that the accompanying message is authentic: "
	    "there is a chance that a source with no knowledge of the key can "
	    "present a purported MAC.",
	    "2D018E26700751241B6B8C6EF7E68496482CFC12A0E66624848F41B564379E5E"
	    "38EFF5E5B2EC4E52C84E845E2A37E92372F656660FB10C030EA34ED4953F8C05");
	for (i = 0; i < 3; i++) {
		uint8_t m[256], d[64], want[64];
		int k;

		hex(kat[i].msg, m);
		hex(kat[i].md, want);
		edonr512(m, kat[i].len, d);
		for (k = 0; k < 64; k++)
			if (d[k] != want[k]) {
				printf("FAIL KAT len %zu\n", kat[i].len);
				bad = 1;
				break;
			}
	}
	printf(bad ? "=== Edon-R as submitted FAILED\n" :
	    "=== Edon-R512 as submitted: three KATs, four MAC examples\n");
	return (bad);
}
#else
int
main(void)
{
	static const char *msg[2] = { "abc",
	    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
	    "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu" };
	static const char *want[2] = {
	    "1b14db155f1d406594b8cef70a4362ec6b5de6a5daf50ec999e987c19d3049e2"
	    "de5977bb05b1bb220050a1ea5b46a9f1740acafbf6b45032adc90c628372c22b",
	    "5351070dc51c3b2baca5a60d0252ccb4e4921a96fe5a69e76dad48fd21a0845a"
	    "d57f880b3e4a907bc503151842bb949e1cba7439a6409a34b8436cb46921583c"
	};
	uint8_t d[64];
	char got[129];
	int i, k, bad = 0;

	for (i = 0; i < 2; i++) {
		edonr512(msg[i], strlen(msg[i]), d);
		for (k = 0; k < 64; k++)
			snprintf(got + k * 2, 3, "%02x", d[k]);
		if (strcmp(got, want[i]) != 0) {
			printf("FAIL %s\n  got  %s\n  want %s\n", msg[i], got,
			    want[i]);
			bad = 1;
		}
	}
	printf(bad ? "=== Edon-R as ZFS computes it FAILED\n" :
	    "=== Edon-R512 as ZFS computes it: OpenZFS's two digests\n");
	return (bad);
}
#endif
