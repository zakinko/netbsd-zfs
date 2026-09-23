# ZFS On-Disk Specification (Sun, 2006, Draft) — 第 6 章の写し

## Chapter Six: ZPL (ZFS POSIX Layer)

> The ZPL represents filesystems as an object set of type
> **DMU_OST_ZFS**. All snapshots, clones and filesystems are implemented
> as an object set of this type.

## §6.1 ZPL Filesystem Layout — **二つ目の入口**

> A ZPL object set has one object with a fixed location and fixed object
> number. This object is called the **"master node" and always has an
> object number of 1**. The master node is a ZAP object containing three
> attributes: DELETE_QUEUE, VERSION, and ROOT.

	DELETE_QUEUE	uint64	delete queue の object 番号
	VERSION		uint64	この版では 1
	ROOT		uint64	**root directory の object 番号**

**MOS の object directory も index 1、ZPL の master node も 1。**
どちらも「object set の 1 番」。

読み出しの道順はこれで閉じる:

	uberblock.ub_rootbp -> MOS
	MOS の object 1 (object directory, ZAP)
	  "root_dataset" -> root DSL directory の番号
	  dd_head_dataset_obj -> active dataset
	  ds_bp -> ZPL object set
	ZPL の object 1 (master node, ZAP)
	  "ROOT" -> root directory の object 番号
	そこから §6.2 で path を辿る

## §6.2 Directories and Directory Traversal

> Filesystem directories are implemented as ZAP objects (object type
> **DMU_OT_DIRECTORY**). Each directory holds a set of name-value pairs
> which contain the names and object numbers for each directory entry.
> Traversing through a directory tree is as simple as looking up the
> value for an entry and reading that object number.

**型名の食い違い:** 本文は `DMU_OT_DIRECTORY` と書くが、Table 8 にも
Table 13 にも在るのは `DMU_OT_DIRECTORY_CONTENTS` (20)。同じもの。

> All filesystem objects contain a **znode_phys_t** structure in the
> **bonus buffer** of its dnode.

	uint64_t	zp_atime[2];	/* 秒, ナノ秒 */
	uint64_t	zp_mtime[2];
	uint64_t	zp_ctime[2];
	uint64_t	zp_crtime[2];
	uint64_t	zp_gen;
	uint64_t	zp_mode;
	uint64_t	zp_size;	/* **ファイルのバイト数** */
	uint64_t	zp_parent;
	uint64_t	zp_links;
	uint64_t	zp_xattr;
	uint64_t	zp_rdev;
	uint64_t	zp_flags;
	uint64_t	zp_uid;
	uint64_t	zp_gid;
	uint64_t	zp_pad[4];
	zfs_znode_acl_t	zp_acl;

### zp_mode

> The lower 8 bits of the mode contain the access mode bits, for example
> 755. The 9th bit is the sticky bit ... **Bits 13-16 are used to
> designate the file type.**

Table 15 (bit 13-16 の値):

	S_IFIFO		0x1
	S_IFCHR		0x2
	S_IFDIR		0x4
	S_IFBLK		0x6
	S_IFREG		0x8
	S_IFLNK		0xA
	S_IFSOCK	0xC
	S_IFDOOR	0xD
	S_IFPORT	0xE

**この節の bit の数え方は信用しない。** 「下位 8 bit が 755」は
755 が八進で 9 bit 要ることと合わないし、「9 番目が sticky」も同様に
ずれている。値そのもの (0x4 = ディレクトリ, 0x8 = 通常ファイル) は
POSIX の S_IFMT を 12 bit 右へ寄せたものと一致するので、
**実装は `(zp_mode >> 12) & 0xf` を見る**。
出典は Table 15 の値のほうで、本文の bit 番号ではない。

Table 16 (zp_flags): ZFS_XATTR = 0x1、ZFS_INHERIT_ACE = 0x2。

## §6.3 ZFS Access Control Lists

	#define ACE_SLOT_CNT	6

	typedef struct zfs_znode_acl {
		uint64_t	z_acl_extern_obj;
		uint32_t	z_acl_count;
		uint16_t	z_acl_version;
		uint16_t	z_acl_pad;
		ace_t		z_ace_data[ACE_SLOT_CNT];
	} zfs_znode_acl_t;

	typedef struct ace {
		uid_t		a_who;
		uint32_t	a_access_mask;
		uint16_t	a_flags;
		uint16_t	a_type;
	} ace_t;

Table 17/18/19 は access mask、flag、ACE type の値。
**読み出しだけなら ACL は要らない。** znode の後ろに在ることだけ効く。

---

## この版で足りない所

	symlink の中身	§6.2 は S_IFLNK を型として挙げるだけで、
			**link 先をどこに置くかを書いていない**。
			現行は 長さが収まれば bonus buffer の znode の
			後ろ、収まらなければ object のデータ。
			出典を別に立てること
	System Attributes (SA)
			**これが一番効く。** 現行の ZPL version 5 は
			znode_phys_t を使わず、bonus buffer に SA
			(system attribute) の可変長の並びを置く。
			この版の znode_phys_t をそのまま当てると
			でたらめな値を読む。

			2026-09-23 に openzfs/zfs master を引いて確かめた
			(include/sys/sa_impl.h, include/sys/zfs_sa.h,
			 module/zfs/zfs_sa.c):

			  typedef struct sa_hdr_phys {
				uint32_t sa_magic;	/* 0x2F505A */
				uint16_t sa_layout_info;
				uint16_t sa_lengths[1];
			  } sa_hdr_phys_t;

			sa_layout_info は bit 0-9 が layout 番号、
			bit 10-15 が header の大きさ (その数 * 8 バイト)。
			**bonus の先頭 4 バイトが 0x2F505A なら SA**、
			でなければ古い znode_phys_t、という見分けが付く。
			master node の "VERSION" でも分かる (5 なら SA)。

			layout 番号 -> 属性の並び、は MOS の下の ZAP
			(object directory から "LAYOUTS"/"REGISTRY" を
			引く) に在るので、本来はそれを読む。

			ただし zfs_sa.h は **既定の layout での bonus 内
			offset** を直に持っている:

			  SA_MODE_OFFSET	0
			  SA_SIZE_OFFSET	8
			  SA_GEN_OFFSET		16
			  SA_UID_OFFSET		24
			  SA_GID_OFFSET		32
			  SA_PARENT_OFFSET	40
			  SA_FLAGS_OFFSET	48

			(sa_hdr_phys_t の後ろからの offset。属性の順は
			 zfs_attr_table の ZPL_ATIME..ZPL_GID)

			**読み出しに要るのは mode と size の二つだけ**なので、
			layout 番号が既定のものかを確かめた上でこの offset を
			使い、違えば LAYOUTS を引く、という段取りにできる。
			どちらにするかは実装に入るときに決める
	DELETE_QUEUE	現行の名前は "DELETE_QUEUE" ではなく
			"unlinked set" (Table 8 の 22 も同じ改名)。
			読み出しには要らない
	casesensitivity / normalization
			この版に無い。ZAP を引くときに名前を正規化する
			pool が在りうる (ZAP_FLAG_NORMALIZE)。
			**大文字小文字を畳む pool は読めない**と割り切るか、
			flag を見て断るか。断るほうを採る
