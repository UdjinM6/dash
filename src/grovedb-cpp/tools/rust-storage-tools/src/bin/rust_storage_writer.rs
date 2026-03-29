use std::env;

use grovedb_element::Element;
use grovedb_merk::{Merk, Op, TreeFeatureType, TreeType};
use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use grovedb_storage::{Batch, RawIterator, Storage, StorageBatch, StorageContext};
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

fn log_tree_state(
    merk: &Merk<grovedb_storage::rocksdb_storage::PrefixedRocksDbTransactionContext>,
    label: &str,
) {
    let debug = std::env::var("DEBUG_PARITY").unwrap_or_default();
    if debug != "1" && debug != "true" {
        return;
    }
    let root_hash = merk.root_hash().value;
    eprintln!("[RUST {}] root_hash={:?}", label, root_hash);
}

fn write_merk_feature_variant(path: &str, mode: &str) {
    let (tree_type, uses_sum, mutate, delete_reinsert, multi_mut): (TreeType, bool, bool, bool, bool) = match mode {
        "merk_feature_basic" => (TreeType::NormalTree, false, false, false, false),
        "merk_feature_sum" => (TreeType::SumTree, true, false, false, false),
        "merk_feature_sum_mut" => (TreeType::SumTree, true, true, false, false),
        "merk_feature_sum_delete_reinsert" => (TreeType::SumTree, true, true, true, false),
        "merk_feature_big_sum" => (TreeType::BigSumTree, true, false, false, false),
        "merk_feature_big_sum_mut" => (TreeType::BigSumTree, true, true, false, false),
        "merk_feature_big_sum_delete_reinsert" => (TreeType::BigSumTree, true, true, true, false),
        "merk_feature_big_sum_multi_mut" => (TreeType::BigSumTree, true, true, true, true),
        "merk_feature_count" => (TreeType::CountTree, false, false, false, false),
        "merk_feature_count_mut" => (TreeType::CountTree, false, true, false, false),
        "merk_feature_count_sum" => (TreeType::CountSumTree, true, false, false, false),
        "merk_feature_count_sum_mut" => (TreeType::CountSumTree, true, true, false, false),
        "merk_feature_count_sum_delete_reinsert" => (TreeType::CountSumTree, true, true, true, false),
        "merk_feature_prov_count" => (TreeType::ProvableCountTree, false, false, false, false),
        "merk_feature_prov_count_mut" => (TreeType::ProvableCountTree, false, true, false, false),
        "merk_feature_prov_count_delete_reinsert" => {
            (TreeType::ProvableCountTree, false, true, true, false)
        }
        "merk_feature_prov_count_sum" => (TreeType::ProvableCountSumTree, true, false, false, false),
        "merk_feature_prov_count_sum_mut" => (TreeType::ProvableCountSumTree, true, true, false, false),
        "merk_feature_prov_count_sum_delete_reinsert" => {
            (TreeType::ProvableCountSumTree, true, true, true, false)
        }
        _ => panic!("unsupported merk feature mode: {mode}"),
    };

    let storage = RocksDbStorage::default_rocksdb_with_path(path).expect("open rocksdb storage");
    let batch = StorageBatch::new();
    let transaction = storage.start_transaction();
    let subtree_segments: Vec<&[u8]> = vec![b"root" as &[u8]];
    let subtree = SubtreePath::from(subtree_segments.as_slice());
    let ctx = storage
        .get_transactional_storage_context(subtree, Some(&batch), &transaction)
        .unwrap();
    let grove_version = GroveVersion::latest();
    let mut merk = Merk::open_base(
        ctx,
        tree_type,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open merk");

    log_tree_state(&merk, "INIT");

    let (v1, v2, s1, s2) = if uses_sum {
        (
            Element::new_sum_item(11)
                .serialize(&grove_version)
                .expect("serialize sum item 11"),
            Element::new_sum_item(22)
                .serialize(&grove_version)
                .expect("serialize sum item 22"),
            11_i64,
            22_i64,
        )
    } else {
        (
            Element::new_item(b"v1".to_vec())
                .serialize(&grove_version)
                .expect("serialize item v1"),
            Element::new_item(b"v2".to_vec())
                .serialize(&grove_version)
                .expect("serialize item v2"),
            0_i64,
            0_i64,
        )
    };

    let feature_for_sum = |sum: i64| match tree_type {
        TreeType::NormalTree => TreeFeatureType::BasicMerkNode,
        TreeType::SumTree => TreeFeatureType::SummedMerkNode(sum),
        TreeType::BigSumTree => TreeFeatureType::BigSummedMerkNode(sum as i128),
        TreeType::CountTree => TreeFeatureType::CountedMerkNode(1),
        TreeType::CountSumTree => TreeFeatureType::CountedSummedMerkNode(1, sum),
        TreeType::ProvableCountTree => TreeFeatureType::ProvableCountedMerkNode(1),
        TreeType::ProvableCountSumTree => TreeFeatureType::ProvableCountedSummedMerkNode(1, sum),
    };

    let ops1 = vec![(b"k1".to_vec(), Op::Put(v1, feature_for_sum(s1)))];
    merk.apply::<_, Vec<_>>(&ops1, &[], None, &grove_version)
        .unwrap()
        .expect("apply k1");
    log_tree_state(&merk, "AFTER_K1");
    let ops2 = vec![(b"k2".to_vec(), Op::Put(v2, feature_for_sum(s2)))];
    merk.apply::<_, Vec<_>>(&ops2, &[], None, &grove_version)
        .unwrap()
        .expect("apply k2");
    log_tree_state(&merk, "AFTER_K2");
    if mutate {
        let (v3, v4, s3, s4) = if uses_sum {
            (
                Element::new_sum_item(33)
                    .serialize(&grove_version)
                    .expect("serialize sum item 33"),
                Element::new_sum_item(44)
                    .serialize(&grove_version)
                    .expect("serialize sum item 44"),
                33_i64,
                44_i64,
            )
        } else {
            (
                Element::new_item(b"v3".to_vec())
                    .serialize(&grove_version)
                    .expect("serialize item v3"),
                Element::new_item(b"v4".to_vec())
                    .serialize(&grove_version)
                    .expect("serialize item v4"),
                0_i64,
                0_i64,
            )
        };
        let ops3 = vec![(b"k1".to_vec(), Op::Put(v3, feature_for_sum(s3)))];
        merk.apply::<_, Vec<_>>(&ops3, &[], None, &grove_version)
            .unwrap()
            .expect("apply k1 replace");
        log_tree_state(&merk, "AFTER_K1_REPLACE");
        let ops4 = vec![(b"k2".to_vec(), Op::Delete)];
        merk.apply::<_, Vec<_>>(&ops4, &[], None, &grove_version)
            .unwrap()
            .expect("apply k2 delete");
        log_tree_state(&merk, "AFTER_K2_DELETE");
        let ops5 = vec![(b"k3".to_vec(), Op::Put(v4, feature_for_sum(s4)))];
        merk.apply::<_, Vec<_>>(&ops5, &[], None, &grove_version)
            .unwrap()
            .expect("apply k3");
        log_tree_state(&merk, "AFTER_K3");
        if delete_reinsert {
            let (v5, s5) = if uses_sum {
                (
                    Element::new_sum_item(500)
                        .serialize(&grove_version)
                        .expect("serialize sum item 500"),
                    500_i64,
                )
            } else {
                (
                    Element::new_item(b"v5".to_vec())
                        .serialize(&grove_version)
                        .expect("serialize item v5"),
                    0_i64,
                )
            };
            let ops6 = vec![(b"k1".to_vec(), Op::Put(v5, feature_for_sum(s5)))];
            merk.apply::<_, Vec<_>>(&ops6, &[], None, &grove_version)
                .unwrap()
                .expect("apply k1 reinsert");
            log_tree_state(&merk, "AFTER_K1_REINSERT");

            // Guard this fixture mode with an explicit semantic check so we can
            // distinguish writer correctness from downstream parity-reader issues.
            if matches!(tree_type, TreeType::BigSumTree) {
                let k1_value = merk
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
                    .expect("read k1 after reinsert")
                    .expect("missing k1 after reinsert");
                let k3_value = merk
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
                    .expect("read k3 after reinsert")
                    .expect("missing k3 after reinsert");

                let parsed_k1 = Element::deserialize(&k1_value, &grove_version)
                    .expect("deserialize k1 after reinsert");
                let parsed_k3 = Element::deserialize(&k3_value, &grove_version)
                    .expect("deserialize k3 after reinsert");

                match parsed_k1 {
                    Element::SumItem(500, _) => {}
                    _ => panic!("expected k1=SumItem(500) after reinsert"),
                }
                match parsed_k3 {
                    Element::SumItem(44, _) => {}
                    _ => panic!("expected k3=SumItem(44) after reinsert"),
                }
            }

            // For multi_mut, add additional mutations to stress-test aggregate propagation
            if multi_mut {
                let (v6, v7, v8, s6, s7, s8) = if uses_sum {
                    (
                        Element::new_sum_item(100)
                            .serialize(&grove_version)
                            .expect("serialize sum item 100"),
                        Element::new_sum_item(200)
                            .serialize(&grove_version)
                            .expect("serialize sum item 200"),
                        Element::new_sum_item(75)
                            .serialize(&grove_version)
                            .expect("serialize sum item 75"),
                        100_i64,
                        200_i64,
                        75_i64,
                    )
                } else {
                    (
                        Element::new_item(b"v6".to_vec())
                            .serialize(&grove_version)
                            .expect("serialize item v6"),
                        Element::new_item(b"v7".to_vec())
                            .serialize(&grove_version)
                            .expect("serialize item v7"),
                        Element::new_item(b"v8".to_vec())
                            .serialize(&grove_version)
                            .expect("serialize item v8"),
                        0_i64,
                        0_i64,
                        0_i64,
                    )
                };
                // Insert k4
                let ops7 = vec![(b"k4".to_vec(), Op::Put(v6, feature_for_sum(s6)))];
                merk.apply::<_, Vec<_>>(&ops7, &[], None, &grove_version)
                    .unwrap()
                    .expect("apply k4");
                log_tree_state(&merk, "AFTER_K4");
                // Re-insert k2 (was deleted earlier)
                let ops8 = vec![(b"k2".to_vec(), Op::Put(v7, feature_for_sum(s7)))];
                merk.apply::<_, Vec<_>>(&ops8, &[], None, &grove_version)
                    .unwrap()
                    .expect("apply k2 reinsert");
                log_tree_state(&merk, "AFTER_K2_REINSERT");
                // Delete k4
                let ops9 = vec![(b"k4".to_vec(), Op::Delete)];
                merk.apply::<_, Vec<_>>(&ops9, &[], None, &grove_version)
                    .unwrap()
                    .expect("apply k4 delete");
                log_tree_state(&merk, "AFTER_K4_DELETE");
                // Replace k3
                let ops10 = vec![(b"k3".to_vec(), Op::Put(v8, feature_for_sum(s8)))];
                merk.apply::<_, Vec<_>>(&ops10, &[], None, &grove_version)
                    .unwrap()
                    .expect("apply k3 replace");
                log_tree_state(&merk, "AFTER_K3_REPLACE");

                // Guard multi_mut with semantic check for BigSumTree
                if matches!(tree_type, TreeType::BigSumTree) {
                    let k1_value = merk
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
                        .expect("read k1 after multi_mut")
                        .expect("missing k1 after multi_mut");
                    let k2_value = merk
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
                        .expect("read k2 after multi_mut")
                        .expect("missing k2 after multi_mut");
                    let k3_value = merk
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
                        .expect("read k3 after multi_mut")
                        .expect("missing k3 after multi_mut");

                    let parsed_k1 = Element::deserialize(&k1_value, &grove_version)
                        .expect("deserialize k1 after multi_mut");
                    let parsed_k2 = Element::deserialize(&k2_value, &grove_version)
                        .expect("deserialize k2 after multi_mut");
                    let parsed_k3 = Element::deserialize(&k3_value, &grove_version)
                        .expect("deserialize k3 after multi_mut");

                    match parsed_k1 {
                        Element::SumItem(500, _) => {}
                        _ => panic!("expected k1=SumItem(500) after multi_mut"),
                    }
                    match parsed_k2 {
                        Element::SumItem(200, _) => {}
                        _ => panic!("expected k2=SumItem(200) after multi_mut"),
                    }
                    match parsed_k3 {
                        Element::SumItem(75, _) => {}
                        _ => panic!("expected k3=SumItem(75) after multi_mut"),
                    }
                }
            }
        }
    }
    storage
        .commit_multi_context_batch(batch, None)
        .unwrap()
        .expect("commit batch");
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let mode = env::args().nth(2).unwrap_or_default();
    if mode == "tx_ctx_batch_visibility" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();
        let subtree_segments: Vec<&[u8]> = vec![b"root"];
        let subtree = SubtreePath::from(subtree_segments.as_slice());
        let ctx = storage
            .get_transactional_storage_context(subtree, Some(&batch), &transaction)
            .unwrap();

        ctx.put(b"k1", b"v1", None, None)
            .unwrap()
            .expect("put staged key");
        if ctx.get(b"k1").unwrap().expect("get staged key").is_some() {
            panic!("staged write should be hidden before batch commit");
        }

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit staged multi-context batch");

        let read_in_tx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        if read_in_tx
            .get(b"k1")
            .unwrap()
            .expect("tx get after batch commit")
            != Some(b"v1".to_vec())
        {
            panic!("staged write should be visible inside transaction after batch commit");
        }

        let other_transaction = storage.start_transaction();
        let read_other_tx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_transaction)
            .unwrap();
        if read_other_tx
            .get(b"k1")
            .unwrap()
            .expect("other tx get before commit")
            .is_some()
        {
            panic!("staged write leaked outside transaction before commit");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit transaction");

        let committed_reader = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_reader)
            .unwrap();
        if committed_ctx.get(b"k1").unwrap().expect("get after commit") != Some(b"v1".to_vec()) {
            panic!("committed staged write missing after transaction commit");
        }
        return;
    }
    if mode == "tx_ctx_batch_delete_visibility" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put(b"k_seed", b"v_seed", None, None)
            .unwrap()
            .expect("seed put");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed batch into tx");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed transaction");

        let transaction = storage.start_transaction();

        let delete_batch = StorageBatch::new();
        let delete_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&delete_batch),
                &transaction,
            )
            .unwrap();
        delete_ctx
            .delete(b"k_seed", None)
            .unwrap()
            .expect("stage delete");

        let before_batch_commit = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        if before_batch_commit
            .get(b"k_seed")
            .unwrap()
            .expect("get before delete batch commit")
            != Some(b"v_seed".to_vec())
        {
            panic!("staged delete should be hidden before batch commit");
        }

        storage
            .commit_multi_context_batch(delete_batch, Some(&transaction))
            .unwrap()
            .expect("commit delete batch into tx");

        let after_batch_commit = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        if after_batch_commit
            .get(b"k_seed")
            .unwrap()
            .expect("get after delete batch commit")
            .is_some()
        {
            panic!("committed delete batch should be visible in transaction");
        }

        let other_transaction = storage.start_transaction();
        let other_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_transaction)
            .unwrap();
        if other_ctx
            .get(b"k_seed")
            .unwrap()
            .expect("other tx get before outer tx commit")
            != Some(b"v_seed".to_vec())
        {
            panic!("delete should not leak outside transaction before outer commit");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit delete visibility transaction");

        let committed_reader = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_reader)
            .unwrap();
        if committed_ctx
            .get(b"k_seed")
            .unwrap()
            .expect("get after outer tx commit")
            .is_some()
        {
            panic!("delete should persist after outer transaction commit");
        }
        return;
    }
    if mode == "tx_ctx_no_batch_noop_multi_context" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let transaction = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_root_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &transaction,
            )
            .unwrap();
        let seed_other_ctx = storage
            .get_transactional_storage_context(
                [b"other"].as_ref().into(),
                Some(&seed_batch),
                &transaction,
            )
            .unwrap();
        seed_root_ctx
            .put(b"k_seed", b"v_seed", None, None)
            .unwrap()
            .expect("seed put root");
        seed_other_ctx
            .put_aux(b"a_seed", b"av_seed", None)
            .unwrap()
            .expect("seed put aux");
        storage
            .commit_multi_context_batch(seed_batch, Some(&transaction))
            .unwrap()
            .expect("commit seed batch into tx");

        let root_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        let other_ctx = storage
            .get_transactional_storage_context([b"other"].as_ref().into(), None, &transaction)
            .unwrap();

        root_ctx
            .put(b"k_new", b"v_new", None, None)
            .unwrap()
            .expect("no-batch put should not error");
        root_ctx
            .delete(b"k_seed", None)
            .unwrap()
            .expect("no-batch delete should not error");
        other_ctx
            .put_aux(b"a_new", b"av_new", None)
            .unwrap()
            .expect("no-batch put_aux should not error");
        other_ctx
            .delete_aux(b"a_seed", None)
            .unwrap()
            .expect("no-batch delete_aux should not error");

        if root_ctx
            .get(b"k_new")
            .unwrap()
            .expect("get no-batch put result")
            .is_some()
        {
            panic!("no-batch put should not be visible in transaction context");
        }
        if root_ctx
            .get(b"k_seed")
            .unwrap()
            .expect("get no-batch delete result")
            != Some(b"v_seed".to_vec())
        {
            panic!("no-batch delete should not remove seeded key");
        }
        if other_ctx
            .get_aux(b"a_new")
            .unwrap()
            .expect("get no-batch put_aux result")
            .is_some()
        {
            panic!("no-batch put_aux should not be visible in transaction context");
        }
        if other_ctx
            .get_aux(b"a_seed")
            .unwrap()
            .expect("get no-batch delete_aux result")
            != Some(b"av_seed".to_vec())
        {
            panic!("no-batch delete_aux should not remove seeded aux key");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit no-batch no-op transaction");
        return;
    }
    if mode == "tx_ctx_batch_cross_context_visibility" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put(b"k_seed", b"v_seed", None, None)
            .unwrap()
            .expect("seed put");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed batch into tx");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed transaction");

        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();

        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .put(b"k_new", b"v_new", None, None)
            .unwrap()
            .expect("put staged key in write context");

        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();

        let before_batch = read_ctx.get(b"k_new").unwrap().unwrap();
        if before_batch.is_some() {
            panic!("read context should not see staged writes before batch commit");
        }

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit staged multi-context batch");

        let after_batch = read_ctx.get(b"k_new").unwrap().unwrap();
        if after_batch != Some(b"v_new".to_vec()) {
            panic!("read context should see writes after batch commit");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit transaction");
        return;
    }
    if mode == "tx_ctx_batch_cross_context_delete_visibility" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put(b"k_seed", b"v_seed", None, None)
            .unwrap()
            .expect("seed put");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed batch into tx");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed transaction");

        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();

        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .delete(b"k_seed", None)
            .unwrap()
            .expect("stage delete in write context");

        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();

        if read_ctx
            .get(b"k_seed")
            .unwrap()
            .expect("get seeded key before batch commit")
            != Some(b"v_seed".to_vec())
        {
            panic!("read context should still see seeded key before batch commit");
        }

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit staged delete batch");

        if read_ctx
            .get(b"k_seed")
            .unwrap()
            .expect("get seeded key after batch commit")
            .is_some()
        {
            panic!("read context should see delete after batch commit");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit transaction");
        return;
    }
    if mode == "tx_ctx_batch_cross_context_aux_visibility" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put_aux(b"a_seed", b"av_seed", None)
            .unwrap()
            .expect("seed aux put");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed aux batch into tx");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed aux transaction");

        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();

        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .put_aux(b"a_new", b"av_new", None)
            .unwrap()
            .expect("put staged aux key in write context");

        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();

        let before_batch = read_ctx.get_aux(b"a_new").unwrap().unwrap();
        if before_batch.is_some() {
            panic!("read context should not see staged aux writes before batch commit");
        }

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit staged aux multi-context batch");

        let after_batch = read_ctx.get_aux(b"a_new").unwrap().unwrap();
        if after_batch != Some(b"av_new".to_vec()) {
            panic!("read context should see aux writes after batch commit");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit aux transaction");
        return;
    }
    if mode == "tx_ctx_batch_cross_context_roots_meta_visibility" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put_root(b"r_seed", b"rv_seed", None)
            .unwrap()
            .expect("seed roots put");
        seed_ctx
            .put_meta(b"m_seed", b"mv_seed", None)
            .unwrap()
            .expect("seed meta put");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed roots/meta batch into tx");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed roots/meta transaction");

        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();

        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .put_root(b"r_new", b"rv_new", None)
            .unwrap()
            .expect("stage roots put");
        write_ctx
            .delete_meta(b"m_seed", None)
            .unwrap()
            .expect("stage meta delete");

        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        if read_ctx
            .get_root(b"r_new")
            .unwrap()
            .expect("get roots before batch commit")
            .is_some()
        {
            panic!("read context should not see staged roots writes before batch commit");
        }
        if read_ctx
            .get_meta(b"m_seed")
            .unwrap()
            .expect("get meta before batch commit")
            != Some(b"mv_seed".to_vec())
        {
            panic!("read context should still see seeded meta key before batch commit");
        }

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit staged roots/meta multi-context batch");

        if read_ctx
            .get_root(b"r_new")
            .unwrap()
            .expect("get roots after batch commit")
            != Some(b"rv_new".to_vec())
        {
            panic!("read context should see roots write after batch commit");
        }
        if read_ctx
            .get_meta(b"m_seed")
            .unwrap()
            .expect("get meta after batch commit")
            .is_some()
        {
            panic!("read context should see meta delete after batch commit");
        }

        let other_transaction = storage.start_transaction();
        let other_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_transaction)
            .unwrap();
        if other_ctx
            .get_root(b"r_new")
            .unwrap()
            .expect("other tx get roots before outer tx commit")
            .is_some()
        {
            panic!("roots write should not leak outside transaction before outer commit");
        }
        if other_ctx
            .get_meta(b"m_seed")
            .unwrap()
            .expect("other tx get meta before outer tx commit")
            != Some(b"mv_seed".to_vec())
        {
            panic!("meta delete should not leak outside transaction before outer commit");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit roots/meta transaction");

        let committed_reader = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_reader)
            .unwrap();
        if committed_ctx
            .get_root(b"r_new")
            .unwrap()
            .expect("get roots after outer commit")
            != Some(b"rv_new".to_vec())
        {
            panic!("roots write should persist after transaction commit");
        }
        if committed_ctx
            .get_meta(b"m_seed")
            .unwrap()
            .expect("get meta after outer commit")
            .is_some()
        {
            panic!("meta delete should persist after transaction commit");
        }

        return;
    }
    if mode == "tx_ctx_iterator_staged_writes" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let seed_transaction = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_transaction,
            )
            .unwrap();
        seed_ctx
            .put(b"k1", b"v1", None, None)
            .unwrap()
            .expect("seed k1");
        seed_ctx
            .put(b"k3", b"v3", None, None)
            .unwrap()
            .expect("seed k3");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_transaction))
            .unwrap()
            .expect("commit seed batch");
        storage
            .commit_transaction(seed_transaction)
            .unwrap()
            .expect("commit seed transaction");

        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();

        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .put(b"k2", b"v2", None, None)
            .unwrap()
            .expect("stage k2 insert");
        write_ctx
            .delete(b"k3", None)
            .unwrap()
            .expect("stage k3 delete");
        write_ctx
            .put(b"k4", b"v4", None, None)
            .unwrap()
            .expect("stage k4 insert");

        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();

        let mut iter = read_ctx.raw_iter();
        iter.seek_to_first().value;

        let mut keys_before: Vec<Vec<u8>> = Vec::new();
        while iter.valid().value {
            keys_before.push(iter.key().value.unwrap().to_vec());
            iter.next().value;
        }
        drop(iter);
        if keys_before != vec![b"k1".to_vec(), b"k3".to_vec()] {
            panic!(
                "iterator before batch commit should only see committed keys: {:?}",
                keys_before
            );
        }

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit staged batch");

        let mut iter_after = read_ctx.raw_iter();
        iter_after.seek_to_first().value;

        let mut keys_after: Vec<Vec<u8>> = Vec::new();
        while iter_after.valid().value {
            keys_after.push(iter_after.key().value.unwrap().to_vec());
            iter_after.next().value;
        }
        drop(iter_after);
        if keys_after != vec![b"k1".to_vec(), b"k2".to_vec(), b"k4".to_vec()] {
            panic!(
                "iterator after batch commit should see staged writes: {:?}",
                keys_after
            );
        }

        let other_transaction = storage.start_transaction();
        let other_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_transaction)
            .unwrap();
        let mut other_iter = other_ctx.raw_iter();
        other_iter.seek_to_first().value;

        let mut other_keys: Vec<Vec<u8>> = Vec::new();
        while other_iter.valid().value {
            other_keys.push(other_iter.key().value.unwrap().to_vec());
            other_iter.next().value;
        }
        drop(other_iter);
        if other_keys != vec![b"k1".to_vec(), b"k3".to_vec()] {
            panic!(
                "iterator in other transaction should not see staged writes: {:?}",
                other_keys
            );
        }
        drop(other_ctx);
        drop(other_transaction);

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit transaction");

        let committed_reader = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_reader)
            .unwrap();
        let mut committed_iter = committed_ctx.raw_iter();
        committed_iter.seek_to_first().value;

        let mut committed_keys: Vec<Vec<u8>> = Vec::new();
        while committed_iter.valid().value {
            committed_keys.push(committed_iter.key().value.unwrap().to_vec());
            committed_iter.next().value;
        }
        drop(committed_iter);
        if committed_keys != vec![b"k1".to_vec(), b"k2".to_vec(), b"k4".to_vec()] {
            panic!(
                "iterator after commit should see persisted writes: {:?}",
                committed_keys
            );
        }

        return;
    }
    if mode == "tx_ctx_multi_batch_orchestration" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let transaction = storage.start_transaction();
        let shared_batch = StorageBatch::new();

        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        if read_ctx
            .get(b"k_new")
            .unwrap()
            .expect("get default before merge")
            .is_some()
        {
            panic!("default part should be hidden before merge");
        }
        if read_ctx
            .get_aux(b"a_new")
            .unwrap()
            .expect("get aux before merge")
            .is_some()
        {
            panic!("aux part should be hidden before merge");
        }

        let merge_default_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&shared_batch),
                &transaction,
            )
            .unwrap();
        let mut default_part = merge_default_ctx.new_batch();
        default_part
            .put(b"k_new", b"v_new", None, None)
            .expect("build default part");
        merge_default_ctx
            .commit_batch(default_part)
            .unwrap()
            .expect("merge default part");

        let merge_aux_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&shared_batch),
                &transaction,
            )
            .unwrap();
        let mut aux_part = merge_aux_ctx.new_batch();
        aux_part
            .put_aux(b"a_new", b"av_new", None)
            .expect("build aux part");
        merge_aux_ctx
            .commit_batch(aux_part)
            .unwrap()
            .expect("merge aux part");

        if read_ctx
            .get(b"k_new")
            .unwrap()
            .expect("get default after merge before shared commit")
            .is_some()
        {
            panic!("merged default part should stay hidden before shared batch commit");
        }
        if read_ctx
            .get_aux(b"a_new")
            .unwrap()
            .expect("get aux after merge before shared commit")
            .is_some()
        {
            panic!("merged aux part should stay hidden before shared batch commit");
        }

        storage
            .commit_multi_context_batch(shared_batch, Some(&transaction))
            .unwrap()
            .expect("commit merged shared batch");

        if read_ctx
            .get(b"k_new")
            .unwrap()
            .expect("get default after shared commit")
            != Some(b"v_new".to_vec())
        {
            panic!("default merged part should be visible after shared batch commit");
        }
        if read_ctx
            .get_aux(b"a_new")
            .unwrap()
            .expect("get aux after shared commit")
            != Some(b"av_new".to_vec())
        {
            panic!("aux merged part should be visible after shared batch commit");
        }

        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit multi-batch orchestration tx");
        return;
    }
    if mode == "tx_ctx_delete_prefix_visibility" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");

        // Seed: insert k1=v1, k2=v2, k3=v3, k4=v4
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put(b"k1", b"v1", None, None)
            .unwrap()
            .expect("seed k1");
        seed_ctx
            .put(b"k2", b"v2", None, None)
            .unwrap()
            .expect("seed k2");
        seed_ctx
            .put(b"k3", b"v3", None, None)
            .unwrap()
            .expect("seed k3");
        seed_ctx
            .put(b"k4", b"v4", None, None)
            .unwrap()
            .expect("seed k4");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed batch");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed tx");

        // Delete k2 and k3 individually (simulating prefix delete semantics)
        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();
        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .delete(b"k2", None)
            .unwrap()
            .expect("stage delete k2");
        write_ctx
            .delete(b"k3", None)
            .unwrap()
            .expect("stage delete k3");

        // Read context should still see all keys before batch commit
        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        if read_ctx.get(b"k2").unwrap().expect("get k2 before commit") != Some(b"v2".to_vec()) {
            panic!("k2 should be visible before batch commit");
        }
        if read_ctx.get(b"k3").unwrap().expect("get k3 before commit") != Some(b"v3".to_vec()) {
            panic!("k3 should be visible before batch commit");
        }

        // Commit batch
        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit batch with deletes");

        // After batch commit, k2 and k3 should be deleted
        if read_ctx
            .get(b"k2")
            .unwrap()
            .expect("get k2 after commit")
            .is_some()
        {
            panic!("k2 should be deleted after batch commit");
        }
        if read_ctx
            .get(b"k3")
            .unwrap()
            .expect("get k3 after commit")
            .is_some()
        {
            panic!("k3 should be deleted after batch commit");
        }
        // k1 and k4 should remain
        if read_ctx.get(b"k1").unwrap().expect("get k1 after commit") != Some(b"v1".to_vec()) {
            panic!("k1 should remain after deletes");
        }
        if read_ctx.get(b"k4").unwrap().expect("get k4 after commit") != Some(b"v4".to_vec()) {
            panic!("k4 should remain after deletes");
        }

        // Cross-transaction isolation: other tx should not see staged deletes before outer commit
        let other_tx = storage.start_transaction();
        let other_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_tx)
            .unwrap();
        if other_ctx.get(b"k2").unwrap().expect("other tx get k2") != Some(b"v2".to_vec()) {
            panic!("deletes should not leak to other tx before outer commit");
        }
        if other_ctx.get(b"k3").unwrap().expect("other tx get k3") != Some(b"v3".to_vec()) {
            panic!("deletes should not leak to other tx before outer commit");
        }

        // Commit outer transaction
        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit outer tx");

        // After outer commit, deletions should be persisted
        let committed_tx = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_tx)
            .unwrap();
        if committed_ctx
            .get(b"k2")
            .unwrap()
            .expect("get k2 after outer commit")
            .is_some()
        {
            panic!("k2 should remain deleted after outer commit");
        }
        if committed_ctx
            .get(b"k3")
            .unwrap()
            .expect("get k3 after outer commit")
            .is_some()
        {
            panic!("k3 should remain deleted after outer commit");
        }
        if committed_ctx
            .get(b"k1")
            .unwrap()
            .expect("get k1 after outer commit")
            != Some(b"v1".to_vec())
        {
            panic!("k1 should persist after outer commit");
        }
        if committed_ctx
            .get(b"k4")
            .unwrap()
            .expect("get k4 after outer commit")
            != Some(b"v4".to_vec())
        {
            panic!("k4 should persist after outer commit");
        }

        return;
    }
    if mode == "tx_ctx_aux_batch_composition" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");

        // Seed: put_aux a1=v1, a2=v2
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put_aux(b"a1", b"v1", None)
            .unwrap()
            .expect("seed a1");
        seed_ctx
            .put_aux(b"a2", b"v2", None)
            .unwrap()
            .expect("seed a2");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed batch");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed tx");

        // Compose batch: put_aux a3=v3, delete_aux a2
        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();
        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .put_aux(b"a3", b"v3", None)
            .unwrap()
            .expect("stage put_aux a3");
        write_ctx
            .delete_aux(b"a2", None)
            .unwrap()
            .expect("stage delete_aux a2");

        // Read context should see old state before batch commit
        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        if read_ctx
            .get_aux(b"a2")
            .unwrap()
            .expect("get a2 before commit")
            != Some(b"v2".to_vec())
        {
            panic!("a2 should be visible before batch commit");
        }
        if read_ctx
            .get_aux(b"a3")
            .unwrap()
            .expect("get a3 before commit")
            .is_some()
        {
            panic!("a3 should not be visible before batch commit");
        }

        // Commit batch
        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit batch with aux composition");

        // After batch commit: a2 deleted, a3 visible
        if read_ctx
            .get_aux(b"a2")
            .unwrap()
            .expect("get a2 after commit")
            .is_some()
        {
            panic!("a2 should be deleted after batch commit");
        }
        if read_ctx
            .get_aux(b"a3")
            .unwrap()
            .expect("get a3 after commit")
            != Some(b"v3".to_vec())
        {
            panic!("a3 should be visible after batch commit");
        }
        // a1 should remain
        if read_ctx
            .get_aux(b"a1")
            .unwrap()
            .expect("get a1 after commit")
            != Some(b"v1".to_vec())
        {
            panic!("a1 should remain after aux batch");
        }

        // Cross-transaction isolation
        let other_tx = storage.start_transaction();
        let other_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_tx)
            .unwrap();
        if other_ctx.get_aux(b"a2").unwrap().expect("other tx get a2") != Some(b"v2".to_vec()) {
            panic!("aux delete should not leak to other tx before outer commit");
        }
        if other_ctx
            .get_aux(b"a3")
            .unwrap()
            .expect("other tx get a3")
            .is_some()
        {
            panic!("aux put should not leak to other tx before outer commit");
        }

        // Commit outer transaction
        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit outer tx");

        // After outer commit, changes should be persisted
        let committed_tx = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_tx)
            .unwrap();
        if committed_ctx
            .get_aux(b"a2")
            .unwrap()
            .expect("get a2 after outer commit")
            .is_some()
        {
            panic!("a2 should remain deleted after outer commit");
        }
        if committed_ctx
            .get_aux(b"a3")
            .unwrap()
            .expect("get a3 after outer commit")
            != Some(b"v3".to_vec())
        {
            panic!("a3 should persist after outer commit");
        }
        if committed_ctx
            .get_aux(b"a1")
            .unwrap()
            .expect("get a1 after outer commit")
            != Some(b"v1".to_vec())
        {
            panic!("a1 should persist after outer commit");
        }

        return;
    }
    if mode == "tx_ctx_iterator_roots_meta_staged" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");

        // Seed: put_root r1=rv1, put_meta m1=mv1
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put_root(b"r1", b"rv1", None)
            .unwrap()
            .expect("seed root r1");
        seed_ctx
            .put_meta(b"m1", b"mv1", None)
            .unwrap()
            .expect("seed meta m1");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed batch");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed tx");

        // Stage: put_root r2=rv2, delete_meta m1
        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();
        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .put_root(b"r2", b"rv2", None)
            .unwrap()
            .expect("stage put_root r2");
        write_ctx
            .delete_meta(b"m1", None)
            .unwrap()
            .expect("stage delete_meta m1");

        // Read context before batch commit should see old state
        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();
        
        // r2 should not be visible before batch commit
        if read_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("get r2 before commit")
            .is_some()
        {
            panic!("r2 should not be visible before batch commit");
        }
        
        // m1 should still be visible before batch commit
        if read_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("get m1 before commit")
            != Some(b"mv1".to_vec())
        {
            panic!("m1 should be visible before batch commit");
        }

        // Commit batch
        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit batch with roots/meta staged");

        // After batch commit: r2 visible, m1 deleted
        if read_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("get r2 after batch commit")
            != Some(b"rv2".to_vec())
        {
            panic!("r2 should be visible after batch commit");
        }
        
        if read_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("get m1 after batch commit")
            .is_some()
        {
            panic!("m1 should be deleted after batch commit");
        }
        
        // r1 should remain
        if read_ctx
            .get_root(b"r1")
            .unwrap()
            .expect("get r1 after batch commit")
            != Some(b"rv1".to_vec())
        {
            panic!("r1 should remain after batch commit");
        }

        // Cross-transaction isolation: other tx should not see staged writes before outer commit
        let other_tx = storage.start_transaction();
        let other_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_tx)
            .unwrap();
        
        if other_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("other tx get r2 before outer commit")
            .is_some()
        {
            panic!("r2 should not leak to other tx before outer commit");
        }
        
        if other_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("other tx get m1 before outer commit")
            != Some(b"mv1".to_vec())
        {
            panic!("m1 delete should not leak to other tx before outer commit");
        }

        // Commit outer transaction
        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit outer tx");

        // After outer commit, changes should be persisted
        let committed_tx = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_tx)
            .unwrap();
        
        if committed_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("get r2 after outer commit")
            != Some(b"rv2".to_vec())
        {
            panic!("r2 should persist after outer commit");
        }
        
        if committed_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("get m1 after outer commit")
            .is_some()
        {
            panic!("m1 delete should persist after outer commit");
        }
        
        if committed_ctx
            .get_root(b"r1")
            .unwrap()
            .expect("get r1 after outer commit")
            != Some(b"rv1".to_vec())
        {
            panic!("r1 should persist after outer commit");
        }

        return;
    }
    if mode == "tx_ctx_iterator_over_staged_roots_meta" {
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");

        // Seed: put_root r1=rv1, put_meta m1=mv1
        let seed_tx = storage.start_transaction();
        let seed_batch = StorageBatch::new();
        let seed_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&seed_batch),
                &seed_tx,
            )
            .unwrap();
        seed_ctx
            .put_root(b"r1", b"rv1", None)
            .unwrap()
            .expect("seed root r1");
        seed_ctx
            .put_meta(b"m1", b"mv1", None)
            .unwrap()
            .expect("seed meta m1");
        storage
            .commit_multi_context_batch(seed_batch, Some(&seed_tx))
            .unwrap()
            .expect("commit seed batch");
        storage
            .commit_transaction(seed_tx)
            .unwrap()
            .expect("commit seed tx");

        // Stage: put_root r2=rv2, put_meta m2=mv2, delete_meta m1
        let transaction = storage.start_transaction();
        let batch = StorageBatch::new();
        let write_ctx = storage
            .get_transactional_storage_context(
                [b"root"].as_ref().into(),
                Some(&batch),
                &transaction,
            )
            .unwrap();
        write_ctx
            .put_root(b"r2", b"rv2", None)
            .unwrap()
            .expect("stage put_root r2");
        write_ctx
            .put_meta(b"m2", b"mv2", None)
            .unwrap()
            .expect("stage put_meta m2");
        write_ctx
            .delete_meta(b"m1", None)
            .unwrap()
            .expect("stage delete_meta m1");

        // Read context before batch commit - verify old state
        let read_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &transaction)
            .unwrap();

        // Before batch commit: r2 should not be visible
        if read_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("get r2 before commit")
            .is_some()
        {
            panic!("r2 should not be visible before batch commit");
        }

        // Before batch commit: m2 should not be visible
        if read_ctx
            .get_meta(b"m2")
            .unwrap()
            .expect("get m2 before commit")
            .is_some()
        {
            panic!("m2 should not be visible before batch commit");
        }

        // Before batch commit: m1 should still be visible
        if read_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("get m1 before commit")
            != Some(b"mv1".to_vec())
        {
            panic!("m1 should be visible before batch commit");
        }

        // Commit batch
        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("commit batch with roots/meta staged");

        // After batch commit: r2 should be visible
        if read_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("get r2 after batch commit")
            != Some(b"rv2".to_vec())
        {
            panic!("r2 should be visible after batch commit");
        }

        // After batch commit: m2 should be visible
        if read_ctx
            .get_meta(b"m2")
            .unwrap()
            .expect("get m2 after batch commit")
            != Some(b"mv2".to_vec())
        {
            panic!("m2 should be visible after batch commit");
        }

        // After batch commit: m1 should be deleted
        if read_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("get m1 after batch commit")
            .is_some()
        {
            panic!("m1 should be deleted after batch commit");
        }

        // Cross-transaction isolation: other tx should not see staged writes before outer commit
        let other_tx = storage.start_transaction();
        let other_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &other_tx)
            .unwrap();

        // Other tx: r2 should not be visible
        if other_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("other tx get r2 before outer commit")
            .is_some()
        {
            panic!("r2 should not leak to other tx before outer commit");
        }

        // Other tx: m2 should not be visible
        if other_ctx
            .get_meta(b"m2")
            .unwrap()
            .expect("other tx get m2 before outer commit")
            .is_some()
        {
            panic!("m2 should not leak to other tx before outer commit");
        }

        // Other tx: m1 should still be visible
        if other_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("other tx get m1 before outer commit")
            != Some(b"mv1".to_vec())
        {
            panic!("m1 delete should not leak to other tx before outer commit");
        }

        // Commit outer transaction
        storage
            .commit_transaction(transaction)
            .unwrap()
            .expect("commit outer tx");

        // After outer commit, changes should be persisted
        let committed_tx = storage.start_transaction();
        let committed_ctx = storage
            .get_transactional_storage_context([b"root"].as_ref().into(), None, &committed_tx)
            .unwrap();

        // After commit: r2 should persist
        if committed_ctx
            .get_root(b"r2")
            .unwrap()
            .expect("get r2 after outer commit")
            != Some(b"rv2".to_vec())
        {
            panic!("r2 should persist after outer commit");
        }

        // After commit: m2 should persist
        if committed_ctx
            .get_meta(b"m2")
            .unwrap()
            .expect("get m2 after outer commit")
            != Some(b"mv2".to_vec())
        {
            panic!("m2 should persist after outer commit");
        }

        // After commit: m1 delete should persist
        if committed_ctx
            .get_meta(b"m1")
            .unwrap()
            .expect("get m1 after outer commit")
            .is_some()
        {
            panic!("m1 delete should persist after outer commit");
        }

        // After commit: r1 should remain
        if committed_ctx
            .get_root(b"r1")
            .unwrap()
            .expect("get r1 after outer commit")
            != Some(b"rv1".to_vec())
        {
            panic!("r1 should persist after outer commit");
        }

        return;
    }
    if mode == "merk_incremental_save" {
        let subtree_segments: Vec<&[u8]> = vec![b"root" as &[u8]];
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let grove_version = GroveVersion::latest();

        // Phase 1: insert k1=v1, k2=v2, k3=v3
        {
            let batch = StorageBatch::new();
            let tx = storage.start_transaction();
            let subtree = SubtreePath::from(subtree_segments.as_slice());
            let ctx = storage
                .get_transactional_storage_context(subtree, Some(&batch), &tx)
                .unwrap();
            let mut merk = Merk::open_base(
                ctx,
                TreeType::NormalTree,
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
            .expect("open merk phase 1");
            let ops = vec![
                (
                    b"k1".to_vec(),
                    Op::Put(b"v1".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (
                    b"k2".to_vec(),
                    Op::Put(b"v2".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (
                    b"k3".to_vec(),
                    Op::Put(b"v3".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            merk.apply::<_, Vec<_>>(&ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply phase 1");
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit phase 1");
        }

        // Phase 2: reopen, replace k1=v1m, delete k3, insert k4=v4
        {
            let batch = StorageBatch::new();
            let tx = storage.start_transaction();
            let subtree = SubtreePath::from(subtree_segments.as_slice());
            let ctx = storage
                .get_transactional_storage_context(subtree, Some(&batch), &tx)
                .unwrap();
            let mut merk = Merk::open_base(
                ctx,
                TreeType::NormalTree,
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
            .expect("open merk phase 2");
            let ops = vec![
                (
                    b"k1".to_vec(),
                    Op::Put(b"v1m".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (b"k3".to_vec(), Op::Delete),
                (
                    b"k4".to_vec(),
                    Op::Put(b"v4".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            merk.apply::<_, Vec<_>>(&ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply phase 2");
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit phase 2");
        }

        // Phase 3: reopen, replace k2=v2m, insert k5=v5
        {
            let batch = StorageBatch::new();
            let tx = storage.start_transaction();
            let subtree = SubtreePath::from(subtree_segments.as_slice());
            let ctx = storage
                .get_transactional_storage_context(subtree, Some(&batch), &tx)
                .unwrap();
            let mut merk = Merk::open_base(
                ctx,
                TreeType::NormalTree,
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
            .expect("open merk phase 3");
            let ops = vec![
                (
                    b"k2".to_vec(),
                    Op::Put(b"v2m".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (
                    b"k5".to_vec(),
                    Op::Put(b"v5".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            merk.apply::<_, Vec<_>>(&ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply phase 3");
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit phase 3");
        }

        return;
    }
    if mode == "merk_multi_reopen" {
        let subtree_segments: Vec<&[u8]> = vec![b"root" as &[u8]];
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let grove_version = GroveVersion::latest();

        // Phase 1: insert k1=v1, k2=v2, commit
        {
            let batch = StorageBatch::new();
            let tx = storage.start_transaction();
            let subtree = SubtreePath::from(subtree_segments.as_slice());
            let ctx = storage
                .get_transactional_storage_context(subtree, Some(&batch), &tx)
                .unwrap();
            let mut merk = Merk::open_base(
                ctx,
                TreeType::NormalTree,
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
            .expect("open merk phase 1");
            let ops = vec![
                (
                    b"k1".to_vec(),
                    Op::Put(b"v1".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (
                    b"k2".to_vec(),
                    Op::Put(b"v2".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            merk.apply::<_, Vec<_>>(&ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply phase 1");
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit phase 1");
        }

        // Phase 2: reopen, clear, reopen, insert k3=v3, commit
        {
            let batch = StorageBatch::new();
            let tx = storage.start_transaction();
            let subtree = SubtreePath::from(subtree_segments.as_slice());
            let ctx = storage
                .get_transactional_storage_context(subtree, Some(&batch), &tx)
                .unwrap();
            let mut merk = Merk::open_base(
                ctx,
                TreeType::NormalTree,
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
            .expect("open merk phase 2");
            // Clear the tree
            merk.clear().unwrap().expect("clear merk");
            // Insert new key after clear
            let ops = vec![(
                b"k3".to_vec(),
                Op::Put(b"v3".to_vec(), TreeFeatureType::BasicMerkNode),
            )];
            merk.apply::<_, Vec<_>>(&ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply phase 2");
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit phase 2");
        }

        // Phase 3: reopen, verify only k3 exists (k1,k2 cleared)
        {
            let batch = StorageBatch::new();
            let tx = storage.start_transaction();
            let subtree = SubtreePath::from(subtree_segments.as_slice());
            let ctx = storage
                .get_transactional_storage_context(subtree, Some(&batch), &tx)
                .unwrap();
            let _merk = Merk::open_base(
                ctx,
                TreeType::NormalTree,
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
            .expect("open merk phase 3");
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
    {
        let subtree_segments: Vec<&[u8]> =
            if mode == "merk_child" || mode == "merk_child_mut" || mode == "merk_child_lifecycle" {
                vec![b"root" as &[u8], b"child" as &[u8]]
            } else {
                vec![b"root" as &[u8]]
            };
        let storage =
            RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
        let batch = StorageBatch::new();
        let transaction = storage.start_transaction();
        let subtree = SubtreePath::from(subtree_segments.as_slice());
        let ctx = storage
            .get_transactional_storage_context(subtree, Some(&batch), &transaction)
            .unwrap();
        let grove_version = GroveVersion::latest();
        let mut merk =
            Merk::open_base(
                ctx,
                TreeType::NormalTree,
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
        let ops1 = vec![(
            b"k1".to_vec(),
            Op::Put(b"v1".to_vec(), TreeFeatureType::BasicMerkNode),
        )];
        merk.apply::<_, Vec<_>>(&ops1, &[], None, &grove_version)
            .unwrap()
            .expect("apply k1");
        let ops2 = vec![(
            b"k2".to_vec(),
            Op::Put(b"v2".to_vec(), TreeFeatureType::BasicMerkNode),
        )];
        merk.apply::<_, Vec<_>>(&ops2, &[], None, &grove_version)
            .unwrap()
            .expect("apply k2");
        if mode == "merk_mut" || mode == "merk_child_mut" {
            let ops3 = vec![(
                b"k1".to_vec(),
                Op::Put(b"v1x".to_vec(), TreeFeatureType::BasicMerkNode),
            )];
            merk.apply::<_, Vec<_>>(&ops3, &[], None, &grove_version)
                .unwrap()
                .expect("apply k1 replace");
            let ops4 = vec![(b"k2".to_vec(), Op::Delete)];
            merk.apply::<_, Vec<_>>(&ops4, &[], None, &grove_version)
                .unwrap()
                .expect("apply k2 delete");
            let ops5 = vec![(
                b"k3".to_vec(),
                Op::Put(b"v3".to_vec(), TreeFeatureType::BasicMerkNode),
            )];
            merk.apply::<_, Vec<_>>(&ops5, &[], None, &grove_version)
                .unwrap()
                .expect("apply k3");
        } else if mode == "merk_tx_lifecycle" {
            // Phase 1 commit: k1=v1, k2=v2
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit tx lifecycle initial batch");

            // Phase 2: reopen in new tx, mutate (replace k1, delete k2, insert k3)
            let batch2 = StorageBatch::new();
            let tx2 = storage.start_transaction();
            let subtree2 = SubtreePath::from(subtree_segments.as_slice());
            let ctx2 = storage
                .get_transactional_storage_context(subtree2, Some(&batch2), &tx2)
                .unwrap();
            let mut merk2 = Merk::open_base(
                ctx2,
                TreeType::NormalTree,
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
            .expect("open merk for tx lifecycle phase 2");
            let ops_phase2 = vec![
                (
                    b"k1".to_vec(),
                    Op::Put(b"v1m".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (b"k2".to_vec(), Op::Delete),
                (
                    b"k3".to_vec(),
                    Op::Put(b"v3".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            merk2
                .apply::<_, Vec<_>>(&ops_phase2, &[], None, &grove_version)
                .unwrap()
                .expect("apply tx lifecycle phase 2 ops");
            storage
                .commit_multi_context_batch(batch2, None)
                .unwrap()
                .expect("commit tx lifecycle phase 2 batch");
            return;
        } else if mode == "merk_lifecycle" || mode == "merk_child_lifecycle" {
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit initial lifecycle batch");

            let transaction_clear = storage.start_transaction();
            let subtree_clear = SubtreePath::from(subtree_segments.as_slice());
            let ctx_clear = storage
                .get_transactional_storage_context(subtree_clear, None, &transaction_clear)
                .unwrap();
            let mut merk_clear = Merk::open_base(
                ctx_clear,
                TreeType::NormalTree,
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
            .expect("open merk for lifecycle clear");
            merk_clear.clear().unwrap().expect("clear lifecycle merk");

            let batch_reopen = StorageBatch::new();
            let transaction_reopen = storage.start_transaction();
            let subtree_reopen = SubtreePath::from(subtree_segments.as_slice());
            let ctx_reopen = storage
                .get_transactional_storage_context(
                    subtree_reopen,
                    Some(&batch_reopen),
                    &transaction_reopen,
                )
                .unwrap();
            let mut merk_reopen = Merk::open_base(
                ctx_reopen,
                TreeType::NormalTree,
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
            .expect("open merk after lifecycle clear");
            let ops_reopen = vec![
                (
                    b"k1".to_vec(),
                    Op::Put(b"v1r".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (
                    b"k3".to_vec(),
                    Op::Put(b"v3".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            merk_reopen
                .apply::<_, Vec<_>>(&ops_reopen, &[], None, &grove_version)
                .unwrap()
                .expect("apply lifecycle reopen ops");
            storage
                .commit_multi_context_batch(batch_reopen, None)
                .unwrap()
                .expect("commit lifecycle reopen batch");
            return;
        } else if mode == "merk_child_clear_lifecycle" {
            // Phase 1: create parent with k1=v1, k2=v2 and child with ck1=cv1, ck2=cv2
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit initial parent batch");

            let child_batch = StorageBatch::new();
            let tx_child = storage.start_transaction();
            let child_segments: Vec<&[u8]> = vec![b"root", b"child"];
            let child_subtree = SubtreePath::from(child_segments.as_slice());
            let child_ctx = storage
                .get_transactional_storage_context(child_subtree, Some(&child_batch), &tx_child)
                .unwrap();
            let mut child_merk = Merk::open_base(
                child_ctx,
                TreeType::NormalTree,
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
            .expect("open child merk");
            let child_ops = vec![
                (
                    b"ck1".to_vec(),
                    Op::Put(b"cv1".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (
                    b"ck2".to_vec(),
                    Op::Put(b"cv2".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            child_merk
                .apply::<_, Vec<_>>(&child_ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply child ops");
            storage
                .commit_multi_context_batch(child_batch, Some(&tx_child))
                .unwrap()
                .expect("commit child batch");
            tx_child.commit().unwrap();

            // Phase 2: clear child subtree
            let clear_batch = StorageBatch::new();
            let tx_clear = storage.start_transaction();
            let clear_subtree = SubtreePath::from(child_segments.as_slice());
            let clear_ctx = storage
                .get_transactional_storage_context(clear_subtree, Some(&clear_batch), &tx_clear)
                .unwrap();
            let mut clear_merk = Merk::open_base(
                clear_ctx,
                TreeType::NormalTree,
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
            .expect("open clear merk");
            clear_merk.clear().unwrap().expect("clear child merk");
            storage
                .commit_multi_context_batch(clear_batch, Some(&tx_clear))
                .unwrap()
                .expect("commit clear batch");
            tx_clear.commit().unwrap();

            // Phase 3: verify parent still has k1=v1, k2=v2 and child is empty
            let verify_batch = StorageBatch::new();
            let tx_verify = storage.start_transaction();

            // Verify parent
            let parent_subtree = SubtreePath::from(subtree_segments.as_slice());
            let parent_ctx = storage
                .get_transactional_storage_context(parent_subtree, Some(&verify_batch), &tx_verify)
                .unwrap();
            let mut parent_merk = Merk::open_base(
                parent_ctx,
                TreeType::NormalTree,
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
            .expect("open parent merk for verify");

            let parent_v1 = parent_merk
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
            assert_eq!(parent_v1, b"v1".to_vec(), "parent k1 should be v1");

            let parent_v2 = parent_merk
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
            assert_eq!(parent_v2, b"v2".to_vec(), "parent k2 should be v2");

            // Verify child is empty
            let verify_child_subtree = SubtreePath::from(child_segments.as_slice());
            let verify_child_ctx = storage
                .get_transactional_storage_context(verify_child_subtree, None, &tx_verify)
                .unwrap();
            let mut verify_child_merk = Merk::open_base(
                verify_child_ctx,
                TreeType::NormalTree,
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
            .expect("open child merk for verify");

            let child_ck1 = verify_child_merk
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
                .expect("get child ck1");
            assert!(child_ck1.is_none(), "child ck1 should be cleared");

            let child_ck2 = verify_child_merk
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
                .expect("get child ck2");
            assert!(child_ck2.is_none(), "child ck2 should be cleared");

            // Re-insert into child and verify parent still intact
            let reinsert_batch = StorageBatch::new();
            let tx_reinsert = storage.start_transaction();
            let reinsert_subtree = SubtreePath::from(child_segments.as_slice());
            let reinsert_ctx = storage
                .get_transactional_storage_context(
                    reinsert_subtree,
                    Some(&reinsert_batch),
                    &tx_reinsert,
                )
                .unwrap();
            let mut reinsert_merk = Merk::open_base(
                reinsert_ctx,
                TreeType::NormalTree,
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
            .expect("open child merk for reinsert");
            let reinsert_ops = vec![(
                b"ck3".to_vec(),
                Op::Put(b"cv3".to_vec(), TreeFeatureType::BasicMerkNode),
            )];
            reinsert_merk
                .apply::<_, Vec<_>>(&reinsert_ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply child reinsert ops");
            storage
                .commit_multi_context_batch(reinsert_batch, Some(&tx_reinsert))
                .unwrap()
                .expect("commit reinsert batch");
            tx_reinsert.commit().unwrap();

            // Final verify: parent intact, child has ck3=cv3
            let final_tx = storage.start_transaction();
            let final_parent_subtree = SubtreePath::from(subtree_segments.as_slice());
            let final_parent_ctx = storage
                .get_transactional_storage_context(final_parent_subtree, None, &final_tx)
                .unwrap();
            let mut final_parent_merk = Merk::open_base(
                final_parent_ctx,
                TreeType::NormalTree,
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
            .expect("open parent merk for final verify");

            let final_parent_v1 = final_parent_merk
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
                .expect("get final parent k1")
                .expect("final parent k1 should exist");
            assert_eq!(
                final_parent_v1,
                b"v1".to_vec(),
                "final parent k1 should still be v1"
            );

            let final_child_subtree = SubtreePath::from(child_segments.as_slice());
            let final_child_ctx = storage
                .get_transactional_storage_context(final_child_subtree, None, &final_tx)
                .unwrap();
            let mut final_child_merk = Merk::open_base(
                final_child_ctx,
                TreeType::NormalTree,
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
            .expect("open child merk for final verify");

            let final_child_ck3 = final_child_merk
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
                .expect("get final child ck3")
                .expect("final child ck3 should exist");
            assert_eq!(
                final_child_ck3,
                b"cv3".to_vec(),
                "final child ck3 should be cv3"
            );

            return;
        } else if mode == "merk_clear_reopen_lifecycle" {
            // Phase 1: create merk with k1=v1, k2=v2, k3=v3
            storage
                .commit_multi_context_batch(batch, None)
                .unwrap()
                .expect("commit initial batch");

            // Phase 2: clear the subtree
            let clear_batch = StorageBatch::new();
            let tx_clear = storage.start_transaction();
            let clear_subtree = SubtreePath::from(subtree_segments.as_slice());
            let clear_ctx = storage
                .get_transactional_storage_context(clear_subtree, Some(&clear_batch), &tx_clear)
                .unwrap();
            let mut clear_merk = Merk::open_base(
                clear_ctx,
                TreeType::NormalTree,
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
            .expect("open merk for clear");
            clear_merk.clear().unwrap().expect("clear merk");
            storage
                .commit_multi_context_batch(clear_batch, Some(&tx_clear))
                .unwrap()
                .expect("commit clear batch");
            tx_clear.commit().unwrap();

            // Phase 3: reopen cleared merk and verify it's empty
            let verify_empty_batch = StorageBatch::new();
            let tx_verify_empty = storage.start_transaction();
            let verify_empty_subtree = SubtreePath::from(subtree_segments.as_slice());
            let verify_empty_ctx = storage
                .get_transactional_storage_context(
                    verify_empty_subtree,
                    Some(&verify_empty_batch),
                    &tx_verify_empty,
                )
                .unwrap();
            let mut verify_empty_merk = Merk::open_base(
                verify_empty_ctx,
                TreeType::NormalTree,
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
            .expect("open merk after clear");

            // Verify all keys are gone
            let v1 = verify_empty_merk
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
                .expect("get k1 after clear");
            assert!(v1.is_none(), "k1 should be cleared");

            let v2 = verify_empty_merk
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
                .expect("get k2 after clear");
            assert!(v2.is_none(), "k2 should be cleared");

            let v3 = verify_empty_merk
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
                .expect("get k3 after clear");
            assert!(v3.is_none(), "k3 should be cleared");

            // Verify root hash is empty tree hash (will check after reinsert_batch is defined)
            let empty_root_hash_after_clear = verify_empty_merk.root_hash().unwrap();

            storage
                .commit_multi_context_batch(verify_empty_batch, Some(&tx_verify_empty))
                .unwrap()
                .expect("commit verify empty batch");
            tx_verify_empty.commit().unwrap();

            // Phase 4: re-insert new data (k10=v10, k20=v20) and verify
            let reinsert_batch = StorageBatch::new();
            let tx_reinsert = storage.start_transaction();
            let reinsert_subtree = SubtreePath::from(subtree_segments.as_slice());
            let reinsert_ctx = storage
                .get_transactional_storage_context(
                    reinsert_subtree.clone(),
                    Some(&reinsert_batch),
                    &tx_reinsert,
                )
                .unwrap();
            let mut reinsert_merk = Merk::open_base(
                reinsert_ctx,
                TreeType::NormalTree,
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
            .expect("open merk for reinsert");

            // Verify empty root hash matches expected empty tree hash
            let empty_ctx_for_verify = storage
                .get_transactional_storage_context(
                    reinsert_subtree,
                    Some(&reinsert_batch),
                    &tx_reinsert,
                )
                .unwrap();
            let expected_merk = Merk::open_base(
                empty_ctx_for_verify,
                TreeType::NormalTree,
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
            .expect("open empty merk");
            let expected_empty_hash = expected_merk.root_hash().unwrap();
            assert_eq!(
                empty_root_hash_after_clear, expected_empty_hash,
                "root hash after clear should match empty tree hash"
            );

            let reinsert_ops = vec![
                (
                    b"k10".to_vec(),
                    Op::Put(b"v10".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
                (
                    b"k20".to_vec(),
                    Op::Put(b"v20".to_vec(), TreeFeatureType::BasicMerkNode),
                ),
            ];
            reinsert_merk
                .apply::<_, Vec<_>>(&reinsert_ops, &[], None, &grove_version)
                .unwrap()
                .expect("apply reinsert ops");
            storage
                .commit_multi_context_batch(reinsert_batch, Some(&tx_reinsert))
                .unwrap()
                .expect("commit reinsert batch");
            tx_reinsert.commit().unwrap();

            // Phase 5: final verify - old keys gone, new keys present
            let final_batch = StorageBatch::new();
            let tx_final = storage.start_transaction();
            let final_subtree = SubtreePath::from(subtree_segments.as_slice());
            let final_ctx = storage
                .get_transactional_storage_context(final_subtree, Some(&final_batch), &tx_final)
                .unwrap();
            let mut final_merk = Merk::open_base(
                final_ctx,
                TreeType::NormalTree,
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
            .expect("open merk for final verify");

            // Old keys should be gone
            let old_v1 = final_merk
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
                .expect("get old k1");
            assert!(old_v1.is_none(), "old k1 should be gone");

            // New keys should be present
            let new_v10 = final_merk
                .get(
                    b"k10",
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
                .expect("get new k10");
            assert_eq!(new_v10, Some(b"v10".to_vec()), "new k10 should be v10");

            let new_v20 = final_merk
                .get(
                    b"k20",
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
                .expect("get new k20");
            assert_eq!(new_v20, Some(b"v20".to_vec()), "new k20 should be v20");

            return;
        }

        storage
            .commit_multi_context_batch(batch, None)
            .unwrap()
            .expect("commit merk batch");
        return;
    }
    if mode == "merk_feature_basic"
        || mode == "merk_feature_sum"
        || mode == "merk_feature_sum_mut"
        || mode == "merk_feature_sum_delete_reinsert"
        || mode == "merk_feature_big_sum"
        || mode == "merk_feature_big_sum_mut"
        || mode == "merk_feature_big_sum_delete_reinsert"
        || mode == "merk_feature_big_sum_multi_mut"
        || mode == "merk_feature_count"
        || mode == "merk_feature_count_mut"
        || mode == "merk_feature_count_sum"
        || mode == "merk_feature_count_sum_mut"
        || mode == "merk_feature_count_sum_delete_reinsert"
        || mode == "merk_feature_prov_count"
        || mode == "merk_feature_prov_count_mut"
        || mode == "merk_feature_prov_count_delete_reinsert"
        || mode == "merk_feature_prov_count_sum"
        || mode == "merk_feature_prov_count_sum_mut"
        || mode == "merk_feature_prov_count_sum_delete_reinsert"
    {
        write_merk_feature_variant(&path, &mode);
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

    db.put_cf(&cf_default, prefixed_key(prefix_root, b"k1"), b"v1")
        .unwrap();
    db.put_cf(&cf_default, prefixed_key(prefix_root, b"k2"), b"v2")
        .unwrap();
    db.put_cf(&cf_default, prefixed_key(prefix_child, b"k2"), b"v2")
        .unwrap();
    db.put_cf(&cf_aux, prefixed_key(prefix_root, b"a1"), b"av1")
        .unwrap();
    db.put_cf(&cf_roots, prefixed_key(prefix_root, b"r1"), b"rv1")
        .unwrap();
    db.put_cf(&cf_meta, prefixed_key(prefix_root, b"m1"), b"mv1")
        .unwrap();
}
