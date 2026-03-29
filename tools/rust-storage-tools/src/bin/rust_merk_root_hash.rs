use std::env;
use std::fs;

use grovedb_merk::Merk;
use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use grovedb_storage::Storage;
use grovedb_version::version::GroveVersion;

fn to_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let out_path = env::args().nth(2).expect("output path required");
    let subtree_mode = env::args().nth(3).unwrap_or_else(|| "root".to_string());

    let storage = RocksDbStorage::default_rocksdb_with_path(path).expect("open rocksdb storage");
    let transaction = storage.start_transaction();
    let subtree_segments: Vec<&[u8]> = if subtree_mode == "child" {
        vec![b"root" as &[u8], b"child" as &[u8]]
    } else {
        vec![b"root" as &[u8]]
    };
    let subtree = SubtreePath::from(subtree_segments.as_slice());
    let ctx = storage
        .get_transactional_storage_context(subtree, None, &transaction)
        .unwrap();
    let grove_version = GroveVersion::latest();
    let merk = Merk::open_base(
        ctx,
        grovedb_merk::tree_type::TreeType::NormalTree,
        None::<&fn(&[u8], &GroveVersion) -> Option<grovedb_merk::tree::kv::ValueDefinedCostType>>,
        &grove_version,
    )
    .unwrap()
    .expect("open merk");

    let root_hash = merk.root_hash().value;
    let hex = to_hex(&root_hash);
    fs::write(out_path, hex).expect("write root hash");
}
