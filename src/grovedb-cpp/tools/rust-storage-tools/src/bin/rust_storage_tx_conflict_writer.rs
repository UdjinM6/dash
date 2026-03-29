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

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let scenario = env::args().nth(2).expect("scenario required");
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
    let key = |k: &'static [u8]| prefixed_key(prefix_root, k);

    match scenario.as_str() {
        "same_key" => {
            let tx1 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx1.snapshot();
            let tx2 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx2.snapshot();
            tx1.put_cf(&cf_default, key(b"k"), b"v1").unwrap();
            tx2.put_cf(&cf_default, key(b"k"), b"v2").unwrap();

            tx1.commit().expect("tx1 commit should succeed");
            let tx2_result = tx2.commit();
            assert!(tx2_result.is_err(), "tx2 should conflict on same key");
        }
        "disjoint" => {
            let tx1 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx1.snapshot();
            let tx2 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx2.snapshot();
            tx1.put_cf(&cf_default, key(b"k1"), b"v1").unwrap();
            tx2.put_cf(&cf_default, key(b"k2"), b"v2").unwrap();

            tx1.commit().expect("tx1 commit should succeed");
            tx2.commit().expect("tx2 commit should succeed");
        }
        "delete_then_put_same_key" => {
            db.put_cf(&cf_default, key(b"k"), b"seed")
                .expect("seed write should succeed");

            let tx1 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx1.snapshot();
            let tx2 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx2.snapshot();
            tx1.delete_cf(&cf_default, key(b"k")).unwrap();
            tx2.put_cf(&cf_default, key(b"k"), b"v2").unwrap();

            tx1.commit().expect("tx1 delete commit should succeed");
            let tx2_result = tx2.commit();
            assert!(tx2_result.is_err(), "tx2 should conflict after tx1 delete");
        }
        "delete_prefix_then_put_same_key" => {
            db.put_cf(&cf_default, key(b"p"), b"seed")
                .expect("seed write should succeed");

            let tx1 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx1.snapshot();
            let tx2 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx2.snapshot();
            tx1.delete_cf(&cf_default, key(b"p")).unwrap();
            tx2.put_cf(&cf_default, key(b"p"), b"v2").unwrap();

            tx1.commit()
                .expect("tx1 prefix-delete commit should succeed");
            let tx2_result = tx2.commit();
            assert!(
                tx2_result.is_err(),
                "tx2 should conflict after tx1 prefix-delete"
            );
        }
        "delete_prefix_multi_then_put_same_key" => {
            db.put_cf(&cf_default, key(b"p1"), b"seed1")
                .expect("seed write p1 should succeed");
            db.put_cf(&cf_default, key(b"p2"), b"seed2")
                .expect("seed write p2 should succeed");

            let tx1 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx1.snapshot();
            let tx2 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx2.snapshot();

            let mut to_delete = Vec::new();
            {
                let mut it = tx1.raw_iterator_cf(&cf_default);
                it.seek(prefix_root);
                while it.valid() {
                    let full_key = it.key().expect("iterator key");
                    if !full_key.starts_with(&prefix_root) {
                        break;
                    }
                    to_delete.push(full_key.to_vec());
                    it.next();
                }
            }
            for full_key in to_delete {
                tx1.delete_cf(&cf_default, full_key).unwrap();
            }

            tx2.put_cf(&cf_default, key(b"p1"), b"v2").unwrap();

            tx1.commit()
                .expect("tx1 multi-prefix-delete commit should succeed");
            let tx2_result = tx2.commit();
            assert!(
                tx2_result.is_err(),
                "tx2 should conflict after tx1 multi-prefix-delete"
            );
        }
        "delete_then_put_disjoint" => {
            db.put_cf(&cf_default, key(b"k1"), b"seed1")
                .expect("seed write k1 should succeed");
            db.put_cf(&cf_default, key(b"k2"), b"seed2")
                .expect("seed write k2 should succeed");

            let tx1 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx1.snapshot();
            let tx2 = db.transaction_opt(
                &WriteOptions::default(),
                &rocksdb::OptimisticTransactionOptions::default(),
            );
            let _ = tx2.snapshot();
            tx1.delete_cf(&cf_default, key(b"k1")).unwrap();
            tx2.put_cf(&cf_default, key(b"k2"), b"v2").unwrap();

            tx1.commit()
                .expect("tx1 disjoint delete commit should succeed");
            tx2.commit()
                .expect("tx2 disjoint put commit should succeed");
        }
        _ => panic!("unknown scenario: {scenario}"),
    }
}
