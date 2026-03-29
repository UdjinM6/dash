use std::env;

use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use rocksdb::{ColumnFamilyDescriptor, OptimisticTransactionDB, Options, WriteOptions};

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

fn marker(seen: bool) -> &'static [u8] {
    if seen {
        b"1"
    } else {
        b"0"
    }
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
    let prefix_iter = build_prefix(vec![b"iter_root".to_vec()]);
    let prefix_other = build_prefix(vec![b"other_root".to_vec()]);

    // Tx should always see its own writes.
    let tx_own = db.transaction_opt(
        &WriteOptions::default(),
        &rocksdb::OptimisticTransactionOptions::default(),
    );
    let _ = tx_own.snapshot();
    tx_own
        .put_cf(&cf_default, prefixed_key(prefix_root, b"k_self"), b"v_self")
        .unwrap();
    let own_seen = tx_own
        .get_cf(&cf_default, prefixed_key(prefix_root, b"k_self"))
        .unwrap()
        .is_some();
    tx_own
        .put_cf(
            &cf_default,
            prefixed_key(prefix_root, b"m_own"),
            marker(own_seen),
        )
        .unwrap();
    tx_own.commit().unwrap();

    // Tx snapshot should not see writes committed by a concurrent tx started later.
    let tx_reader = db.transaction_opt(
        &WriteOptions::default(),
        &rocksdb::OptimisticTransactionOptions::default(),
    );
    let _ = tx_reader.snapshot();
    let tx_writer = db.transaction_opt(
        &WriteOptions::default(),
        &rocksdb::OptimisticTransactionOptions::default(),
    );
    tx_writer
        .put_cf(
            &cf_default,
            prefixed_key(prefix_root, b"k_external"),
            b"v_external",
        )
        .unwrap();
    tx_writer.commit().unwrap();
    let concurrent_seen = tx_reader
        .get_cf(&cf_default, prefixed_key(prefix_root, b"k_external"))
        .unwrap()
        .is_some();
    tx_reader
        .put_cf(
            &cf_default,
            prefixed_key(prefix_root, b"m_concurrent"),
            marker(concurrent_seen),
        )
        .unwrap();
    tx_reader.commit().unwrap();

    // Tx iterator should include its own writes and remain within prefix bounds.
    db.put_cf(&cf_default, prefixed_key(prefix_iter, b"a"), b"va")
        .unwrap();
    db.put_cf(&cf_default, prefixed_key(prefix_iter, b"c"), b"vc")
        .unwrap();
    db.put_cf(&cf_default, prefixed_key(prefix_other, b"z"), b"vz")
        .unwrap();

    let tx_iter = db.transaction_opt(
        &WriteOptions::default(),
        &rocksdb::OptimisticTransactionOptions::default(),
    );
    let _ = tx_iter.snapshot();
    tx_iter
        .put_cf(&cf_default, prefixed_key(prefix_iter, b"b"), b"vb")
        .unwrap();

    let mut keys = Vec::new();
    {
        let mut iter = tx_iter.raw_iterator();
        iter.seek(prefix_iter);
        while iter.valid() {
            let k = iter.key().expect("key should exist");
            if !k.starts_with(&prefix_iter) {
                break;
            }
            keys.push(String::from_utf8(k[prefix_iter.len()..].to_vec()).expect("utf8 key"));
            iter.next();
        }
    }
    let joined = keys.join("|");
    tx_iter
        .put_cf(
            &cf_default,
            prefixed_key(prefix_root, b"m_iter"),
            joined.as_bytes(),
        )
        .unwrap();
    tx_iter.commit().unwrap();

    // Rollback should not leak writes.
    let tx_rb = db.transaction_opt(
        &WriteOptions::default(),
        &rocksdb::OptimisticTransactionOptions::default(),
    );
    let _ = tx_rb.snapshot();
    tx_rb
        .put_cf(&cf_default, prefixed_key(prefix_root, b"k_rb"), b"v_rb")
        .unwrap();
    tx_rb.rollback().unwrap();
    let rollback_seen = db
        .get_cf(&cf_default, prefixed_key(prefix_root, b"k_rb"))
        .unwrap()
        .is_some();
    db.put_cf(
        &cf_default,
        prefixed_key(prefix_root, b"m_rollback"),
        marker(rollback_seen),
    )
    .unwrap();
}
