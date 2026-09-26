# RFC 8878 (Zstandard) — 読み出しに要る節の写し

出典: [R] RFC 8878, *Zstandard Compression and the 'application/zstd'
Media Type*, February 2021 (errata 6441, 6442, 7297 を含めて読む)。
RFC 9659 はこれを更新しているが、HTTP で使うときの窓の上限 (8MB) を
決めるだけで、復号の手順は変えない。

ZFS がこの frame をどう包むかは RFC の外なので [Z] (OpenZFS
module/zstd/zfs_zstd.c と include/sys/zstd/zstd.h) から引く。

2026-09-27 に写した。

## [Z] ZFS の包み方

	uint32_t c_len			ビッグエンディアン。frame のバイト数
	uint32_t raw_version_level	ビッグエンディアン。版と圧縮レベル。
					復号には使わない (zfs_zstd.c 自身も
					"We ignore the ZSTD version")
	frame				c_len バイト

**frame は "magicless"。** zfs_zstd.c は圧縮も伸長も
`ZSTD_f_zstd1_magicless` を指定する。§3.1.1 の Magic_Number
(0xFD2FB528、4 バイト) を省いた形で、**RFC はこの形を定義していない**。
frame は Frame_Header から始まる。

圧縮側は `ZSTD_c_checksumFlag` と `ZSTD_c_contentSizeFlag` を 0 にして
いる。だが復号側で当てにはしない (RFC どおり両方読めるようにする)。

`ZIO_COMPRESS_ZSTD` は 16 (include/sys/zio_compress.h)。

## §3.1.1.1 Frame_Header

	Frame_Header_Descriptor		1 byte
	[Window_Descriptor]		0-1
	[Dictionary_ID]			0-4
	[Frame_Content_Size]		0-8

FHD (Table 3):

	7-6	Frame_Content_Size_Flag
	5	Single_Segment_Flag
	4	unused (解釈しない)
	3	reserved (**0 でなければならない。decoder は確かめる**)
	2	Content_Checksum_Flag
	1-0	Dictionary_ID_Flag

FCS_Field_Size (Table 4): flag 0 → Single_Segment なら 1、でなければ 0。
1 → 2、2 → 4、3 → 8。**2 バイトのときだけ 256 を足す** (§3.1.1.1.4)。
リトルエンディアン。

DID_Field_Size (Table 5): 0, 1, 2, 4。

Window_Descriptor (§3.1.1.1.2) は Single_Segment のとき無い。

	windowLog = 10 + Exponent (bit 7-3)
	Window_Size = (1 << windowLog) + ((1 << windowLog) / 8) * Mantissa

Content_Checksum (§3.1.1): flag が立っていれば frame の末尾に 4 バイト
(XXH64 の下位 32 bit)。

## §3.1.1.2 Blocks

Block_Header は 3 バイト、リトルエンディアン (Table 9):

	bit 0		Last_Block
	bit 1-2		Block_Type	0 Raw, 1 RLE, 2 Compressed, 3 Reserved
	bit 3-23	Block_Size

RLE の Block_Size は繰り返す回数、中身は 1 バイト。Reserved は壊れた
データとして**拒まなければならない**。

§3.1.1.2.4: Block_Maximum_Size = min(Window_Size, 128 KB)。伸長後の
大きさにも圧縮後の大きさにも掛かる。

## §3.1.1.3 Compressed Blocks

Literals_Section → Sequences_Section。復号に要るもの: それまでの出力
(窓)、"recent offsets"、直前の Huffman 表 (Treeless 用)、直前の FSE 表
(Repeat_Mode 用)。**どれも frame の中で block をまたいで持ち越す。**

### §3.1.1.3.1.1 Literals_Section_Header

Literals_Block_Type = byte0 & 3 (Table 13): 0 Raw, 1 RLE, 2 Compressed,
3 Treeless。

Raw/RLE の Size_Format = (byte0 >> 2) & 3:

	00, 10	1 byte、Regenerated_Size = byte0 >> 3
	01	2 bytes、(byte0 >> 4) + (byte1 << 4)
	11	3 bytes、(byte0 >> 4) + (byte1 << 4) + (byte2 << 12)

Compressed/Treeless は 2 bit の Size_Format:

	00	1 stream、10 bit ずつ、3 bytes
	01	4 streams、10 bit ずつ、3 bytes
	10	4 streams、14 bit ずつ、4 bytes
	11	4 streams、18 bit ずつ、5 bytes

