use std::env;

use grovedb_merk::Merk;
use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use grovedb_storage::Storage;
use grovedb_version::version::GroveVersion;
use rocksdb::{ColumnFamilyDescriptor, OptimisticTransactionDB, Options};

fn build_prefix(path: Vec<Vec<u8>>) -> [u8; 32] {
    let segments: Vec<&[u8]> = path.iter().map(|v| v.as_slice()).collect();
    let subtree = SubtreePath::from(segments.as_slice());
    RocksDbStorage::build_prefix(subtree).value
}

fn prefixed_key(prefix: [u8; 32], key: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(prefix.len() + key.len());
    out.extend_from_slice(&prefix);
    out.extend_from_slice(key);
    out
}

fn expect_get(
    db: &OptimisticTransactionDB<rocksdb::MultiThreaded>,
    cf: &std::sync::Arc<rocksdb::BoundColumnFamily<'_>>,
    key: Vec<u8>,
    expected: &[u8],
) {
    let value = db.get_cf(cf, key).expect("db get").expect("missing key");
    if value.as_slice() != expected {
        panic!("value mismatch");
    }
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let mode = env::args().nth(2).unwrap_or_default();
    if mode == "merk_incremental_save" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let transaction = storage.start_transaction();
        let subtree_refs: Vec<&[u8]> = vec![b"root"];
        let subtree = SubtreePath::from(subtree_refs.as_slice());
        let ctx = storage
            .get_transactional_storage_context(subtree, None, &transaction)
            .unwrap();
        let grove_version = GroveVersion::latest();
        let merk =
            Merk::open_base(
                ctx,
                grovedb_merk::tree_type::TreeType::NormalTree,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("open merk incremental_save");
        // Final state: k1=v1m, k2=v2m, k3=missing, k4=v4, k5=v5
        let v1 = merk
            .get(
                b"k1",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k1")
            .expect("missing k1");
        if v1.as_slice() != b"v1m" {
            panic!("incremental_save: k1 should be v1m, got {:?}", v1);
        }
        let v2 = merk
            .get(
                b"k2",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k2")
            .expect("missing k2");
        if v2.as_slice() != b"v2m" {
            panic!("incremental_save: k2 should be v2m, got {:?}", v2);
        }
        let k3 = merk
            .get(
                b"k3",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k3");
        if k3.is_some() {
            panic!("incremental_save: k3 should be deleted");
        }
        let v4 = merk
            .get(
                b"k4",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k4")
            .expect("missing k4");
        if v4.as_slice() != b"v4" {
            panic!("incremental_save: k4 should be v4, got {:?}", v4);
        }
        let v5 = merk
            .get(
                b"k5",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k5")
            .expect("missing k5");
        if v5.as_slice() != b"v5" {
            panic!("incremental_save: k5 should be v5, got {:?}", v5);
        }
        return;
    }
    if mode == "merk_multi_reopen" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let transaction = storage.start_transaction();
        let subtree_refs: Vec<&[u8]> = vec![b"root"];
        let subtree = SubtreePath::from(subtree_refs.as_slice());
        let ctx = storage
            .get_transactional_storage_context(subtree, None, &transaction)
            .unwrap();
        let grove_version = GroveVersion::latest();
        let merk =
            Merk::open_base(
                ctx,
                grovedb_merk::tree_type::TreeType::NormalTree,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("open merk multi_reopen");
        // Final state: only k3=v3 should exist (k1,k2 cleared in phase 2)
        let v3 = merk
            .get(
                b"k3",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k3")
            .expect("missing k3");
        if v3.as_slice() != b"v3" {
            panic!("merk_multi_reopen: k3 should be v3, got {:?}", v3);
        }
        let k1 = merk
            .get(
                b"k1",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k1");
        if k1.is_some() {
            panic!("merk_multi_reopen: k1 should be deleted");
        }
        let k2 = merk
            .get(
                b"k2",
                true,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("get k2");
        if k2.is_some() {
            panic!("merk_multi_reopen: k2 should be deleted");
        }
        return;
    }
    if mode == "merk"
        || mode == "merk_child"
        || mode == "merk_mut"
        || mode == "merk_child_mut"
        || mode == "merk_lifecycle"
        || mode == "merk_child_lifecycle"
        || mode == "merk_child_clear_lifecycle"
        || mode == "merk_tx_lifecycle"
        || mode == "merk_clear_reopen_lifecycle"
    {
        let subtree_segments: Vec<Vec<u8>> = if mode == "merk_child"
            || mode == "merk_child_mut"
            || mode == "merk_child_lifecycle"
            || mode == "merk_child_clear_lifecycle"
        {
            vec![b"root".to_vec(), b"child".to_vec()]
        } else {
            vec![b"root".to_vec()]
        };
        let is_child_clear_lifecycle = mode == "merk_child_clear_lifecycle";
        let is_clear_reopen_lifecycle = mode == "merk_clear_reopen_lifecycle";
        {
            let storage =
                RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
            let transaction = storage.start_transaction();
            let subtree_refs: Vec<&[u8]> = subtree_segments.iter().map(|v| v.as_slice()).collect();
            let subtree = SubtreePath::from(subtree_refs.as_slice());
            let ctx = storage
                .get_transactional_storage_context(subtree, None, &transaction)
                .unwrap();
            let grove_version = GroveVersion::latest();
            let merk = Merk::open_base(
                ctx,
                grovedb_merk::tree_type::TreeType::NormalTree,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("open merk");
            let is_mut = mode == "merk_mut" || mode == "merk_child_mut";
            let is_lifecycle = mode == "merk_lifecycle" || mode == "merk_child_lifecycle";
            let is_tx_lifecycle = mode == "merk_tx_lifecycle";
            if is_child_clear_lifecycle {
                let ck1 = merk
                    .get(
                        b"ck1",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get ck1");
                if ck1.is_some() {
                    panic!("ck1 should be missing after child clear lifecycle");
                }
                let ck2 = merk
                    .get(
                        b"ck2",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get ck2");
                if ck2.is_some() {
                    panic!("ck2 should be missing after child clear lifecycle");
                }
                let ck3 = merk
                    .get(
                        b"ck3",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get ck3")
                    .expect("missing ck3");
                if ck3.as_slice() != b"cv3" {
                    panic!("ck3 value mismatch");
                }
                // Parent subtree is verified below in the dedicated branch.
            } else {
            let v1_expected: &[u8] = if is_mut {
                b"v1x"
            } else if is_lifecycle {
                b"v1r"
            } else if is_tx_lifecycle {
                b"v1m"
            } else if is_clear_reopen_lifecycle {
                // For clear reopen lifecycle, k1 should be k10=v10 after reinsert
                b"v10"
            } else {
                b"v1"
            };
            let v1 = merk
                .get(
                    b"k1",
                    true,
                    None::<
                        &fn(
                            &[u8],
                            &GroveVersion,
                        )
                            -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                    >,
                    &grove_version,
                )
                .unwrap()
                .expect("get k1")
                .expect("missing k1");
            if v1.as_slice() != v1_expected {
                panic!("k1 value mismatch");
            }
            if is_mut {
                let k2 = merk
                    .get(
                        b"k2",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get k2");
                if k2.is_some() {
                    panic!("k2 should be missing");
                }
                let v3 = merk
                    .get(
                        b"k3",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get k3")
                    .expect("missing k3");
                if v3.as_slice() != b"v3" {
                    panic!("k3 value mismatch");
                }
            } else if is_lifecycle || is_tx_lifecycle {
                let k2 = merk
                    .get(
                        b"k2",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get k2");
                if k2.is_some() {
                    panic!("k2 should be missing after lifecycle reopen");
                }
                let v3 = merk
                    .get(
                        b"k3",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get k3")
                    .expect("missing k3");
                if v3.as_slice() != b"v3" {
                    panic!("k3 value mismatch");
                }
            } else {
                let v2 = merk
                    .get(
                        b"k2",
                        true,
                        None::<
                            &fn(
                                &[u8],
                                &GroveVersion,
                            )
                                -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                        >,
                        &grove_version,
                    )
                    .unwrap()
                    .expect("get k2")
                    .expect("missing k2");
                if v2.as_slice() != b"v2" {
                    panic!("k2 value mismatch");
                }
            }
            }
        }

        // For merk_child_clear_lifecycle, also verify parent subtree is intact
        if is_child_clear_lifecycle {
            let storage =
                RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
            let transaction = storage.start_transaction();
            let parent_segments: Vec<&[u8]> = vec![b"root" as &[u8]];
            let parent_subtree = SubtreePath::from(parent_segments.as_slice());
            let parent_ctx = storage
                .get_transactional_storage_context(parent_subtree, None, &transaction)
                .unwrap();
            let grove_version = GroveVersion::latest();
            let parent_merk = Merk::open_base(
                parent_ctx,
                grovedb_merk::tree_type::TreeType::NormalTree,
                None::<
                    &fn(
                        &[u8],
                        &GroveVersion,
                    )
                        -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                >,
                &grove_version,
            )
            .unwrap()
            .expect("open parent merk");

            // Parent should still have k1=v1, k2=v2
            let parent_k1 = parent_merk
                .get(
                    b"k1",
                    true,
                    None::<
                        &fn(
                            &[u8],
                            &GroveVersion,
                        )
                            -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                    >,
                    &grove_version,
                )
                .unwrap()
                .expect("get parent k1")
                .expect("parent k1 should exist");
            assert_eq!(parent_k1, b"v1".to_vec(), "parent k1 should be v1");

            let parent_k2 = parent_merk
                .get(
                    b"k2",
                    true,
                    None::<
                        &fn(
                            &[u8],
                            &GroveVersion,
                        )
                            -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>,
                    >,
                    &grove_version,
                )
                .unwrap()
                .expect("get parent k2")
                .expect("parent k2 should exist");
            assert_eq!(parent_k2, b"v2".to_vec(), "parent k2 should be v2");
        }

        let mut opts = Options::default();
        opts.create_if_missing(true);
        opts.create_missing_column_families(true);
        let cfs = vec![
            ColumnFamilyDescriptor::new("default", Options::default()),
            ColumnFamilyDescriptor::new("aux", Options::default()),
            ColumnFamilyDescriptor::new("roots", Options::default()),
            ColumnFamilyDescriptor::new("meta", Options::default()),
        ];
        let db: OptimisticTransactionDB<rocksdb::MultiThreaded> =
            OptimisticTransactionDB::open_cf_descriptors(&opts, &path, cfs).expect("open rocksdb");
        let cf_default = db.cf_handle("default").unwrap();
        let cf_roots = db.cf_handle("roots").unwrap();
        let prefix_root = build_prefix(subtree_segments.clone());
        let root_key = db
            .get_cf(&cf_roots, prefixed_key(prefix_root, b"r"))
            .expect("read root key")
            .expect("missing root key");
        let node_bytes = db
            .get_cf(
                &cf_default,
                prefixed_key(build_prefix(subtree_segments), &root_key),
            )
            .expect("read root node")
            .expect("missing root node");
        if node_bytes.is_empty() {
            panic!("root node bytes empty");
        }
        return;
    }
    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.create_missing_column_families(true);
    let cfs = vec![
        ColumnFamilyDescriptor::new("default", Options::default()),
        ColumnFamilyDescriptor::new("aux", Options::default()),
        ColumnFamilyDescriptor::new("roots", Options::default()),
        ColumnFamilyDescriptor::new("meta", Options::default()),
    ];
    let db: OptimisticTransactionDB<rocksdb::MultiThreaded> =
        OptimisticTransactionDB::open_cf_descriptors(&opts, &path, cfs).expect("open rocksdb");

    let cf_default = db.cf_handle("default").unwrap();
    let cf_aux = db.cf_handle("aux").unwrap();
    let cf_roots = db.cf_handle("roots").unwrap();
    let cf_meta = db.cf_handle("meta").unwrap();

    let prefix_root = build_prefix(vec![b"root".to_vec()]);
    let prefix_child = build_prefix(vec![b"root".to_vec(), b"child".to_vec()]);

    if mode == "iter" {
        let mut keys = Vec::new();
        let mut iter = db.raw_iterator_cf(&cf_default);
        iter.seek(prefix_root);
        while iter.valid() {
            let key = iter.key().expect("key");
            if !key.starts_with(&prefix_root) {
                break;
            }
            keys.push(key[prefix_root.len()..].to_vec());
            iter.next();
        }
        if keys != vec![b"k1".to_vec(), b"k2".to_vec()] {
            panic!("iterator keys mismatch");
        }
    } else if mode == "iter_rev" {
        let mut iter = db.raw_iterator_cf(&cf_default);
        let mut prefix_end = prefix_root.to_vec();
        for i in (0..prefix_end.len()).rev() {
            prefix_end[i] = prefix_end[i].wrapping_add(1);
            if prefix_end[i] != 0 {
                break;
            }
        }
        iter.seek_for_prev(prefix_end);
        let key = iter.key().expect("key");
        if !key.starts_with(&prefix_root) || key[prefix_root.len()..] != *b"k2" {
            panic!("reverse iter last key mismatch");
        }
        iter.prev();
        let key = iter.key().expect("key");
        if !key.starts_with(&prefix_root) || key[prefix_root.len()..] != *b"k1" {
            panic!("reverse iter prev key mismatch");
        }
    } else if mode == "merk" {
        expect_get(&db, &cf_default, prefixed_key(prefix_root, b"k1"), b"v1");
        expect_get(&db, &cf_default, prefixed_key(prefix_root, b"k2"), b"v2");
    } else {
        expect_get(&db, &cf_default, prefixed_key(prefix_root, b"k1"), b"v1");
        expect_get(&db, &cf_default, prefixed_key(prefix_child, b"k2"), b"v2");
        expect_get(&db, &cf_aux, prefixed_key(prefix_root, b"a1"), b"av1");
        expect_get(&db, &cf_roots, prefixed_key(prefix_root, b"r1"), b"rv1");
        expect_get(&db, &cf_meta, prefixed_key(prefix_root, b"m1"), b"mv1");
    }
}
