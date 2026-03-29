use std::env;
use std::fs;
use std::path::Path;

use grovedb_merk::proofs::Query;
use grovedb_merk::{Merk, Op, TreeFeatureType, TreeType};
use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use grovedb_storage::{Storage, StorageBatch};
use grovedb_version::version::GroveVersion;

fn write_file(path: &Path, name: &str, data: &[u8]) {
    let out_path = path.join(name);
    fs::write(out_path, data).expect("write output file");
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let db_path = Path::new(&path);

    let storage = RocksDbStorage::default_rocksdb_with_path(&path).expect("open rocksdb storage");
    let batch = StorageBatch::new();
    let transaction = storage.start_transaction();
    let subtree = SubtreePath::from(&[b"root" as &[u8]]);
    let ctx = storage
        .get_transactional_storage_context(subtree, Some(&batch), &transaction)
        .unwrap();
    let grove_version = GroveVersion::latest();
    let mut merk = Merk::open_base(
        ctx,
        TreeType::NormalTree,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open merk");

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
        .expect("apply ops");
    storage
        .commit_multi_context_batch(batch, None)
        .unwrap()
        .expect("commit batch");

    let transaction = storage.start_transaction();
    let ctx = storage
        .get_transactional_storage_context(
            SubtreePath::from(&[b"root" as &[u8]]),
            None,
            &transaction,
        )
        .unwrap();
    let merk = Merk::open_base(
        ctx,
        TreeType::NormalTree,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open merk");

    let present_query = Query::new_single_key(b"k2".to_vec());
    let present_proof = merk
        .prove(present_query, None, &grove_version)
        .unwrap()
        .expect("present proof");
    let absent_query = Query::new_single_key(b"k0".to_vec());
    let absent_proof = merk
        .prove(absent_query, None, &grove_version)
        .unwrap()
        .expect("absent proof");
    let mut range_query = Query::default();
    range_query.left_to_right = true;
    range_query.insert_range_inclusive(b"k1".to_vec()..=b"k3".to_vec());
    let range_proof = merk
        .prove(range_query, None, &grove_version)
        .unwrap()
        .expect("range proof");
    let mut left_edge_boundary_range_query = Query::default();
    left_edge_boundary_range_query.left_to_right = true;
    left_edge_boundary_range_query.insert_range_inclusive(b"k0".to_vec()..=b"k1".to_vec());
    let left_edge_boundary_range_proof = merk
        .prove(left_edge_boundary_range_query, None, &grove_version)
        .unwrap()
        .expect("left-edge boundary range proof");
    let mut right_edge_absence_range_query = Query::default();
    right_edge_absence_range_query.left_to_right = true;
    right_edge_absence_range_query.insert_range_inclusive(b"k4".to_vec()..=b"k5".to_vec());
    let right_edge_absence_range_proof = merk
        .prove(right_edge_absence_range_query, None, &grove_version)
        .unwrap()
        .expect("right-edge absence range proof");
    let mut range_exclusive_end_query = Query::default();
    range_exclusive_end_query.left_to_right = true;
    range_exclusive_end_query.insert_range(b"k1".to_vec()..b"k3".to_vec());
    let range_exclusive_end_proof = merk
        .prove(range_exclusive_end_query, None, &grove_version)
        .unwrap()
        .expect("range exclusive-end proof");
    let present_leftmost_query = Query::new_single_key(b"k1".to_vec());
    let present_leftmost_proof = merk
        .prove(present_leftmost_query, None, &grove_version)
        .unwrap()
        .expect("present leftmost proof");
    let present_rightmost_query = Query::new_single_key(b"k3".to_vec());
    let present_rightmost_proof = merk
        .prove(present_rightmost_query, None, &grove_version)
        .unwrap()
        .expect("present rightmost proof");
    let root_hash = merk.root_hash().value;

    write_file(db_path, "proof_present.bin", &present_proof.proof);
    write_file(
        db_path,
        "proof_present_leftmost.bin",
        &present_leftmost_proof.proof,
    );
    write_file(
        db_path,
        "proof_present_rightmost.bin",
        &present_rightmost_proof.proof,
    );
    write_file(db_path, "proof_absent.bin", &absent_proof.proof);
    write_file(db_path, "proof_range.bin", &range_proof.proof);
    write_file(
        db_path,
        "proof_range_left_edge_boundary.bin",
        &left_edge_boundary_range_proof.proof,
    );
    write_file(
        db_path,
        "proof_range_right_edge_absence.bin",
        &right_edge_absence_range_proof.proof,
    );
    write_file(
        db_path,
        "proof_range_exclusive_end.bin",
        &range_exclusive_end_proof.proof,
    );
    write_file(db_path, "root_hash.bin", &root_hash);

    let batch = StorageBatch::new();
    let transaction = storage.start_transaction();
    let subtree_count = SubtreePath::from(&[b"root_count" as &[u8]]);
    let ctx_count = storage
        .get_transactional_storage_context(subtree_count, Some(&batch), &transaction)
        .unwrap();
    let mut merk_count = Merk::open_base(
        ctx_count,
        TreeType::ProvableCountTree,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open count merk");
    let count_ops = vec![
        (
            b"k1".to_vec(),
            Op::Put(b"v1".to_vec(), TreeFeatureType::ProvableCountedMerkNode(1)),
        ),
        (
            b"k2".to_vec(),
            Op::Put(b"v2".to_vec(), TreeFeatureType::ProvableCountedMerkNode(1)),
        ),
        (
            b"k3".to_vec(),
            Op::Put(b"v3".to_vec(), TreeFeatureType::ProvableCountedMerkNode(1)),
        ),
    ];
    merk_count
        .apply::<_, Vec<_>>(&count_ops, &[], None, &grove_version)
        .unwrap()
        .expect("apply count ops");
    storage
        .commit_multi_context_batch(batch, None)
        .unwrap()
        .expect("commit count batch");

    let transaction = storage.start_transaction();
    let ctx_count = storage
        .get_transactional_storage_context(
            SubtreePath::from(&[b"root_count" as &[u8]]),
            None,
            &transaction,
        )
        .unwrap();
    let merk_count = Merk::open_base(
        ctx_count,
        TreeType::ProvableCountTree,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open count merk");
    let present_count_query = Query::new_single_key(b"k2".to_vec());
    let present_count_proof = merk_count
        .prove(present_count_query, None, &grove_version)
        .unwrap()
        .expect("present count proof");
    let absent_count_query = Query::new_single_key(b"k0".to_vec());
    let absent_count_proof = merk_count
        .prove(absent_count_query, None, &grove_version)
        .unwrap()
        .expect("absent count proof");
    let mut range_count_query = Query::default();
    range_count_query.left_to_right = true;
    range_count_query.insert_range_inclusive(b"k1".to_vec()..=b"k3".to_vec());
    let range_count_proof = merk_count
        .prove(range_count_query, None, &grove_version)
        .unwrap()
        .expect("range count proof");
    let root_count_hash = merk_count.root_hash().value;

    write_file(
        db_path,
        "proof_count_present.bin",
        &present_count_proof.proof,
    );
    write_file(db_path, "proof_count_absent.bin", &absent_count_proof.proof);
    write_file(db_path, "proof_count_range.bin", &range_count_proof.proof);
    write_file(db_path, "root_count_hash.bin", &root_count_hash);
}
