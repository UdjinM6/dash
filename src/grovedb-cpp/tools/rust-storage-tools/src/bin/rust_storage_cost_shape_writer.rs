use std::env;

use grovedb_path::SubtreePath;
use grovedb_storage::rocksdb_storage::RocksDbStorage;
use rocksdb::{AsColumnFamilyRef, ColumnFamilyDescriptor, OptimisticTransactionDB, Options};

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

fn varint_len(value: u64) -> u64 {
    if value < (1_u64 << 7) {
        1
    } else if value < (1_u64 << 14) {
        2
    } else if value < (1_u64 << 21) {
        3
    } else if value < (1_u64 << 28) {
        4
    } else if value < (1_u64 << 35) {
        5
    } else if value < (1_u64 << 42) {
        6
    } else if value < (1_u64 << 49) {
        7
    } else if value < (1_u64 << 56) {
        8
    } else if value < (1_u64 << 63) {
        9
    } else {
        10
    }
}

fn paid_len(len: usize) -> u64 {
    let l = len as u64;
    l + varint_len(l)
}

fn blake_block_count(len: usize) -> u64 {
    const BLOCK_LEN: usize = 64;
    if len == 0 {
        return 1;
    }
    (1 + (len - 1) / BLOCK_LEN) as u64
}

fn prefix_hash_calls(path: &[Vec<u8>]) -> u64 {
    if path.is_empty() {
        return 0;
    }
    let bytes_len: usize = path.iter().map(|s| s.len()).sum();
    let body_len = bytes_len + std::mem::size_of::<usize>() + path.len();
    blake_block_count(body_len)
}

