use std::env;
use std::path::Path;

use grovedb::{Element, GroveDb};
use grovedb_version::version::GroveVersion;

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let db = GroveDb::open(Path::new(&path)).expect("open grovedb");
    let gv = GroveVersion::latest();

    db.insert(
        &[] as &[&[u8]],
        b"leafA",
        Element::empty_tree(),
        None,
        None,
        &gv,
    )
    .unwrap()
    .expect("insert leafA");
    db.insert(
        &[] as &[&[u8]],
        b"leafB",
        Element::empty_tree(),
        None,
        None,
        &gv,
    )
    .unwrap()
    .expect("insert leafB");

    let tx_a = db.start_transaction();
    let tx_b = db.start_transaction();

    db.insert(
        &[b"leafA".as_slice()],
        b"a1",
        Element::new_item(b"v".to_vec()),
        None,
        Some(&tx_a),
        &gv,
    )
    .unwrap()
    .expect("tx_a insert");
    db.insert(
        &[b"leafB".as_slice()],
        b"b1",
        Element::new_item(b"v".to_vec()),
        None,
        Some(&tx_b),
        &gv,
    )
    .unwrap()
    .expect("tx_b insert");

    let commit_a = db.commit_transaction(tx_a).unwrap();
    let commit_b = db.commit_transaction(tx_b).unwrap();

    println!("disjoint_commit_a_ok={}", commit_a.is_ok());
    println!("disjoint_commit_b_ok={}", commit_b.is_ok());
    if let Err(e) = commit_b {
        println!("disjoint_commit_b_err={}", e);
    }

    let tx_c1 = db.start_transaction();
    let tx_c2 = db.start_transaction();
    db.insert(
        &[b"leafA".as_slice()],
        b"conflict",
        Element::new_item(b"a".to_vec()),
        None,
        Some(&tx_c1),
        &gv,
    )
    .unwrap()
    .expect("tx_c1 insert");
    db.insert(
        &[b"leafA".as_slice()],
        b"conflict",
        Element::new_item(b"b".to_vec()),
        None,
        Some(&tx_c2),
        &gv,
    )
    .unwrap()
    .expect("tx_c2 insert");

    let commit_c1 = db.commit_transaction(tx_c1).unwrap();
    let commit_c2 = db.commit_transaction(tx_c2).unwrap();
    println!("same_path_commit_1_ok={}", commit_c1.is_ok());
    println!("same_path_commit_2_ok={}", commit_c2.is_ok());
    if let Err(e) = commit_c2 {
        println!("same_path_commit_2_err={}", e);
    }
}
