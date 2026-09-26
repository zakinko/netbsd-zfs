/*	$NetBSD$	*/

/*
 * Edon-R512, from "Cryptographic Hash Function EDON-R" (Gligoroski,
 * Ødegård, Mihova, Knapskog, Kocarev, Drápal, NTNU, October 2008), the
 * supporting documentation of the SHA-3 submission, for blocks written
 * with checksum=edonr.  The section, table and figure numbers in the
 * comments are that document's.
 *
 * [Z] edonr_zfs.c: ZFS keys it with the pool's 32 byte checksum salt
 * by hashing a 128 byte prefix ahead of the data, H(salt) || H(H(salt)),
 * and keeps the first 32 bytes of the 64 byte digest, copied into the
 * checksum's words as they are.  That construction is ZFS's own and not
 * part of Edon-R.
 *
 * Two things set what ZFS computes apart from the submission package
 * of October 2008, whose code and known answer tests say otherwise:
 *
 *  - The rotation amounts.  The document's Table 2.2 and Table 2.3 give
 *    (5, 15, 22, 31, 40, 50, 59) and (10, 19, 29, 36, 44, 48, 55); the
 *    package's reference code, and so its KATs and the MAC examples of
 *    §3.16, use (5, 19, 29, 31, 41, 57, 61) and (3, 17, 23, 31, 37, 45,
 *    59).  The copy of the document read here is the one archived in
 *    2012, which seems to have been revised; no record says when.  ZFS
 *    uses Table 2.2's.  With the package's instead this reproduces all
 *    256 byte aligned ShortMsgKAT_512 answers and the four MAC examples,
 *    which is how everything but the rotations was checked; compile with
 *    EDONR_SUBMISSION_ROTATIONS for that.
 *
 *  - The feedback.  Gligoroski's OFFICIAL COMMENT: EDON-R to NIST's
 *    hash-forum, 25 May 2009, replaced R(oldPipe, M) by
 *	R(oldPipe, M) xor oldPipe xor M', with M' = (M1, M0),
 *    the function later written EDON-R'.  ZFS computes that one.
 */

#include <sys/types.h>

#include "edonr.h"

#define	BLOCK	128		/* §1.4.2: 1024 bit blocks */

#define	ROTL(x, n)	(((x) << (n)) | ((x) >> (64 - (n))))

/* Table 1.6 */
static const uint64_t P0[16] = {
	0x8081828384858687ULL, 0x88898A8B8C8D8E8FULL,
	0x9091929394959697ULL, 0x98999A9B9C9D9E9FULL,
	0xA0A1A2A3A4A5A6A7ULL, 0xA8A9AAABACADAEAFULL,
	0xB0B1B2B3B4B5B6B7ULL, 0xB8B9BABBBCBDBEBFULL,
	0xC0C1C2C3C4C5C6C7ULL, 0xC8C9CACBCCCDCECFULL,
	0xD0D1D2D3D4D5D6D7ULL, 0xD8D9DADBDCDDDEDFULL,
	0xE0E1E2E3E4E5E6E7ULL, 0xE8E9EAEBECEDEEEFULL,
	0xF0F1F2F3F4F5F6F7ULL, 0xF8F9FAFBFCFDFEFFULL
};

/* Table 2.2's rotations, or the submission code's; see the top. */
#ifndef EDONR_SUBMISSION_ROTATIONS
static const uint8_t RX[8] = { 0, 5, 15, 22, 31, 40, 50, 59 };
static const uint8_t RY[8] = { 0, 10, 19, 29, 36, 44, 48, 55 };
#else
static const uint8_t RX[8] = { 0, 5, 19, 29, 31, 41, 57, 61 };
static const uint8_t RY[8] = { 0, 3, 17, 23, 31, 37, 45, 59 };
#endif

