use std::env;

use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use rocksdb::{
    ColumnFamilyDescriptor, OptimisticTransactionDB, Options, WriteBatchWithTransaction,
};

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

fn main() {
    let path = env::args().nth(1).expect("db path required");
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
        OptimisticTransactionDB::open_cf_descriptors(&opts, path, cfs).expect("open rocksdb");

    let cf_default = db.cf_handle("default").unwrap();
    let prefix_root = build_prefix(vec![b"root".to_vec()]);

    let mut batch = WriteBatchWithTransaction::<true>::default();
    batch.put_cf(&cf_default, prefixed_key(prefix_root, b"k1"), b"v1");
    batch.put_cf(&cf_default, prefixed_key(prefix_root, b"k2"), b"v2");
    batch.delete_cf(&cf_default, prefixed_key(prefix_root, b"k1"));
    db.write(batch).unwrap();
}