fn write_marker<T: AsColumnFamilyRef>(
    db: &OptimisticTransactionDB<rocksdb::MultiThreaded>,
    cf_default: &T,
    prefix_root: [u8; 32],
    key: &[u8],
    value: u64,
) {
    db.put_cf(
        cf_default,
        prefixed_key(prefix_root, key),
        value.to_string().as_bytes(),
    )
    .expect("marker write");
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

    let path_one = vec![b"root".to_vec()];
    let path_hash_calls = prefix_hash_calls(&path_one);
    let path_two = vec![b"root".to_vec(), b"child".to_vec()];
    let path_two_hash_calls = prefix_hash_calls(&path_two);

    // Overlay scenario:
    // put(k1,v1), put(k1,v2), delete(k1), with absent key baseline.
    let overlay_key_len = b"o1".len();
    let overlay_v1_len = b"ov1".len();
    let overlay_v2_len = b"ov2".len();
    let overlay_seek = 4_u64;
    let overlay_loaded = 0_u64;
    let overlay_added = paid_len(overlay_key_len) + paid_len(overlay_v1_len);
    let overlay_replaced = paid_len(overlay_v2_len);
    let overlay_removed = paid_len(overlay_key_len) + paid_len(overlay_v2_len);
    let overlay_hash = path_hash_calls * 3;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_overlay_seek",
        overlay_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_overlay_loaded",
        overlay_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_overlay_added",
        overlay_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_overlay_replaced",
        overlay_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_overlay_removed",
        overlay_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_overlay_hash",
        overlay_hash,
    );

    // Mixed overlay scenario:
    // put(k1,a), put(k1,b), put(k2,c), delete(k1), put(k2,d), absent baseline.
    let k1_len = b"m1".len();
    let k2_len = b"m2".len();
    let v_a_len = b"va".len();
    let v_b_len = b"vb".len();
    let v_c_len = b"vc".len();
    let v_d_len = b"vd".len();
    let mixed_seek = 7_u64;
    let mixed_loaded = 0_u64;
    let mixed_added = paid_len(k1_len) + paid_len(v_a_len) + paid_len(k2_len) + paid_len(v_c_len);
    let mixed_replaced = paid_len(v_b_len) + paid_len(v_d_len);
    let mixed_removed = paid_len(k1_len) + paid_len(v_b_len);
    let mixed_hash = path_hash_calls * 5;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_mixed_seek",
        mixed_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_mixed_loaded",
        mixed_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_mixed_added",
        mixed_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_mixed_replaced",
        mixed_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_mixed_removed",
        mixed_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_mixed_hash",
        mixed_hash,
    );

    // Existing-key replace scenario:
    // existing seed old -> put(new) in one-op batch.
    let replace_key_len = b"r1".len();
    let replace_old_len = b"oldv".len();
    let replace_new_len = b"newv".len();
    let replace_seek = 2_u64;
    let replace_loaded = replace_old_len as u64;
    let replace_added = 0_u64;
    let replace_replaced = paid_len(replace_new_len);
    let replace_removed = 0_u64;
    let replace_hash = path_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_replace_seek",
        replace_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_replace_loaded",
        replace_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_replace_added",
        replace_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_replace_replaced",
        replace_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_replace_removed",
        replace_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_replace_hash",
        replace_hash,
    );

    // Existing-key delete scenario:
    // existing seed old -> delete in one-op batch.
    let delete_key_len = b"d1".len();
    let delete_old_len = b"oldv".len();
    let delete_seek = 2_u64;
    let delete_loaded = delete_old_len as u64;
    let delete_added = 0_u64;
    let delete_replaced = 0_u64;
    let delete_removed = paid_len(delete_key_len) + paid_len(delete_old_len);
    let delete_hash = path_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_seek",
        delete_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_loaded",
        delete_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_added",
        delete_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_replaced",
        delete_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_removed",
        delete_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_hash",
        delete_hash,
    );

    // Missing-key delete scenario:
    // delete absent key in one-op batch.
    let delete_missing_key_len = b"x1".len();
    let delete_missing_seek = 2_u64;
    let delete_missing_loaded = 0_u64;
    let delete_missing_added = 0_u64;
    let delete_missing_replaced = 0_u64;
    let delete_missing_removed = paid_len(delete_missing_key_len) + paid_len(0);
    let delete_missing_hash = path_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_missing_seek",
        delete_missing_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_missing_loaded",
        delete_missing_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_missing_added",
        delete_missing_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_missing_replaced",
        delete_missing_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_missing_removed",
        delete_missing_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_delete_missing_hash",
        delete_missing_hash,
    );

    // Deep-path put scenario:
    // put(k,v) at path ["root","child"] as absent-key insert.
    let deep_key_len = b"p1".len();
    let deep_value_len = b"pv".len();
    let deep_seek = 2_u64;
    let deep_loaded = 0_u64;
    let deep_added = paid_len(deep_key_len) + paid_len(deep_value_len);
    let deep_replaced = 0_u64;
    let deep_removed = 0_u64;
    let deep_hash = path_two_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_put_seek",
        deep_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_put_loaded",
        deep_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_put_added",
        deep_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_put_replaced",
        deep_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_put_removed",
        deep_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_put_hash",
        deep_hash,
    );

    // Deep-path existing-key delete scenario:
    // existing seed old at ["root","child"] -> delete in one-op batch.
    let deep_delete_key_len = b"q1".len();
    let deep_delete_old_len = b"oldv".len();
    let deep_delete_seek = 2_u64;
    let deep_delete_loaded = deep_delete_old_len as u64;
    let deep_delete_added = 0_u64;
    let deep_delete_replaced = 0_u64;
    let deep_delete_removed = paid_len(deep_delete_key_len) + paid_len(deep_delete_old_len);
    let deep_delete_hash = path_two_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_seek",
        deep_delete_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_loaded",
        deep_delete_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_added",
        deep_delete_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_replaced",
        deep_delete_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_removed",
        deep_delete_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_hash",
        deep_delete_hash,
    );

    // Deep-path existing-key replace scenario:
    // existing seed old at ["root","child"] -> put(new) in one-op batch.
    let deep_replace_old_len = b"oldv".len();
    let deep_replace_new_len = b"newv".len();
    let deep_replace_seek = 2_u64;
    let deep_replace_loaded = deep_replace_old_len as u64;
    let deep_replace_added = 0_u64;
    let deep_replace_replaced = paid_len(deep_replace_new_len);
    let deep_replace_removed = 0_u64;
    let deep_replace_hash = path_two_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_replace_seek",
        deep_replace_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_replace_loaded",
        deep_replace_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_replace_added",
        deep_replace_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_replace_replaced",
        deep_replace_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_replace_removed",
        deep_replace_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_replace_hash",
        deep_replace_hash,
    );

    // Deep-path missing-key delete scenario:
    // delete absent key at ["root","child"] in one-op batch.
    let deep_delete_missing_key_len = b"z1".len();
    let deep_delete_missing_seek = 2_u64;
    let deep_delete_missing_loaded = 0_u64;
    let deep_delete_missing_added = 0_u64;
    let deep_delete_missing_replaced = 0_u64;
    let deep_delete_missing_removed = paid_len(deep_delete_missing_key_len) + paid_len(0);
    let deep_delete_missing_hash = path_two_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_missing_seek",
        deep_delete_missing_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_missing_loaded",
        deep_delete_missing_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_missing_added",
        deep_delete_missing_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_missing_replaced",
        deep_delete_missing_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_missing_removed",
        deep_delete_missing_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_delete_missing_hash",
        deep_delete_missing_hash,
    );

    // Deep-path mixed overlay scenario:
    // put(u1,a), put(u1,b), put(u2,c), delete(u1), put(u2,d), absent baseline.
    let du1_len = b"u1".len();
    let du2_len = b"u2".len();
    let dva_len = b"va".len();
    let dvb_len = b"vb".len();
    let dvc_len = b"vc".len();
    let dvd_len = b"vd".len();
    let deep_mixed_seek = 7_u64;
    let deep_mixed_loaded = 0_u64;
    let deep_mixed_added =
        paid_len(du1_len) + paid_len(dva_len) + paid_len(du2_len) + paid_len(dvc_len);
    let deep_mixed_replaced = paid_len(dvb_len) + paid_len(dvd_len);
    let deep_mixed_removed = paid_len(du1_len) + paid_len(dvb_len);
    let deep_mixed_hash = path_two_hash_calls * 5;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_mixed_seek",
        deep_mixed_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_mixed_loaded",
        deep_mixed_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_mixed_added",
        deep_mixed_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_mixed_replaced",
        deep_mixed_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_mixed_removed",
        deep_mixed_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_deep_mixed_hash",
        deep_mixed_hash,
    );

    // CF-specific replace scenario (aux):
    // existing seed old -> put(new) in one-op batch under aux.
    let cf_replace_old_len = b"oldv".len();
    let cf_replace_new_len = b"newv".len();
    let cf_replace_seek = 2_u64;
    let cf_replace_loaded = cf_replace_old_len as u64;
    let cf_replace_added = 0_u64;
    let cf_replace_replaced = paid_len(cf_replace_new_len);
    let cf_replace_removed = 0_u64;
    let cf_replace_hash = path_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_replace_seek",
        cf_replace_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_replace_loaded",
        cf_replace_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_replace_added",
        cf_replace_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_replace_replaced",
        cf_replace_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_replace_removed",
        cf_replace_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_replace_hash",
        cf_replace_hash,
    );

    // CF-specific delete scenario (meta, existing key).
    let cf_delete_key_len = b"c1".len();
    let cf_delete_old_len = b"oldv".len();
    let cf_delete_seek = 2_u64;
    let cf_delete_loaded = cf_delete_old_len as u64;
    let cf_delete_added = 0_u64;
    let cf_delete_replaced = 0_u64;
    let cf_delete_removed = paid_len(cf_delete_key_len) + paid_len(cf_delete_old_len);
    let cf_delete_hash = path_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_delete_seek",
        cf_delete_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_delete_loaded",
        cf_delete_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_delete_added",
        cf_delete_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_delete_replaced",
        cf_delete_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_delete_removed",
        cf_delete_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_delete_hash",
        cf_delete_hash,
    );

    // CF-specific missing-delete scenario (roots).
    let cf_missing_delete_key_len = b"c2".len();
    let cf_missing_delete_seek = 2_u64;
    let cf_missing_delete_loaded = 0_u64;
    let cf_missing_delete_added = 0_u64;
    let cf_missing_delete_replaced = 0_u64;
    let cf_missing_delete_removed = paid_len(cf_missing_delete_key_len) + paid_len(0);
    let cf_missing_delete_hash = path_hash_calls;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_missing_delete_seek",
        cf_missing_delete_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_missing_delete_loaded",
        cf_missing_delete_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_missing_delete_added",
        cf_missing_delete_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_missing_delete_replaced",
        cf_missing_delete_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_missing_delete_removed",
        cf_missing_delete_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_missing_delete_hash",
        cf_missing_delete_hash,
    );

    // Mixed-CF scenario:
    // default put(c3,va), aux put(c3,vb), meta put(c3,vc), roots delete(c3) (absent baseline).
    let cf_mixed_key_len = b"c3".len();
    let cf_mixed_va_len = b"va".len();
    let cf_mixed_vb_len = b"vb".len();
    let cf_mixed_vc_len = b"vc".len();
    let cf_mixed_seek = 8_u64;
    let cf_mixed_loaded = 0_u64;
    let cf_mixed_added = paid_len(cf_mixed_key_len)
        + paid_len(cf_mixed_va_len)
        + paid_len(cf_mixed_key_len)
        + paid_len(cf_mixed_vb_len)
        + paid_len(cf_mixed_key_len)
        + paid_len(cf_mixed_vc_len);
    let cf_mixed_replaced = 0_u64;
    let cf_mixed_removed = paid_len(cf_mixed_key_len) + paid_len(0);
    let cf_mixed_hash = path_hash_calls * 4;

    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_mixed_seek",
        cf_mixed_seek,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_mixed_loaded",
        cf_mixed_loaded,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_mixed_added",
        cf_mixed_added,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_mixed_replaced",
        cf_mixed_replaced,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_mixed_removed",
        cf_mixed_removed,
    );
    write_marker(
        &db,
        &cf_default,
        prefix_root,
        b"m_cost_cf_mixed_hash",
        cf_mixed_hash,
    );
}