/* Table 2.2: the quasigroup operation of order 2^512, Z = X * Y. */
static void
q512(const uint64_t X[8], const uint64_t Y[8], uint64_t Z[8])
{
	uint64_t T[16];

	/* 1. */
	T[0] = 0xAAAAAAAAAAAAAAAAULL + X[0] + X[1] + X[2] + X[4] + X[7];
	T[1] = ROTL(X[0] + X[1] + X[3] + X[4] + X[7], RX[1]);
	T[2] = ROTL(X[0] + X[1] + X[4] + X[6] + X[7], RX[2]);
	T[3] = ROTL(X[2] + X[3] + X[5] + X[6] + X[7], RX[3]);
	T[4] = ROTL(X[1] + X[2] + X[3] + X[5] + X[6], RX[4]);
	T[5] = ROTL(X[0] + X[2] + X[3] + X[4] + X[5], RX[5]);
	T[6] = ROTL(X[0] + X[1] + X[5] + X[6] + X[7], RX[6]);
	T[7] = ROTL(X[2] + X[3] + X[4] + X[5] + X[6], RX[7]);
	/* 2. */
	T[8] = T[3] ^ T[5] ^ T[6];
	T[9] = T[2] ^ T[5] ^ T[6];
	T[10] = T[2] ^ T[3] ^ T[5];
	T[11] = T[0] ^ T[1] ^ T[4];
	T[12] = T[0] ^ T[4] ^ T[7];
	T[13] = T[1] ^ T[6] ^ T[7];
	T[14] = T[2] ^ T[3] ^ T[4];
	T[15] = T[0] ^ T[1] ^ T[7];
	/* 3. */
	T[0] = 0x5555555555555555ULL + Y[0] + Y[1] + Y[2] + Y[5] + Y[7];
	T[1] = ROTL(Y[0] + Y[1] + Y[3] + Y[4] + Y[6], RY[1]);
	T[2] = ROTL(Y[0] + Y[1] + Y[2] + Y[3] + Y[5], RY[2]);
	T[3] = ROTL(Y[2] + Y[3] + Y[4] + Y[6] + Y[7], RY[3]);
	T[4] = ROTL(Y[0] + Y[1] + Y[3] + Y[4] + Y[5], RY[4]);
	T[5] = ROTL(Y[2] + Y[4] + Y[5] + Y[6] + Y[7], RY[5]);
	T[6] = ROTL(Y[1] + Y[2] + Y[5] + Y[6] + Y[7], RY[6]);
	T[7] = ROTL(Y[0] + Y[3] + Y[4] + Y[6] + Y[7], RY[7]);
	/* 4. */
	Z[5] = T[8] + (T[3] ^ T[4] ^ T[6]);
	Z[6] = T[9] + (T[2] ^ T[5] ^ T[7]);
	Z[7] = T[10] + (T[4] ^ T[6] ^ T[7]);
	Z[0] = T[11] + (T[0] ^ T[1] ^ T[5]);
	Z[1] = T[12] + (T[2] ^ T[6] ^ T[7]);
	Z[2] = T[13] + (T[0] ^ T[1] ^ T[3]);
	Z[3] = T[14] + (T[0] ^ T[3] ^ T[4]);
	Z[4] = T[15] + (T[1] ^ T[2] ^ T[5]);
}

/* Definition 7: the reversed vector. */
static void
rev(const uint64_t X[8], uint64_t R[8])
{
	int i;

	for (i = 0; i < 8; i++)
		R[i] = X[7 - i];
}

/*
 * Definition 8 and Table 2.5a: R(C0, C1, A0, A1) = (B0, B1), with the
 * double pipe as C and the message block as A (Figure 2.1).  The
 * formula in Definition 8 bars A0 and A1 where the extracted text of
 * the document loses the bars; the rendered page shows them.
 */