Regenerated_Size が先、Compressed_Size が後 (どちらも 4 bit 目から
詰めて並ぶ)。Compressed_Size は Huffman_Tree_Description を含む。

**errata 7297 (Verified):** 4 streams の値域は 0 からではなく 6 から。
Jump_Table が 6 バイトあるので、6 未満だと Stream4_Size の計算が負に
なる。decoder は確かめる。

### §3.1.1.3.1.6 Jump_Table

4 streams のときだけ。6 バイト、2 バイトずつ LE で Stream1..3 の大きさ。

	Stream4_Size = Total_Streams_Size - 6 - S1 - S2 - S3

S1+S2+S3 が Total_Streams_Size を超えたら壊れている。各 stream の伸長後
の大きさは (Regenerated_Size + 3) / 4、最後だけ残り。

### §3.1.1.3.2.1 Sequences_Section_Header

Number_of_Sequences:

	byte0 == 0	sequence 無し。FSE の Repeat 表は**更新しない**
	byte0 < 128	byte0
	byte0 < 255	((byte0 - 128) << 8) + byte1
	byte0 == 255	byte1 + (byte2 << 8) + 0x7F00

Symbol_Compression_Modes (Table 14): 7-6 LL、5-4 OF、3-2 ML、1-0 は 0
でなければならない。Mode (Table 15): 0 Predefined、1 RLE (1 バイトの
記号)、2 FSE_Compressed (表の記述が続く)、3 Repeat。

FSE_Compressed の Accuracy_Log の上限: LL と ML は 9、OF は 8。
表は LL、OF、ML の順に並ぶ (§3.1.1.3.2 の図)。

### §3.1.1.3.2.1.1 Codes

Literals length (Table 16): 0-15 は値そのもの、bit 0。

	16 16/1  17 18/1  18 20/1  19 22/1  20 24/2  21 28/2
	22 32/3  23 40/3  24 48/4  25 64/6  26 128/7 27 256/8
	28 512/9 29 1024/10 30 2048/11 31 4096/12 32 8192/13
	33 16384/14 34 32768/15 35 65536/16

Match length (Table 17): 0-31 は code + 3、bit 0。

	32 35/1  33 37/1  34 39/1  35 41/1  36 43/2  37 47/2
	38 51/3  39 59/3  40 67/4  41 83/4  42 99/5  43 131/7
	44 259/8 45 515/9 46 1027/10 47 2051/11 48 4099/12
	49 8195/13 50 16387/14 51 32771/15 52 65539/16

Offset: code は読む bit 数そのもの。

	Offset_Value = (1 << code) + readNBits(code)

N の上限は decoder が決めてよい (22 以上を推奨)。

### §3.1.1.3.2.1.2 Decoding Sequences

bitstream は後ろから読む。最後のバイトの最上位の 1 は終端の印で、その上
の 0 と一緒に読み飛ばす。最初に LL、OF、ML の順で初期状態を読む。

各 sequence: **offset の追加 bit、ML の追加 bit、LL の追加 bit の順**に
読む。最後の sequence でなければ、**LL、ML、OF の順**に状態を更新する。
終わったとき bitstream が丁度使い切られていなければ壊れている。

### §3.1.1.3.2.2 Default Distributions

	LL  AL 6  { 4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,
	            2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1, -1,-1,-1,-1 }
	ML  AL 6  { 1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,
	            1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
	            1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,
	            -1,-1,-1,-1,-1 }
	OF  AL 5  { 1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,
	            1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1 }   (N ≤ 28)

Appendix A にこれらから作った復号表がある。**errata 6441 (Verified):**
Appendix A の三つの表は state 0 の行が二重になっていて、先の全部 0 の行
は誤り。

## §3.1.1.4 Sequence Execution, §3.1.1.5 Repeat Offsets

literals_length バイトを literals から出力へ写し、次に match_length
バイトを offset だけ前から写す (重なってよい)。offset は Window_Size
より小さくなければならない。sequence を全部実行したら、残った literals
を末尾に足す。

Offset_Value > 3 → offset = Offset_Value - 3。1..3 は繰り返し:

	literals_length != 0:	1 → Rep1、2 → Rep2、3 → Rep3
	literals_length == 0:	1 → Rep2、2 → Rep3、3 → Rep1 - 1

