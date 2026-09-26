# SHA-256 以降の checksum — 出典と、仕様書が言わなかったこと

2026-09-27 に書いた。[S] §2.4 の表は SHA-256 (8) まで。[Z] zio.h は
11 sha512、12 skein、13 edonr、14 blake3 を足している。それぞれ自分の
仕様書から書き、ZFS の使い方だけを [Z] から引いた。

## 仕様書

	sha512	FIPS 180-4 (2015) §4.1.3, §4.2.3, §5.1.2, §5.3.6.2,
		§6.4.2, §6.7。例は NIST の SHA512_256.pdf
	skein	The Skein Hash Function Family 1.3 (2010-10-01)
		§3.3 Threefish, §3.4 UBI, §3.5.1 Table 6, §3.5.2 Table 7,
		§3.5.3, §3.5.5。例は Appendix C.2 と B.8
	blake3	BLAKE3: one function, fast everywhere (2020)
		§2.1-§2.6, Table 1-3。例は BLAKE3-team の
		test_vectors/test_vectors.json
	edonr	Cryptographic Hash Function EDON-R (NTNU, 2008-10) の
		SHA-3 提出文書 (2012 年に archive された版)、
		Gligoroski の NIST hash-forum OFFICIAL COMMENT
		(2009-05-25)。例は提出パッケージ EDON-R.zip の
		KAT_MCT と文書 §3.16 の MAC 例

## ZFS の使い方 ([Z])

	sha512	SHA-512/256 を鍵なしで。digest のバイトをそのまま
		checksum の語に写す (sha256 だけは big endian で読む)
	skein	Skein-512、出力 256 bit、pool の salt (32 バイト) を
		鍵にした §3.5.5 の MAC。出力のバイトをそのまま
	edonr	Edon-R512 の前に H(salt) || H(H(salt)) の 128 バイトを
		流し、64 バイトの digest の先頭 32 バイト
	blake3	§2.3 の keyed_hash、鍵は salt、既定の 32 バイト出力

salt は MOS の object directory の "org.illumos:checksum_salt"、1 バイト
整数 32 個の fat ZAP 値。reader の ZAP 検索は 64 bit 整数一つしか返せな
かったので、配列を返す口を足した。

## 書かれていないこと、食い違っていたこと

**Edon-R の回転量。** 文書の Table 2.2 と 2.3 は (5,15,22,31,40,50,59)
と (10,19,29,36,44,48,55)。同じ提出パッケージの参照実装は (5,19,29,31,
41,57,61) と (3,17,23,31,37,45,59) で、パッケージの KAT も文書 §3.16 の
MAC 例もこちらで作られている。前者で書くと KAT も MAC 例も全部外れ、
後者に差し替えると ShortMsgKAT_512 の byte 単位の 256 件と MAC 例 4 件が
全部合う。ZFS (OpenZFS) は Table 2.2 の値。読んだ文書は 2012 年の
archive なので改訂版らしいが、いつ変わったかの記録は見つけていない。

**Edon-R の tweak。** OpenZFS は R の出力を古い pipe と M' = (M1, M0) で
xor する。これは 2008 年の文書に無く、Gligoroski の 2009-05-25 の投稿が
出典 (後に EDON-R' と書かれるもの)。

**Edon-R の pipe の半分。** Figure 2.1 は出力を B0 B1 の下に "P1 P0" と
書き、P0 を次の C0 にし、最後の P0 を digest として丸で囲む。§2.2 は
digest を「最下位 n bit」と言う。P0 を words 8-15 と読むと全部が整合
し、KAT とも合う。words 0-7 と読むと何とも合わない。

**Edon-R の Definition 8 の上線。** pdftotext で抜いた文には Ā0 と Ā1 の
上線が無い (Definition 7 の逆順ベクトル)。描画したページで確かめた。

**BLAKE3 Table 3 と FIPS 180-4 (4.12), (4.13)、Skein Table 3 と 4。**
いずれも文字抽出で上付きや段組が崩れた。描画したページで数え直した。

## 確かめたこと

	sha512	NIST の例 2 件、hashlib と 0..299 バイト全部と 4 件
	skein	C.2 の 3 件と B.8 の IV (Skein-512-256 の構成)
	blake3	test_vectors.json の 35 長 x (hash, keyed) = 70 件
	edonr	上記の KAT 256 件と MAC 例 4 件 (提出版の回転量で)、
		OpenZFS の試験の digest 2 件 (ZFS の形で)

四つとも OpenZFS 2.4.1 が checksum=... で書いた pool のファイルが
ZFS 本体と同じ SHA-256 で読め、zdb が各 33-35 本の block pointer を
その checksum で数えた。鍵付きの三つはここで初めて鍵の使い方まで確かめ
られた。
