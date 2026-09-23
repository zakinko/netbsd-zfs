# ZFS On-Disk Specification (Sun, 2006, Draft) — 第 4 章の写し

## Chapter Four: DSL (Dataset and Snapshot Layer)

object set は四種: filesystem / clone / snapshot / volume。
DSL はその間の関係 (clone の親、snapshot の連なり、親子) を持つ。

## §4.1 DSL Infrastructure

> Each object set is represented in the DSL as a **dataset**.
> Datasets are grouped together hierarchically into collections called
> **Dataset Directories**.

> A DSL directory always has exactly **one "active dataset"**.

Illustration 13: DSL Directory の下に active dataset、そこから snapshot の
linked list。左に child ZAP、右に properties ZAP。各 dataset が DMU object
set を指す。

## §4.2 DSL Implementation Details — **読み出しの入口**

> The DSL is implemented as an object set of type **DMU_OST_META**. This
> object set is often called the **Meta Object Set, or MOS**. There is
> only one MOS per pool and the **uberblock points to it directly**.

> There is a single distinguished object in the Meta Object Set. This
> object is called the **object directory** and is always located in the
> **second element of the dnode array (index 1)**.

> All objects, with the exception of the object directory, can be located
> by traversing through a set of object references starting at this
> object.

**つまり道順は**

	uberblock.ub_rootbp  ->  objset_phys_t (os_type = DMU_OST_META)
	metadnode            ->  dnode の配列
	その配列の index 1   ->  object directory (ZAP)

### object directory の中身 (三対)

	root_dataset	uint64	root DSL directory の object 番号。
				type は DMU_OT_DSL_DIR
	config		uint64	DMU_OT_PACKED_NVLIST の object 番号。
				中身は §1.3.3 と同じ XDR nvlist
	sync_bplist	uint64	次の transaction で解放する blkptr の list

**注意:** 本文は `sync_bplist` の型を `DMU_OT_SYNC_BPLIST` と書くが、
Table 8 (§2.8) にその名前は無い。読み出しでは使わないので追わない。

Illustration 14 の実例値: `root_dataset = 2`, `config = 4`,
`sync_bplist = 1023`。**番号は固定ではない。ZAP を引いて得る。**

## §4.3 Dataset Internals

> Datasets are stored as an object of type **DMU_OT_DSL_DATASET**. This
> object type uses the **bonus buffer** in the dnode_phys_t to hold a
> **dsl_dataset_phys_t** structure.

	uint64_t ds_dir_obj		この dataset を参照する DSL directory
	uint64_t ds_prev_snap_obj	直前の snapshot。無ければ 0
	uint64_t ds_prev_snap_txg
	uint64_t ds_next_snap_obj	snapshot のときだけ
	uint64_t ds_snapnames_zapobj	snapshot 名 -> object 番号の ZAP
	uint64_t ds_num_children
	uint64_t ds_creation_time	1970-01-01 GMT からの秒
	uint64_t ds_creation_txg
	uint64_t ds_deadlist_obj
	uint64_t ds_used_bytes
	uint64_t ds_compressed_bytes
	uint64_t ds_uncompressed_bytes
	uint64_t ds_unique_bytes
	uint64_t ds_fsid_guid
	uint64_t ds_guid
	uint64_t ds_restoring
	blkptr_t ds_bp			**この dataset が表す object set の位置**

**読み出しに要るのは ds_bp だけ。** ここから objset_phys_t を読めば、
それが ZPL の object set (第 6 章)。

## §4.4 DSL Directory Internals

> The DSL Directory object contains a **dsl_dir_phys_t** structure in its
> bonus buffer.

	uint64_t dd_creation_time
	uint64_t dd_head_dataset_obj	**active dataset の object 番号**
	uint64_t dd_parent_obj
	uint64_t dd_clone_parent_obj
	uint64_t dd_child_dir_zapobj	**子 DSL directory の名前 -> 番号の ZAP**
	uint64_t dd_used_bytes
	uint64_t dd_compressed_bytes
	uint64_t dd_uncompressed_bytes
	uint64_t dd_quota
	uint64_t dd_reserved
	uint64_t dd_props_zapobj	局所設定された property だけの ZAP

> Only the **non-inherited / locally set** values are represented in this
> ZAP object. Default, inherited values are inferred when there is an
> absence of an entry.

### 道順のまとめ (dataset 名から object set へ)

	object directory["root_dataset"]	-> root DSL directory の番号
	その dnode の bonus = dsl_dir_phys_t
	  dd_child_dir_zapobj を名前で引く	-> 子 DSL directory の番号
	  (名前の各成分について繰り返す)
	dd_head_dataset_obj			-> active dataset の番号
	その dnode の bonus = dsl_dataset_phys_t
	  ds_bp					-> ZPL object set

Table 12 は property の一覧 (aclinherit .. zoned)。読み出しには要らない。
**ただし `mountpoint` はここに在る** ので、root filesystem を選ぶのに
使うなら出典はここ。

---

## この版で足りない所

	bootfs		pool の property で、どの dataset を root にするかを
			言う。**この版の Table 12 に無い** (Table 12 は
			DSL directory の property で、pool の property では
			ない)。出典を別に立てること
	dsl_dir_phys_t	現行 (OpenZFS include/sys/dsl_dir.h) は
			dd_props_zapobj の後に dd_deleg_zapobj, dd_flags,
			dd_used_breakdown[], dd_clones, dd_pad[13] が続く。
			**dd_props_zapobj までの欄の順と offset は同じ**で、
			dd_clone_parent_obj が dd_origin_obj に改名された
			だけ。dd_head_dataset_obj と dd_child_dir_zapobj は
			動いていないので、この版の定義で読める
	dsl_dataset_phys_t
			現行 (include/sys/dsl_dataset.h) は ds_restoring が
			ds_flags に、ds_used_bytes が ds_referenced_bytes に
			改名され、ds_bp の後ろに ds_next_clones_obj,
			ds_props_obj, ds_userrefs_obj, ds_pad[5] が足された。
			**足されたのは後ろだけで、ds_bp は同じ 16 語目**。
			この版の定義のまま ds_bp を読んでよい
			(2026-09-23 に openzfs/zfs master を引いて数えた)