static void
compress(uint64_t P[16], const uint8_t *blk)
{
	uint64_t A0[8], A1[8], X01[8], X11[8], X02[8], X12[8], X03[8],
	    X13[8], B0[8], B1[8], r[8];
	int i, k;

	/* §1.2: little endian, words 0-7 are M0 and 8-15 are M1. */
	for (i = 0; i < 16; i++) {
		uint64_t w = 0;

		for (k = 7; k >= 0; k--)
			w = (w << 8) | blk[i * 8 + k];
		if (i < 8)
			A0[i] = w;
		else
			A1[i - 8] = w;
	}

	rev(A1, r);
	q512(r, A0, X01);		/* X0(1) = rev(A1) * A0 */
	q512(X01, A1, X11);		/* X1(1) = X0(1) * A1 */
	q512(P + 8, X01, X02);		/* X0(2) = C0 * X0(1) */
	q512(X02, X11, X12);		/* X1(2) = X0(2) * X1(1) */
	q512(X02, P, X03);		/* X0(3) = X0(2) * C1 */
	q512(X12, X03, X13);		/* X1(3) = X1(2) * X0(3) */
	rev(A0, r);
	q512(r, X03, B0);		/* B0 = rev(A0) * X0(3) */
	q512(B0, X13, B1);		/* B1 = B0 * X1(3) */

	/*
	 * The 2009 feedback, R xor oldPipe xor (M1, M0): see the top.
	 *
	 * Figure 2.1 writes the outputs "P1 P0" under B0 and B1, and the
	 * next block takes P0 as C0.  P0 is the double pipe's less
	 * significant half, words 8-15 -- the n bits that §2.2 says are the
	 * digest, and that the figure circles -- so B0 becomes words 0-7,
	 * B1 words 8-15, and C0 is words 8-15.  Read the other way round
	 * the result matches none of the document's test examples.
	 */
	for (i = 0; i < 8; i++) {
#ifndef EDONR_SUBMISSION_ROTATIONS
		P[i] ^= A1[i] ^ B0[i];
		P[8 + i] ^= A0[i] ^ B1[i];
#else
		P[i] = B0[i];		/* as submitted, for the KATs */
		P[8 + i] = B1[i];
#endif
	}
}

/*
 * Table 2.6 over a whole buffer, with §1.4.1's padding: a 1 bit, zeros
 * to 960 mod 1024, and the length in bits as a 64 bit word -- the last
 * of the block, so little endian like the rest.  The digest is the 512
 * bits Figure 2.1 circles, P0 of the last block -- words 8-15 -- as
 * bytes.  pre, if not NULL, is one whole block hashed ahead of buf.
 */
static void
edonr_run(const uint8_t *pre, const void *buf, size_t len, uint8_t digest[64])
{
	uint64_t P[16], bits;
	const uint8_t *p = buf;
	uint8_t tail[2 * BLOCK];
	size_t n, rem;
	int i;

	for (i = 0; i < 16; i++)
		P[i] = P0[i];
	bits = (uint64_t)len * 8;
	if (pre != 0) {
		compress(P, pre);
		bits += BLOCK * 8;
	}
	for (n = 0; len - n >= BLOCK; n += BLOCK)
		compress(P, p + n);

	rem = len - n;
	for (i = 0; i < (int)rem; i++)
		tail[i] = p[n + i];
	tail[rem] = 0x80;
	n = (rem < BLOCK - 8) ? BLOCK : 2 * BLOCK;
	for (i = (int)rem + 1; i < (int)n - 8; i++)
		tail[i] = 0;
	for (i = 0; i < 8; i++)
		tail[n - 8 + i] = (uint8_t)(bits >> (8 * i));
	compress(P, tail);
	if (n == 2 * BLOCK)
		compress(P, tail + BLOCK);

	for (i = 0; i < 64; i++)
		digest[i] = (uint8_t)(P[8 + i / 8] >> (8 * (i % 8)));
}

void
edonr512(const void *buf, size_t len, uint8_t digest[64])
{

	edonr_run(0, buf, len, digest);
}

/*
 * What ZFS stores, from [Z] edonr_zfs.c: the salt is widened to a whole
 * 128 byte block, H(salt) || H(H(salt)) -- both 64 byte digests -- which
 * is hashed ahead of the data, and the first 32 bytes of the result are
 * the checksum.
 */
void
edonr_zfs(const uint8_t salt[32], const void *buf, size_t len,
    uint64_t out[4])
{
	uint8_t pre[BLOCK], d[64];
	int i, k;

	edonr512(salt, 32, pre);
	edonr512(pre, 64, pre + 64);
	edonr_run(pre, buf, len, d);
	for (i = 0; i < 4; i++) {
		out[i] = 0;
		for (k = 7; k >= 0; k--)
			out[i] = (out[i] << 8) | d[i * 8 + k];
	}
}