初期値 Rep1 = 1、Rep2 = 4、Rep3 = 8。Compressed_Block 以外は履歴に
関わらない。更新:

	繰り返しでない (Offset_Value > 3、または 3 で ll == 0):
		Rep3 = Rep2、Rep2 = Rep1、Rep1 = 使った offset
	繰り返し: 使った Rep を先頭へ、その前にあったものを一つずつ後ろへ

**errata 6442 (Verified):** Table 18 の下から二行目の offset_value は
1 ではなく 3。本文の規則のほうが正しい。(errata 8085 は Reported のまま
で、同じ表の最後の行に触れている。本文に従う。)

## §4.1 FSE

表は 2^Accuracy_Log 個の (Symbol, Num_Bits, Baseline)。初期状態は
Accuracy_Log bit を読む。次の状態は Num_Bits を読んで Baseline に足す。

### §4.1.1 FSE Table Description

前から読む LE の bitstream。Accuracy_Log = (byte0 の下位 4 bit) + 5。

記号 0 から順に値を読む。残り = (1 << AL) - 配った分。読める値は
0 .. 残り + 1。その範囲を表せる bit 数を n とすると、小さい値は n-1 bit
で済む (Table 20 の例: 0..157 なら 0..97 は 7 bit、98..157 は 8 bit)。
P = 値 - 1。-1 は「1 未満」で、配った分としては 1 と数える。

P == 0 の後には 2 bit の繰り返し数 (0..3) が続き、3 ならさらに 2 bit
... と続く。配った分が 1 << AL に達したら終わり、超えたら壊れている。
使ったバイト数は切り上げ。期待される記号数と違えば壊れている。

表の作り方:

	"1 未満" の記号は表の末尾から一つずつ後ろ向きに一マス
	残りの記号は記号 0、位置 0 から、確率の数だけマスを配る。次の位置は
		position += (tableSize >> 1) + (tableSize >> 3) + 3
		position &= tableSize - 1
	"1 未満" の記号が居るマスは飛ばす

Num_Bits と Baseline: 記号ごとにその記号のマスを番号順に並べる。確率 p
の次の 2 の冪を P2 とすると、下位の P2 - p 個は bit が 1 つ多い。
Baseline は bit の少ない上位のマスから順に、続いて先頭に戻って割り当てる
(Table 21 の例)。

## §4.2 Huffman

§4.2.1: 最大の符号長は **11 bit**。Weight から Number_of_Bits:

	Weight == 0 → 0、それ以外 → Max_Number_of_Bits + 1 - Weight

最後の記号の Weight は書かれていない。2^(Weight-1) の和を次の 2 の冪ま
で補う差から求める。その冪が 2^Max_Number_of_Bits。

§4.2.1.1: headerByte < 128 → Weight の列は FSE で headerByte バイト。
≥ 128 → 4 bit ずつ直に、記号数 = headerByte - 127、**上位 4 bit が先**。

§4.2.1.2: FSE の AL は最大 6。二つの状態が同じ表を共有し、State1 が
偶数番、State2 が奇数番。State1 を先に初期化。交互に一つ復号して状態を
更新する。状態の更新に残り以上の bit が要るときは、足りない bit を 0 と
して読み、**それぞれの最後の状態の記号を出して終わる**。Weight は最大
255 個。

§4.2.1.3: Weight の小さい順、同じ Weight の中では記号の自然な順に、符号
を小さい方から配る (Table 25)。

**Table 25 と Table 26 が食い違う。** Table 25 は Weight 1 の記号 4 と
5 に 0000 と 0001 を、Table 26 は 5 に 0000、4 に 0001 を当てている。
本文 ("Within the same Weight, symbols keep natural sequential order")
は Table 25 の側。実装は本文に従い、実データで確かめる。

§4.2.2: 各 stream は後ろから読む。最後のバイトの最上位の 1 は印。上位
Max_Number_of_Bits bit を覗いて表を引き、その記号の bit 数だけ進める。
**丁度使い切らなければ壊れている。**

## 写さなかった節

§3.1.2 Skippable Frames (ZFS の block は frame 一つ)、§5 と §6 の
Dictionary (ZFS は使わない。Dictionary_ID が立っていたら断る)、§7 IANA。
