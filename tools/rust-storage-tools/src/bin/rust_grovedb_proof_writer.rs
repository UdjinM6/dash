use std::env;
use std::fs;
use std::path::Path;

use grovedb::reference_path::ReferencePathType;
use grovedb::{Element, GroveDb, PathQuery, SizedQuery, TransactionArg};
use grovedb_merk::proofs::Query;
use grovedb_version::version::GroveVersion;

fn write_file(path: &Path, name: &str, data: &[u8]) {
    let out_path = path.join(name);
    fs::write(out_path, data).expect("write output file");
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let db_path = Path::new(&path);

    let db = GroveDb::open(db_path).expect("open grovedb");
    let grove_version = GroveVersion::latest();

    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::Item(b"v1".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert k1");

    db.insert(
        &[b"root".as_slice()],
        b"k2",
        Element::Item(b"v2".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert k2");

    db.insert(
        &[b"root".as_slice()],
        b"key",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key tree");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice()],
        b"a",
        Element::Item(b"1".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/a");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice()],
        b"b",
        Element::Item(b"2".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/b");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice()],
        b"branch",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/branch tree");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice(), b"branch".as_slice()],
        b"x",
        Element::Item(b"9".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/branch/x");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice(), b"branch".as_slice()],
        b"y",
        Element::Item(b"8".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/branch/y");

    db.insert(
        &[b"root".as_slice()],
        b"key_b",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key_b tree");

    db.insert(
        &[b"root".as_slice(), b"key_b".as_slice()],
        b"branch",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key_b/branch tree");

    db.insert(
        &[
            b"root".as_slice(),
            b"key_b".as_slice(),
            b"branch".as_slice(),
        ],
        b"z",
        Element::Item(b"4".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key_b/branch/z");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice()],
        b"nest",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/nest tree");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice(), b"nest".as_slice()],
        b"m",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/nest/m tree");

    db.insert(
        &[
            b"root".as_slice(),
            b"key".as_slice(),
            b"nest".as_slice(),
            b"m".as_slice(),
        ],
        b"u",
        Element::Item(b"7".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/nest/m/u");

    db.insert(
        &[b"root".as_slice(), b"key".as_slice(), b"nest".as_slice()],
        b"n",
        Element::Item(b"6".to_vec(), None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert key/nest/n");

    db.insert(
        &[b"root".as_slice()],
        b"refs",
        Element::Tree(None, None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert refs tree");

    db.insert(
        &[b"root".as_slice()],
        b"refb",
        Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
            b"root".to_vec(),
            b"key".to_vec(),
        ])),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert refb");

    db.insert(
        &[b"root".as_slice()],
        b"refa",
        Element::new_reference_with_hops(
            ReferencePathType::SiblingReference(b"refb".to_vec()),
            Some(2),
        ),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert refa");

    db.insert(
        &[b"root".as_slice(), b"refs".as_slice()],
        b"ref_cycle_a",
        Element::new_item(b"a".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ref_cycle_a");

    db.insert(
        &[b"root".as_slice(), b"refs".as_slice()],
        b"ref_cycle_b",
        Element::new_reference(ReferencePathType::SiblingReference(b"ref_cycle_a".to_vec())),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ref_cycle_b");

    db.insert(
        &[b"root".as_slice(), b"refs".as_slice()],
        b"ref_cycle_a",
        Element::new_reference(ReferencePathType::SiblingReference(b"ref_cycle_b".to_vec())),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("replace ref_cycle_a with reference");

    db.insert(
        &[b"root".as_slice(), b"refs".as_slice()],
        b"ref_hop_b",
        Element::new_reference_with_hops(
            ReferencePathType::AbsolutePathReference(vec![b"root".to_vec(), b"key".to_vec()]),
            Some(1),
        ),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ref_hop_b");

    db.insert(
        &[b"root".as_slice(), b"refs".as_slice()],
        b"ref_hop_a",
        Element::new_reference_with_hops(
            ReferencePathType::SiblingReference(b"ref_hop_b".to_vec()),
            Some(2),
        ),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ref_hop_a");

    let present_query = PathQuery::new_single_key(vec![b"root".to_vec()], b"k2".to_vec());
    let present_proof = db
        .prove_query(&present_query, None, &grove_version)
        .unwrap()
        .expect("present proof");

    let absent_query = PathQuery::new_single_key(vec![b"root".to_vec()], b"k0".to_vec());
    let absent_proof = db
        .prove_query(&absent_query, None, &grove_version)
        .unwrap()
        .expect("absent proof");

    let mut root_range_query = Query::new();
    root_range_query.insert_range_inclusive(b"k1".to_vec()..=b"k2".to_vec());
    let root_range_path_query = PathQuery::new_unsized(vec![b"root".to_vec()], root_range_query);
    let root_range_proof = db
        .prove_query(&root_range_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range proof");
    let mut root_range_desc_query = Query::new();
    root_range_desc_query.left_to_right = false;
    root_range_desc_query.insert_range_inclusive(b"k1".to_vec()..=b"k2".to_vec());
    let root_range_desc_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_desc_query);
    let root_range_desc_proof = db
        .prove_query(&root_range_desc_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_desc proof");
    let mut root_range_exclusive_query = Query::new();
    root_range_exclusive_query.insert_range(b"k1".to_vec()..b"k3".to_vec());
    let root_range_exclusive_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_exclusive_query);
    let root_range_exclusive_proof = db
        .prove_query(&root_range_exclusive_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_exclusive proof");
    let mut root_range_to_inclusive_query = Query::new();
    root_range_to_inclusive_query.insert_range_to_inclusive(..=b"k2".to_vec());
    let root_range_to_inclusive_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_to_inclusive_query);
    let root_range_to_inclusive_proof = db
        .prove_query(&root_range_to_inclusive_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_to_inclusive proof");
    let mut root_range_after_query = Query::new();
    root_range_after_query.insert_range_after(b"k1".to_vec()..);
    let root_range_after_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_after_query);
    let root_range_after_proof = db
        .prove_query(&root_range_after_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_after proof");
    let mut root_range_to_query = Query::new();
    root_range_to_query.insert_range_to(..b"k2".to_vec());
    let root_range_to_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_to_query);
    let root_range_to_proof = db
        .prove_query(&root_range_to_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_to proof");
    let mut root_range_from_query = Query::new();
    root_range_from_query.insert_range_from(b"k2".to_vec()..);
    let root_range_from_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_from_query);
    let root_range_from_proof = db
        .prove_query(&root_range_from_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_from proof");
    let mut root_range_after_to_query = Query::new();
    root_range_after_to_query.insert_range_after_to(b"k1".to_vec()..b"k3".to_vec());
    let root_range_after_to_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_after_to_query);
    let root_range_after_to_proof = db
        .prove_query(&root_range_after_to_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_after_to proof");
    let mut root_range_after_to_inclusive_query = Query::new();
    root_range_after_to_inclusive_query
        .insert_range_after_to_inclusive(b"k1".to_vec()..=b"k3".to_vec());
    let root_range_after_to_inclusive_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_after_to_inclusive_query);
    let root_range_after_to_inclusive_proof = db
        .prove_query(
            &root_range_after_to_inclusive_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("root_range_after_to_inclusive proof");

    let root_range_full_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], Query::new_range_full());
    let root_range_full_proof = db
        .prove_query(&root_range_full_query, None, &grove_version)
        .unwrap()
        .expect("root_range_full proof");
    let nested_path_range_full_query = PathQuery::new_unsized(
        vec![b"root".to_vec(), b"key".to_vec(), b"branch".to_vec()],
        Query::new_range_full(),
    );
    let nested_path_range_full_proof = db
        .prove_query(&nested_path_range_full_query, None, &grove_version)
        .unwrap()
        .expect("nested_path_range_full proof");
    let mut root_multi_key_query = Query::new();
    root_multi_key_query.insert_key(b"k1".to_vec());
    root_multi_key_query.insert_key(b"k2".to_vec());
    let root_multi_key_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_multi_key_query);
    let root_multi_key_proof = db
        .prove_query(&root_multi_key_path_query, None, &grove_version)
        .unwrap()
        .expect("root_multi_key proof");
    let mut root_range_to_absent_query = Query::new();
    root_range_to_absent_query.insert_range_to(..b"k0".to_vec());
    let root_range_to_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_to_absent_query);
    let root_range_to_absent_proof = db
        .prove_query(&root_range_to_absent_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_to_absent proof");
    let mut root_range_to_inclusive_absent_query = Query::new();
    root_range_to_inclusive_absent_query.insert_range_to_inclusive(..=b"k0".to_vec());
    let root_range_to_inclusive_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_to_inclusive_absent_query);
    let root_range_to_inclusive_absent_proof = db
        .prove_query(
            &root_range_to_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("root_range_to_inclusive_absent proof");
    let mut root_range_from_absent_query = Query::new();
    root_range_from_absent_query.insert_range_from(b"kz".to_vec()..);
    let root_range_from_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_from_absent_query);
    let root_range_from_absent_proof = db
        .prove_query(&root_range_from_absent_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_from_absent proof");
    let mut root_range_after_absent_query = Query::new();
    root_range_after_absent_query.insert_range_after(b"key_b".to_vec()..);
    let root_range_after_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_after_absent_query);
    let root_range_after_absent_proof = db
        .prove_query(&root_range_after_absent_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_after_absent proof");
    let mut root_range_absent_query = Query::new();
    root_range_absent_query.insert_range(b"k0".to_vec()..b"k1".to_vec());
    let root_range_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_absent_query);
    let root_range_absent_proof = db
        .prove_query(&root_range_absent_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_absent proof");
    let mut root_range_inclusive_absent_query = Query::new();
    root_range_inclusive_absent_query.insert_range_inclusive(b"k0".to_vec()..=b"k0".to_vec());
    let root_range_inclusive_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_inclusive_absent_query);
    let root_range_inclusive_absent_proof = db
        .prove_query(
            &root_range_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("root_range_inclusive_absent proof");
    let mut root_range_after_to_absent_query = Query::new();
    root_range_after_to_absent_query.insert_range_after_to(b"key_b".to_vec()..b"kz".to_vec());
    let root_range_after_to_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], root_range_after_to_absent_query);
    let root_range_after_to_absent_proof = db
        .prove_query(&root_range_after_to_absent_path_query, None, &grove_version)
        .unwrap()
        .expect("root_range_after_to_absent proof");
    let mut root_range_after_to_inclusive_absent_query = Query::new();
    root_range_after_to_inclusive_absent_query
        .insert_range_after_to_inclusive(b"key_b".to_vec()..=b"kz".to_vec());
    let root_range_after_to_inclusive_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        root_range_after_to_inclusive_absent_query,
    );
    let root_range_after_to_inclusive_absent_proof = db
        .prove_query(
            &root_range_after_to_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("root_range_after_to_inclusive_absent proof");

    let mut subquery_without_parent = Query::new();
    subquery_without_parent.insert_key(b"key".to_vec());
    subquery_without_parent.set_subquery(Query::new_range_full());
    subquery_without_parent.add_parent_tree_on_subquery = false;
    let subquery_without_parent_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_without_parent);
    let subquery_without_parent_proof = db
        .prove_query(&subquery_without_parent_query, None, &grove_version)
        .unwrap()
        .expect("subquery_without_parent proof");

    let mut subquery_with_parent = Query::new();
    subquery_with_parent.insert_key(b"key".to_vec());
    subquery_with_parent.set_subquery(Query::new_range_full());
    subquery_with_parent.add_parent_tree_on_subquery = true;
    let subquery_with_parent_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_with_parent);
    let subquery_with_parent_proof = db
        .prove_query(&subquery_with_parent_query, None, &grove_version)
        .unwrap()
        .expect("subquery_with_parent proof");

    let mut conditional_query = Query::new();
    conditional_query.insert_key(b"key".to_vec());
    conditional_query.insert_key(b"k2".to_vec());
    conditional_query.set_subquery(Query::new_range_full());
    conditional_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"k2".to_vec()),
        None,
        None,
    );
    let conditional_path_query = PathQuery::new_unsized(vec![b"root".to_vec()], conditional_query);
    let conditional_proof = db
        .prove_query(&conditional_path_query, None, &grove_version)
        .unwrap()
        .expect("conditional subquery proof");

    let mut subquery_path_query = Query::new();
    subquery_path_query.insert_key(b"key".to_vec());
    subquery_path_query.set_subquery_path(vec![b"branch".to_vec()]);
    subquery_path_query.set_subquery(Query::new_range_full());
    let subquery_path_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_query);
    let subquery_path_proof = db
        .prove_query(&subquery_path_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path proof");
    let mut subquery_path_range_after_query = Query::new();
    subquery_path_range_after_query.insert_key(b"key".to_vec());
    subquery_path_range_after_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after = Query::new();
    branch_range_after.insert_range_after(b"x".to_vec()..);
    subquery_path_range_after_query.set_subquery(branch_range_after);
    let subquery_path_range_after_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_after_query);
    let subquery_path_range_after_proof = db
        .prove_query(&subquery_path_range_after_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path_range_after proof");
    let mut subquery_path_range_after_absent_query = Query::new();
    subquery_path_range_after_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_after_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_absent = Query::new();
    branch_range_after_absent.insert_range_after(b"y".to_vec()..);
    subquery_path_range_after_absent_query.set_subquery(branch_range_after_absent);
    let subquery_path_range_after_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_after_absent_query,
    );
    let subquery_path_range_after_absent_proof = db
        .prove_query(
            &subquery_path_range_after_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_absent proof");
    let mut subquery_path_range_after_limit_query = Query::new();
    subquery_path_range_after_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_after_limit_query.insert_key(b"key_b".to_vec());
    subquery_path_range_after_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_limit = Query::new();
    branch_range_after_limit.insert_range_after(b"x".to_vec()..);
    subquery_path_range_after_limit_query.set_subquery(branch_range_after_limit);
    let subquery_path_range_after_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_after_limit_query, Some(1), None),
    );
    let subquery_path_range_after_limit_proof = db
        .prove_query(
            &subquery_path_range_after_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_limit proof");
    let mut subquery_path_range_to_inclusive_query = Query::new();
    subquery_path_range_to_inclusive_query.insert_key(b"key".to_vec());
    subquery_path_range_to_inclusive_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_to_inclusive = Query::new();
    branch_range_to_inclusive.insert_range_to_inclusive(..=b"x".to_vec());
    subquery_path_range_to_inclusive_query.set_subquery(branch_range_to_inclusive);
    let subquery_path_range_to_inclusive_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_to_inclusive_query,
    );
    let subquery_path_range_to_inclusive_proof = db
        .prove_query(
            &subquery_path_range_to_inclusive_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_to_inclusive proof");
    let mut subquery_path_range_to_inclusive_absent_query = Query::new();
    subquery_path_range_to_inclusive_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_to_inclusive_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_to_inclusive_absent = Query::new();
    branch_range_to_inclusive_absent.insert_range_to_inclusive(..=b"w".to_vec());
    subquery_path_range_to_inclusive_absent_query.set_subquery(branch_range_to_inclusive_absent);
    let subquery_path_range_to_inclusive_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_to_inclusive_absent_query,
    );
    let subquery_path_range_to_inclusive_absent_proof = db
        .prove_query(
            &subquery_path_range_to_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_to_inclusive_absent proof");
    let mut subquery_path_range_to_query = Query::new();
    subquery_path_range_to_query.insert_key(b"key".to_vec());
    subquery_path_range_to_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_to = Query::new();
    branch_range_to.insert_range_to(..b"y".to_vec());
    subquery_path_range_to_query.set_subquery(branch_range_to);
    let subquery_path_range_to_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_to_query);
    let subquery_path_range_to_proof = db
        .prove_query(&subquery_path_range_to_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path_range_to proof");
    let mut subquery_path_range_to_absent_query = Query::new();
    subquery_path_range_to_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_to_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_to_absent = Query::new();
    branch_range_to_absent.insert_range_to(..b"x".to_vec());
    subquery_path_range_to_absent_query.set_subquery(branch_range_to_absent);
    let subquery_path_range_to_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_to_absent_query);
    let subquery_path_range_to_absent_proof = db
        .prove_query(
            &subquery_path_range_to_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_to_absent proof");
    let mut subquery_path_range_to_limit_query = Query::new();
    subquery_path_range_to_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_to_limit_query.insert_key(b"key_b".to_vec());
    subquery_path_range_to_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_to_limit = Query::new();
    branch_range_to_limit.insert_range_to(..b"z".to_vec());
    subquery_path_range_to_limit_query.set_subquery(branch_range_to_limit);
    let subquery_path_range_to_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_to_limit_query, Some(1), None),
    );
    let subquery_path_range_to_limit_proof = db
        .prove_query(
            &subquery_path_range_to_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_to_limit proof");
    let mut subquery_path_range_from_query = Query::new();
    subquery_path_range_from_query.insert_key(b"key".to_vec());
    subquery_path_range_from_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_from = Query::new();
    branch_range_from.insert_range_from(b"y".to_vec()..);
    subquery_path_range_from_query.set_subquery(branch_range_from);
    let subquery_path_range_from_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_from_query);
    let subquery_path_range_from_proof = db
        .prove_query(&subquery_path_range_from_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path_range_from proof");
    let mut subquery_path_range_from_limit_query = Query::new();
    subquery_path_range_from_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_from_limit_query.insert_key(b"key_b".to_vec());
    subquery_path_range_from_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_from_limit = Query::new();
    branch_range_from_limit.insert_range_from(b"x".to_vec()..);
    subquery_path_range_from_limit_query.set_subquery(branch_range_from_limit);
    let subquery_path_range_from_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_from_limit_query, Some(1), None),
    );
    let subquery_path_range_from_limit_proof = db
        .prove_query(
            &subquery_path_range_from_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_from_limit proof");
    let mut subquery_path_range_from_absent_query = Query::new();
    subquery_path_range_from_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_from_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_from_absent = Query::new();
    branch_range_from_absent.insert_range_from(b"z".to_vec()..);
    subquery_path_range_from_absent_query.set_subquery(branch_range_from_absent);
    let subquery_path_range_from_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_from_absent_query,
    );
    let subquery_path_range_from_absent_proof = db
        .prove_query(
            &subquery_path_range_from_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_from_absent proof");
    let mut subquery_path_range_from_limit_absent_query = Query::new();
    subquery_path_range_from_limit_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_from_limit_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_from_limit_absent = Query::new();
    branch_range_from_limit_absent.insert_range_from(b"z".to_vec()..);
    subquery_path_range_from_limit_absent_query.set_subquery(branch_range_from_limit_absent);
    let subquery_path_range_from_limit_absent_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_from_limit_absent_query, Some(1), None),
    );
    let subquery_path_range_from_limit_absent_proof = db
        .prove_query(
            &subquery_path_range_from_limit_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_from_limit_absent proof");
    let mut subquery_path_range_query = Query::new();
    subquery_path_range_query.insert_key(b"key".to_vec());
    subquery_path_range_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range = Query::new();
    branch_range.insert_range(b"x".to_vec()..b"y".to_vec());
    subquery_path_range_query.set_subquery(branch_range);
    let subquery_path_range_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_query);
    let subquery_path_range_proof = db
        .prove_query(&subquery_path_range_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path_range proof");
    let mut subquery_path_range_absent_query = Query::new();
    subquery_path_range_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_absent = Query::new();
    branch_range_absent.insert_range(b"w".to_vec()..b"x".to_vec());
    subquery_path_range_absent_query.set_subquery(branch_range_absent);
    let subquery_path_range_absent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_absent_query);
    let subquery_path_range_absent_proof = db
        .prove_query(&subquery_path_range_absent_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path_range_absent proof");
    let mut subquery_path_range_limit_query = Query::new();
    subquery_path_range_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_limit = Query::new();
    branch_range_limit.insert_range(b"a".to_vec()..b"z".to_vec());
    subquery_path_range_limit_query.set_subquery(branch_range_limit);
    let subquery_path_range_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_limit_query, Some(1), None),
    );
    let subquery_path_range_limit_proof = db
        .prove_query(&subquery_path_range_limit_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path_range_limit proof");
    let mut subquery_path_range_inclusive_query = Query::new();
    subquery_path_range_inclusive_query.insert_key(b"key".to_vec());
    subquery_path_range_inclusive_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_inclusive = Query::new();
    branch_range_inclusive.insert_range_inclusive(b"x".to_vec()..=b"y".to_vec());
    subquery_path_range_inclusive_query.set_subquery(branch_range_inclusive);
    let subquery_path_range_inclusive_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_inclusive_query);
    let subquery_path_range_inclusive_proof = db
        .prove_query(
            &subquery_path_range_inclusive_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_inclusive proof");
    let mut subquery_path_range_inclusive_absent_query = Query::new();
    subquery_path_range_inclusive_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_inclusive_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_inclusive_absent = Query::new();
    branch_range_inclusive_absent.insert_range_inclusive(b"w".to_vec()..=b"w".to_vec());
    subquery_path_range_inclusive_absent_query.set_subquery(branch_range_inclusive_absent);
    let subquery_path_range_inclusive_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_inclusive_absent_query,
    );
    let subquery_path_range_inclusive_absent_proof = db
        .prove_query(
            &subquery_path_range_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_inclusive_absent proof");
    let mut subquery_path_range_inclusive_limit_query = Query::new();
    subquery_path_range_inclusive_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_inclusive_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_inclusive_limit = Query::new();
    branch_range_inclusive_limit.insert_range_inclusive(b"x".to_vec()..=b"y".to_vec());
    subquery_path_range_inclusive_limit_query.set_subquery(branch_range_inclusive_limit);
    let subquery_path_range_inclusive_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_inclusive_limit_query, Some(1), None),
    );
    let subquery_path_range_inclusive_limit_proof = db
        .prove_query(
            &subquery_path_range_inclusive_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_inclusive_limit proof");
    let mut subquery_path_range_after_to_query = Query::new();
    subquery_path_range_after_to_query.insert_key(b"key".to_vec());
    subquery_path_range_after_to_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_to = Query::new();
    branch_range_after_to.insert_range_after_to(b"x".to_vec()..b"y".to_vec());
    subquery_path_range_after_to_query.set_subquery(branch_range_after_to);
    let subquery_path_range_after_to_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_range_after_to_query);
    let subquery_path_range_after_to_proof = db
        .prove_query(
            &subquery_path_range_after_to_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_to proof");
    let mut subquery_path_range_after_to_absent_query = Query::new();
    subquery_path_range_after_to_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_after_to_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_to_absent = Query::new();
    branch_range_after_to_absent.insert_range_after_to(b"y".to_vec()..b"z".to_vec());
    subquery_path_range_after_to_absent_query.set_subquery(branch_range_after_to_absent);
    let subquery_path_range_after_to_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_after_to_absent_query,
    );
    let subquery_path_range_after_to_absent_proof = db
        .prove_query(
            &subquery_path_range_after_to_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_to_absent proof");
    let mut subquery_path_range_after_to_limit_query = Query::new();
    subquery_path_range_after_to_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_after_to_limit_query.insert_key(b"key_b".to_vec());
    subquery_path_range_after_to_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_to_limit = Query::new();
    branch_range_after_to_limit.insert_range_after_to(b"x".to_vec()..b"y".to_vec());
    subquery_path_range_after_to_limit_query.set_subquery(branch_range_after_to_limit);
    let subquery_path_range_after_to_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_after_to_limit_query, Some(1), None),
    );
    let subquery_path_range_after_to_limit_proof = db
        .prove_query(
            &subquery_path_range_after_to_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_to_limit proof");
    let mut subquery_path_range_after_to_inclusive_query = Query::new();
    subquery_path_range_after_to_inclusive_query.insert_key(b"key".to_vec());
    subquery_path_range_after_to_inclusive_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_to_inclusive = Query::new();
    branch_range_after_to_inclusive.insert_range_after_to_inclusive(b"x".to_vec()..=b"y".to_vec());
    subquery_path_range_after_to_inclusive_query.set_subquery(branch_range_after_to_inclusive);
    let subquery_path_range_after_to_inclusive_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_after_to_inclusive_query,
    );
    let subquery_path_range_after_to_inclusive_proof = db
        .prove_query(
            &subquery_path_range_after_to_inclusive_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_to_inclusive proof");
    let mut subquery_path_range_after_to_inclusive_absent_query = Query::new();
    subquery_path_range_after_to_inclusive_absent_query.insert_key(b"key".to_vec());
    subquery_path_range_after_to_inclusive_absent_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_to_inclusive_absent = Query::new();
    branch_range_after_to_inclusive_absent
        .insert_range_after_to_inclusive(b"y".to_vec()..=b"y".to_vec());
    subquery_path_range_after_to_inclusive_absent_query
        .set_subquery(branch_range_after_to_inclusive_absent);
    let subquery_path_range_after_to_inclusive_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        subquery_path_range_after_to_inclusive_absent_query,
    );
    let subquery_path_range_after_to_inclusive_absent_proof = db
        .prove_query(
            &subquery_path_range_after_to_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_to_inclusive_absent proof");
    let mut subquery_path_range_after_to_inclusive_limit_query = Query::new();
    subquery_path_range_after_to_inclusive_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_after_to_inclusive_limit_query.insert_key(b"key_b".to_vec());
    subquery_path_range_after_to_inclusive_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    let mut branch_range_after_to_inclusive_limit = Query::new();
    branch_range_after_to_inclusive_limit
        .insert_range_after_to_inclusive(b"x".to_vec()..=b"y".to_vec());
    subquery_path_range_after_to_inclusive_limit_query
        .set_subquery(branch_range_after_to_inclusive_limit);
    let subquery_path_range_after_to_inclusive_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            subquery_path_range_after_to_inclusive_limit_query,
            Some(1),
            None,
        ),
    );
    let subquery_path_range_after_to_inclusive_limit_proof = db
        .prove_query(
            &subquery_path_range_after_to_inclusive_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_after_to_inclusive_limit proof");

    let mut subquery_path_with_parent_query = Query::new();
    subquery_path_with_parent_query.insert_key(b"key".to_vec());
    subquery_path_with_parent_query.set_subquery_path(vec![b"branch".to_vec()]);
    subquery_path_with_parent_query.set_subquery(Query::new_range_full());
    subquery_path_with_parent_query.add_parent_tree_on_subquery = true;
    let subquery_path_with_parent_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_with_parent_query);
    let subquery_path_with_parent_proof = db
        .prove_query(&subquery_path_with_parent_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path_with_parent proof");

    let mut limited_subquery_query = Query::new();
    limited_subquery_query.insert_key(b"key".to_vec());
    limited_subquery_query.insert_key(b"key_b".to_vec());
    limited_subquery_query.set_subquery_path(vec![b"branch".to_vec()]);
    limited_subquery_query.set_subquery(Query::new_range_full());
    let limited_subquery_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(limited_subquery_query, Some(1), None),
    );
    let limited_subquery_proof = db
        .prove_query(&limited_subquery_path_query, None, &grove_version)
        .unwrap()
        .expect("limited_subquery proof");

    let mut limited_desc_subquery_query = Query::new();
    limited_desc_subquery_query.insert_key(b"key".to_vec());
    limited_desc_subquery_query.set_subquery_path(vec![b"branch".to_vec()]);
    limited_desc_subquery_query.set_subquery(Query::new_range_full());
    limited_desc_subquery_query.left_to_right = false;
    let limited_desc_subquery_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(limited_desc_subquery_query, Some(1), None),
    );
    let limited_desc_subquery_proof = db
        .prove_query(&limited_desc_subquery_path_query, None, &grove_version)
        .unwrap()
        .expect("limited_desc_subquery proof");

    let mut reference_chain_wrapped_limit_query = Query::new();
    reference_chain_wrapped_limit_query.insert_key(b"refa".to_vec());
    reference_chain_wrapped_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    reference_chain_wrapped_limit_query.set_subquery(Query::new_range_full());
    reference_chain_wrapped_limit_query.add_parent_tree_on_subquery = true;
    let reference_chain_wrapped_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(reference_chain_wrapped_limit_query, Some(1), None),
    );
    let reference_chain_wrapped_limit_proof = db
        .prove_query(
            &reference_chain_wrapped_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("reference_chain_wrapped_limit proof");

    let mut reference_cycle_query = Query::new();
    reference_cycle_query.insert_key(b"ref_cycle_a".to_vec());
    reference_cycle_query.set_subquery(Query::new_range_full());
    let reference_cycle_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec(), b"refs".to_vec()],
        reference_cycle_query,
    );
    let reference_cycle_result = db
        .prove_query(&reference_cycle_path_query, None, &grove_version)
        .unwrap();
    assert!(
        reference_cycle_result.is_err(),
        "reference_cycle proof should fail at Rust proving stage"
    );

    let mut reference_hop_limit_query = Query::new();
    reference_hop_limit_query.insert_key(b"ref_hop_a".to_vec());
    reference_hop_limit_query.set_subquery(Query::new_range_full());
    reference_hop_limit_query.add_parent_tree_on_subquery = true;
    let reference_hop_limit_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec(), b"refs".to_vec()],
        reference_hop_limit_query,
    );
    let reference_hop_limit_proof = db
        .prove_query(&reference_hop_limit_path_query, None, &grove_version)
        .unwrap()
        .expect("reference_hop_limit proof");

    // Ensure proof contains both reference nodes so verifier reference-chain
    // traversal sees an intermediate hop-limited reference value.
    let mut reference_hop_limit_materialized_chain_query = Query::new();
    reference_hop_limit_materialized_chain_query.insert_key(b"ref_hop_a".to_vec());
    reference_hop_limit_materialized_chain_query.insert_key(b"ref_hop_b".to_vec());
    reference_hop_limit_materialized_chain_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"ref_hop_a".to_vec()),
        None,
        Some(Query::new_range_full()),
    );
    reference_hop_limit_materialized_chain_query.add_parent_tree_on_subquery = true;
    let reference_hop_limit_materialized_chain_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec(), b"refs".to_vec()],
        reference_hop_limit_materialized_chain_query,
    );
    let reference_hop_limit_materialized_chain_proof = db
        .prove_query(
            &reference_hop_limit_materialized_chain_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("reference_hop_limit_materialized_chain proof");

    // Reference with conditional subquery + limit: queries ref_hop_a (which
    // references key/branch with 1 hop) with a conditional subquery that
    // filters on the resolved target and applies a limit. This exercises
    // reference resolution followed by conditional subquery limit enforcement.
    let mut reference_conditional_subquery_limit_query = Query::new();
    reference_conditional_subquery_limit_query.insert_key(b"ref_hop_a".to_vec());
    reference_conditional_subquery_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"ref_hop_a".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(Query::new_range_full()),
    );
    reference_conditional_subquery_limit_query.add_parent_tree_on_subquery = true;
    let reference_conditional_subquery_limit_path_query = PathQuery::new(
        vec![b"root".to_vec(), b"refs".to_vec()],
        SizedQuery::new(reference_conditional_subquery_limit_query, Some(1), None),
    );
    let reference_conditional_subquery_limit_proof = db
        .prove_query(
            &reference_conditional_subquery_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("reference_conditional_subquery_limit proof");

    let mut conditional_subquery_path_query = Query::new();
    conditional_subquery_path_query.insert_key(b"key".to_vec());
    conditional_subquery_path_query.insert_key(b"k2".to_vec());
    conditional_subquery_path_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(Query::new_range_full()),
    );
    let conditional_subquery_path_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], conditional_subquery_path_query);
    let conditional_subquery_path_proof = db
        .prove_query(&conditional_subquery_path_path_query, None, &grove_version)
        .unwrap()
        .expect("conditional_subquery_path proof");
    let mut conditional_subquery_path_range_after_query = Query::new();
    conditional_subquery_path_range_after_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_query.insert_key(b"k2".to_vec());
    let mut conditional_range_after_inner = Query::new();
    conditional_range_after_inner.insert_range_after(b"x".to_vec()..);
    conditional_subquery_path_range_after_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_inner),
    );
    let conditional_subquery_path_range_after_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_after_query,
    );
    let conditional_subquery_path_range_after_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after proof");
    let mut conditional_subquery_path_range_after_absent_query = Query::new();
    conditional_subquery_path_range_after_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_after_absent_inner = Query::new();
    conditional_range_after_absent_inner.insert_range_after(b"y".to_vec()..);
    conditional_subquery_path_range_after_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_absent_inner),
    );
    let conditional_subquery_path_range_after_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_after_absent_query,
    );
    let conditional_subquery_path_range_after_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_absent proof");
    let mut conditional_subquery_path_range_after_limit_query = Query::new();
    conditional_subquery_path_range_after_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_after_limit_inner = Query::new();
    conditional_range_after_limit_inner.insert_range_after(b"x".to_vec()..);
    conditional_subquery_path_range_after_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_limit_inner),
    );
    let conditional_subquery_path_range_after_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_after_limit_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_after_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_limit proof");
    let mut conditional_subquery_path_range_after_limit_absent_query = Query::new();
    conditional_subquery_path_range_after_limit_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_limit_absent_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_after_limit_absent_inner = Query::new();
    conditional_range_after_limit_absent_inner.insert_range_after(b"z".to_vec()..);
    conditional_subquery_path_range_after_limit_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_limit_absent_inner),
    );
    let conditional_subquery_path_range_after_limit_absent_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_after_limit_absent_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_after_limit_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_limit_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_limit_absent proof");
    let mut conditional_subquery_path_range_to_inclusive_query = Query::new();
    conditional_subquery_path_range_to_inclusive_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_to_inclusive_query.insert_key(b"k2".to_vec());
    let mut conditional_range_to_inclusive_inner = Query::new();
    conditional_range_to_inclusive_inner.insert_range_to_inclusive(..=b"x".to_vec());
    conditional_subquery_path_range_to_inclusive_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_to_inclusive_inner),
    );
    let conditional_subquery_path_range_to_inclusive_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_to_inclusive_query,
    );
    let conditional_subquery_path_range_to_inclusive_proof = db
        .prove_query(
            &conditional_subquery_path_range_to_inclusive_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_to_inclusive proof");
    let mut conditional_subquery_path_range_to_inclusive_absent_query = Query::new();
    conditional_subquery_path_range_to_inclusive_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_to_inclusive_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_to_inclusive_absent_inner = Query::new();
    conditional_range_to_inclusive_absent_inner.insert_range_to_inclusive(..=b"w".to_vec());
    conditional_subquery_path_range_to_inclusive_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_to_inclusive_absent_inner),
    );
    let conditional_subquery_path_range_to_inclusive_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_to_inclusive_absent_query,
    );
    let conditional_subquery_path_range_to_inclusive_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_to_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_to_inclusive_absent proof");
    let mut conditional_subquery_path_range_from_query = Query::new();
    conditional_subquery_path_range_from_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_from_query.insert_key(b"k2".to_vec());
    let mut conditional_range_from_inner = Query::new();
    conditional_range_from_inner.insert_range_from(b"y".to_vec()..);
    conditional_subquery_path_range_from_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_from_inner),
    );
    let conditional_subquery_path_range_from_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_from_query,
    );
    let conditional_subquery_path_range_from_proof = db
        .prove_query(
            &conditional_subquery_path_range_from_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_from proof");
    let mut conditional_subquery_path_range_from_absent_query = Query::new();
    conditional_subquery_path_range_from_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_from_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_from_absent_inner = Query::new();
    conditional_range_from_absent_inner.insert_range_from(b"z".to_vec()..);
    conditional_subquery_path_range_from_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_from_absent_inner),
    );
    let conditional_subquery_path_range_from_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_from_absent_query,
    );
    let conditional_subquery_path_range_from_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_from_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_from_absent proof");
    let mut conditional_subquery_path_range_to_query = Query::new();
    conditional_subquery_path_range_to_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_to_query.insert_key(b"k2".to_vec());
    let mut conditional_range_to_inner = Query::new();
    conditional_range_to_inner.insert_range_to(..b"y".to_vec());
    conditional_subquery_path_range_to_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_to_inner),
    );
    let conditional_subquery_path_range_to_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_to_query,
    );
    let conditional_subquery_path_range_to_proof = db
        .prove_query(
            &conditional_subquery_path_range_to_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_to proof");
    let mut conditional_subquery_path_range_to_absent_query = Query::new();
    conditional_subquery_path_range_to_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_to_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_to_absent_inner = Query::new();
    conditional_range_to_absent_inner.insert_range_to(..b"x".to_vec());
    conditional_subquery_path_range_to_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_to_absent_inner),
    );
    let conditional_subquery_path_range_to_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_to_absent_query,
    );
    let conditional_subquery_path_range_to_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_to_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_to_absent proof");
    let mut conditional_subquery_path_range_query = Query::new();
    conditional_subquery_path_range_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_query.insert_key(b"k2".to_vec());
    let mut conditional_range_inner = Query::new();
    conditional_range_inner.insert_range(b"x".to_vec()..b"y".to_vec());
    conditional_subquery_path_range_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_inner),
    );
    let conditional_subquery_path_range_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_query,
    );
    let conditional_subquery_path_range_proof = db
        .prove_query(
            &conditional_subquery_path_range_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range proof");
    let mut conditional_subquery_path_range_absent_query = Query::new();
    conditional_subquery_path_range_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_absent_inner = Query::new();
    conditional_range_absent_inner.insert_range(b"w".to_vec()..b"x".to_vec());
    conditional_subquery_path_range_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_absent_inner),
    );
    let conditional_subquery_path_range_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_absent_query,
    );
    let conditional_subquery_path_range_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_absent proof");
    let mut conditional_subquery_path_range_inclusive_query = Query::new();
    conditional_subquery_path_range_inclusive_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_inclusive_query.insert_key(b"k2".to_vec());
    let mut conditional_range_inclusive_inner = Query::new();
    conditional_range_inclusive_inner.insert_range_inclusive(b"x".to_vec()..=b"y".to_vec());
    conditional_subquery_path_range_inclusive_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_inclusive_inner),
    );
    let conditional_subquery_path_range_inclusive_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_inclusive_query,
    );
    let conditional_subquery_path_range_inclusive_proof = db
        .prove_query(
            &conditional_subquery_path_range_inclusive_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_inclusive proof");
    let mut conditional_subquery_path_range_inclusive_absent_query = Query::new();
    conditional_subquery_path_range_inclusive_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_inclusive_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_inclusive_absent_inner = Query::new();
    conditional_range_inclusive_absent_inner.insert_range_inclusive(b"w".to_vec()..=b"w".to_vec());
    conditional_subquery_path_range_inclusive_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_inclusive_absent_inner),
    );
    let conditional_subquery_path_range_inclusive_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_inclusive_absent_query,
    );
    let conditional_subquery_path_range_inclusive_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_inclusive_absent proof");
    let mut conditional_subquery_path_range_after_to_query = Query::new();
    conditional_subquery_path_range_after_to_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_to_query.insert_key(b"k2".to_vec());
    let mut conditional_range_after_to_inner = Query::new();
    conditional_range_after_to_inner.insert_range_after_to(b"x".to_vec()..b"y".to_vec());
    conditional_subquery_path_range_after_to_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_to_inner),
    );
    let conditional_subquery_path_range_after_to_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_after_to_query,
    );
    let conditional_subquery_path_range_after_to_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_to_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_to proof");
    let mut conditional_subquery_path_range_after_to_absent_query = Query::new();
    conditional_subquery_path_range_after_to_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_to_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_after_to_absent_inner = Query::new();
    conditional_range_after_to_absent_inner.insert_range_after_to(b"y".to_vec()..b"z".to_vec());
    conditional_subquery_path_range_after_to_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_to_absent_inner),
    );
    let conditional_subquery_path_range_after_to_absent_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_after_to_absent_query,
    );
    let conditional_subquery_path_range_after_to_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_to_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_to_absent proof");
    let mut conditional_subquery_path_range_after_to_inclusive_query = Query::new();
    conditional_subquery_path_range_after_to_inclusive_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_to_inclusive_query.insert_key(b"k2".to_vec());
    let mut conditional_range_after_to_inclusive_inner = Query::new();
    conditional_range_after_to_inclusive_inner
        .insert_range_after_to_inclusive(b"x".to_vec()..=b"y".to_vec());
    conditional_subquery_path_range_after_to_inclusive_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_to_inclusive_inner),
    );
    let conditional_subquery_path_range_after_to_inclusive_path_query = PathQuery::new_unsized(
        vec![b"root".to_vec()],
        conditional_subquery_path_range_after_to_inclusive_query,
    );
    let conditional_subquery_path_range_after_to_inclusive_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_to_inclusive_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_to_inclusive proof");
    let mut conditional_subquery_path_range_after_to_inclusive_absent_query = Query::new();
    conditional_subquery_path_range_after_to_inclusive_absent_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_to_inclusive_absent_query.insert_key(b"k2".to_vec());
    let mut conditional_range_after_to_inclusive_absent_inner = Query::new();
    conditional_range_after_to_inclusive_absent_inner
        .insert_range_after_to_inclusive(b"y".to_vec()..=b"y".to_vec());
    conditional_subquery_path_range_after_to_inclusive_absent_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_to_inclusive_absent_inner),
    );
    let conditional_subquery_path_range_after_to_inclusive_absent_path_query =
        PathQuery::new_unsized(
            vec![b"root".to_vec()],
            conditional_subquery_path_range_after_to_inclusive_absent_query,
        );
    let conditional_subquery_path_range_after_to_inclusive_absent_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_to_inclusive_absent_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_to_inclusive_absent proof");
    let mut conditional_subquery_path_range_to_inclusive_limit_query = Query::new();
    conditional_subquery_path_range_to_inclusive_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_to_inclusive_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_to_inclusive_limit_inner = Query::new();
    conditional_range_to_inclusive_limit_inner.insert_range_to_inclusive(..=b"x".to_vec());
    conditional_subquery_path_range_to_inclusive_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_to_inclusive_limit_inner),
    );
    let conditional_subquery_path_range_to_inclusive_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_to_inclusive_limit_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_to_inclusive_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_to_inclusive_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_to_inclusive_limit proof");
    let mut conditional_subquery_path_range_from_limit_query = Query::new();
    conditional_subquery_path_range_from_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_from_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_from_limit_inner = Query::new();
    conditional_range_from_limit_inner.insert_range_from(b"y".to_vec()..);
    conditional_subquery_path_range_from_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_from_limit_inner),
    );
    let conditional_subquery_path_range_from_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_from_limit_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_from_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_from_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_from_limit proof");
    let mut conditional_subquery_path_range_to_limit_query = Query::new();
    conditional_subquery_path_range_to_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_to_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_to_limit_inner = Query::new();
    conditional_range_to_limit_inner.insert_range_to(..b"y".to_vec());
    conditional_subquery_path_range_to_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_to_limit_inner),
    );
    let conditional_subquery_path_range_to_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_to_limit_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_to_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_to_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_to_limit proof");
    let mut conditional_subquery_path_range_limit_query = Query::new();
    conditional_subquery_path_range_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_limit_inner = Query::new();
    conditional_range_limit_inner.insert_range(b"x".to_vec()..b"y".to_vec());
    conditional_subquery_path_range_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_limit_inner),
    );
    let conditional_subquery_path_range_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(conditional_subquery_path_range_limit_query, Some(1), None),
    );
    let conditional_subquery_path_range_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_limit proof");
    let mut conditional_subquery_path_range_inclusive_limit_query = Query::new();
    conditional_subquery_path_range_inclusive_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_inclusive_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_inclusive_limit_inner = Query::new();
    conditional_range_inclusive_limit_inner.insert_range_inclusive(b"x".to_vec()..=b"y".to_vec());
    conditional_subquery_path_range_inclusive_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_inclusive_limit_inner),
    );
    let conditional_subquery_path_range_inclusive_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_inclusive_limit_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_inclusive_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_inclusive_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_inclusive_limit proof");
    let mut conditional_subquery_path_range_after_to_limit_query = Query::new();
    conditional_subquery_path_range_after_to_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_to_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_after_to_limit_inner = Query::new();
    conditional_range_after_to_limit_inner.insert_range_after_to(b"x".to_vec()..b"y".to_vec());
    conditional_subquery_path_range_after_to_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_to_limit_inner),
    );
    let conditional_subquery_path_range_after_to_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_after_to_limit_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_after_to_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_to_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_to_limit proof");
    let mut conditional_subquery_path_range_after_to_inclusive_limit_query = Query::new();
    conditional_subquery_path_range_after_to_inclusive_limit_query.insert_key(b"key".to_vec());
    conditional_subquery_path_range_after_to_inclusive_limit_query.insert_key(b"key_b".to_vec());
    let mut conditional_range_after_to_inclusive_limit_inner = Query::new();
    conditional_range_after_to_inclusive_limit_inner
        .insert_range_after_to_inclusive(b"x".to_vec()..=b"y".to_vec());
    conditional_subquery_path_range_after_to_inclusive_limit_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"key".to_vec()),
        Some(vec![b"branch".to_vec()]),
        Some(conditional_range_after_to_inclusive_limit_inner),
    );
    let conditional_subquery_path_range_after_to_inclusive_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(
            conditional_subquery_path_range_after_to_inclusive_limit_query,
            Some(1),
            None,
        ),
    );
    let conditional_subquery_path_range_after_to_inclusive_limit_proof = db
        .prove_query(
            &conditional_subquery_path_range_after_to_inclusive_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("conditional_subquery_path_range_after_to_inclusive_limit proof");

    let mut nested_conditional_query = Query::new();
    nested_conditional_query.insert_key(b"key".to_vec());
    nested_conditional_query.set_subquery_path(vec![b"nest".to_vec()]);
    let mut nested_inner_query = Query::new_range_full();
    nested_inner_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"m".to_vec()),
        None,
        Some(Query::new_range_full()),
    );
    nested_conditional_query.set_subquery(nested_inner_query);
    let nested_conditional_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], nested_conditional_query);
    let nested_conditional_proof = db
        .prove_query(&nested_conditional_path_query, None, &grove_version)
        .unwrap()
        .expect("nested_conditional proof");

    let mut reference_nested_conditional_query = Query::new();
    reference_nested_conditional_query.insert_key(b"refa".to_vec());
    reference_nested_conditional_query.set_subquery_path(vec![b"nest".to_vec()]);
    let mut reference_nested_inner_query = Query::new_range_full();
    reference_nested_inner_query.add_conditional_subquery(
        grovedb_merk::proofs::query::query_item::QueryItem::Key(b"m".to_vec()),
        None,
        Some(Query::new_range_full()),
    );
    reference_nested_conditional_query.set_subquery(reference_nested_inner_query);
    let reference_nested_conditional_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], reference_nested_conditional_query);
    let reference_nested_conditional_proof = db
        .prove_query(
            &reference_nested_conditional_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("reference_nested_conditional proof");

    // Non-conditional subquery_path with range_to_inclusive + limit — fills a
    // combinatorial gap (only range_after has a non-conditional limit variant).
    let mut subquery_path_range_to_inclusive_limit_query = Query::new();
    subquery_path_range_to_inclusive_limit_query.insert_key(b"key".to_vec());
    subquery_path_range_to_inclusive_limit_query.insert_key(b"key_b".to_vec());
    subquery_path_range_to_inclusive_limit_query.set_subquery_path(vec![b"branch".to_vec()]);
    subquery_path_range_to_inclusive_limit_query.set_subquery(Query::new_single_query_item(
        grovedb_merk::proofs::query::query_item::QueryItem::RangeToInclusive(..=b"y".to_vec()),
    ));
    let subquery_path_range_to_inclusive_limit_path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(subquery_path_range_to_inclusive_limit_query, Some(1), None),
    );
    let subquery_path_range_to_inclusive_limit_proof = db
        .prove_query(
            &subquery_path_range_to_inclusive_limit_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("subquery_path_range_to_inclusive_limit proof");

    let reference_empty_key_fixture_path = db_path.join("reference_empty_key_sibling_fixture_db");
    let _ = fs::remove_dir_all(&reference_empty_key_fixture_path);
    let reference_empty_key_fixture_db = GroveDb::open(&reference_empty_key_fixture_path)
        .expect("open empty-key reference fixture db");
    reference_empty_key_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"root",
            Element::Tree(None, None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert empty-key fixture root tree");
    reference_empty_key_fixture_db
        .insert(
            &[b"root".as_slice()],
            b"key",
            Element::Tree(None, None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert empty-key fixture key tree");
    reference_empty_key_fixture_db
        .insert(
            &[b"root".as_slice(), b"key".as_slice()],
            b"branch",
            Element::Tree(None, None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert empty-key fixture branch tree");
    reference_empty_key_fixture_db
        .insert(
            &[b"root".as_slice(), b"key".as_slice(), b"branch".as_slice()],
            b"x",
            Element::Item(b"9".to_vec(), None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert empty-key fixture x");
    reference_empty_key_fixture_db
        .insert(
            &[b"root".as_slice(), b"key".as_slice(), b"branch".as_slice()],
            b"y",
            Element::Item(b"8".to_vec(), None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert empty-key fixture y");
    reference_empty_key_fixture_db
        .insert(
            &[b"root".as_slice()],
            b"",
            Element::new_reference(ReferencePathType::SiblingReference(b"key".to_vec())),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert empty-key fixture reference");

    let mut reference_empty_key_sibling_query = Query::new();
    reference_empty_key_sibling_query.insert_key(Vec::new());
    reference_empty_key_sibling_query.set_subquery_path(vec![b"branch".to_vec()]);
    reference_empty_key_sibling_query.set_subquery(Query::new_range_full());
    let reference_empty_key_sibling_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], reference_empty_key_sibling_query);
    let reference_empty_key_sibling_proof = reference_empty_key_fixture_db
        .prove_query(
            &reference_empty_key_sibling_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("reference_empty_key_sibling proof");
    let reference_empty_key_sibling_root_hash = reference_empty_key_fixture_db
        .root_hash(TransactionArg::None, &grove_version)
        .unwrap()
        .expect("reference_empty_key_sibling root hash");

    let root_hash = db
        .root_hash(TransactionArg::None, &grove_version)
        .unwrap()
        .expect("root hash");

    let prove_query_fixture_path = db_path.join("grove_prove_query_for_version_fixture_db");
    let _ = fs::remove_dir_all(&prove_query_fixture_path);
    let prove_query_fixture_db =
        GroveDb::open(&prove_query_fixture_path).expect("open prove_query fixture db");
    prove_query_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"k1",
            Element::Item(b"v1".to_vec(), None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert prove_query fixture item");
    let prove_query_fixture_query = PathQuery::new_single_key(Vec::new(), b"k1".to_vec());
    let prove_query_fixture_proof = prove_query_fixture_db
        .prove_query(&prove_query_fixture_query, None, &grove_version)
        .unwrap()
        .expect("prove_query fixture proof");
    let prove_query_fixture_root_hash = prove_query_fixture_db
        .root_hash(TransactionArg::None, &grove_version)
        .unwrap()
        .expect("prove_query fixture root hash");

    let prove_query_layered_fixture_path =
        db_path.join("grove_prove_query_for_version_layered_fixture_db");
    let _ = fs::remove_dir_all(&prove_query_layered_fixture_path);
    let prove_query_layered_fixture_db = GroveDb::open(&prove_query_layered_fixture_path)
        .expect("open layered prove_query fixture db");
    prove_query_layered_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"root",
            Element::Tree(None, None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert layered fixture root tree");
    prove_query_layered_fixture_db
        .insert(
            &[b"root".as_slice()],
            b"key",
            Element::Tree(None, None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert layered fixture key tree");
    prove_query_layered_fixture_db
        .insert(
            &[b"root".as_slice(), b"key".as_slice()],
            b"branch",
            Element::Tree(None, None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert layered fixture branch tree");
    prove_query_layered_fixture_db
        .insert(
            &[b"root".as_slice(), b"key".as_slice(), b"branch".as_slice()],
            b"x",
            Element::Item(b"9".to_vec(), None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert layered fixture branch/x");
    prove_query_layered_fixture_db
        .insert(
            &[b"root".as_slice(), b"key".as_slice(), b"branch".as_slice()],
            b"y",
            Element::Item(b"8".to_vec(), None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert layered fixture branch/y");
    let mut prove_query_layered_fixture_query = Query::new();
    prove_query_layered_fixture_query.insert_key(b"key".to_vec());
    prove_query_layered_fixture_query.set_subquery_path(vec![b"branch".to_vec()]);
    prove_query_layered_fixture_query.set_subquery(Query::new_single_key(b"x".to_vec()));
    let prove_query_layered_fixture_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], prove_query_layered_fixture_query);
    let prove_query_layered_fixture_proof = prove_query_layered_fixture_db
        .prove_query(
            &prove_query_layered_fixture_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("prove_query layered fixture proof");
    let prove_query_layered_fixture_root_hash = prove_query_layered_fixture_db
        .root_hash(TransactionArg::None, &grove_version)
        .unwrap()
        .expect("prove_query layered fixture root hash");

    let aggregate_proof_fixture_path = db_path.join("grove_proof_aggregate_fixture_db");
    let _ = fs::remove_dir_all(&aggregate_proof_fixture_path);
    let aggregate_proof_fixture_db =
        GroveDb::open(&aggregate_proof_fixture_path).expect("open aggregate proof fixture db");
    aggregate_proof_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"sum_tree",
            Element::new_sum_tree(None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert aggregate fixture sum_tree");
    aggregate_proof_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"count_tree",
            Element::new_count_tree(None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert aggregate fixture count_tree");
    aggregate_proof_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"big_sum_tree",
            Element::new_big_sum_tree(None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert aggregate fixture big_sum_tree");
    aggregate_proof_fixture_db
        .insert(
            &[b"sum_tree".as_slice()],
            b"s1",
            Element::new_sum_item(3),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert aggregate fixture sum_tree/s1");
    aggregate_proof_fixture_db
        .insert(
            &[b"sum_tree".as_slice()],
            b"s2",
            Element::new_sum_item(4),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert aggregate fixture sum_tree/s2");
    aggregate_proof_fixture_db
        .insert(
            &[b"count_tree".as_slice()],
            b"c1",
            Element::new_item(b"v1".to_vec()),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert aggregate fixture count_tree/c1");
    aggregate_proof_fixture_db
        .insert(
            &[b"count_tree".as_slice()],
            b"c2",
            Element::new_item(b"v2".to_vec()),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert aggregate fixture count_tree/c2");
    let mut aggregate_proof_fixture_query = Query::new();
    aggregate_proof_fixture_query.insert_key(b"sum_tree".to_vec());
    aggregate_proof_fixture_query.insert_key(b"count_tree".to_vec());
    aggregate_proof_fixture_query.insert_key(b"big_sum_tree".to_vec());
    let aggregate_proof_fixture_path_query =
        PathQuery::new_unsized(Vec::<Vec<u8>>::new(), aggregate_proof_fixture_query);
    let aggregate_proof_fixture_proof = aggregate_proof_fixture_db
        .prove_query(
            &aggregate_proof_fixture_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("aggregate proof fixture proof");
    let aggregate_proof_fixture_root_hash = aggregate_proof_fixture_db
        .root_hash(TransactionArg::None, &grove_version)
        .unwrap()
        .expect("aggregate proof fixture root hash");

    let absence_terminal_fixture_path = db_path.join("grove_proof_absence_terminal_fixture_db");
    let _ = fs::remove_dir_all(&absence_terminal_fixture_path);
    let absence_terminal_fixture_db = GroveDb::open(&absence_terminal_fixture_path)
        .expect("open absence terminal fixture db");
    absence_terminal_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"a",
            Element::Item(b"va".to_vec(), None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert absence terminal fixture a");
    absence_terminal_fixture_db
        .insert(
            &[] as &[&[u8]],
            b"c",
            Element::Item(b"vc".to_vec(), None),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert absence terminal fixture c");
    let mut absence_terminal_query = Query::new();
    absence_terminal_query.insert_range_inclusive(b"a".to_vec()..=b"c".to_vec());
    let absence_terminal_path_query =
        PathQuery::new_unsized(Vec::<Vec<u8>>::new(), absence_terminal_query);
    let absence_terminal_proof = absence_terminal_fixture_db
        .prove_query(
            &absence_terminal_path_query,
            None,
            &grove_version,
        )
        .unwrap()
        .expect("absence terminal fixture proof");
    let absence_terminal_root_hash = absence_terminal_fixture_db
        .root_hash(TransactionArg::None, &grove_version)
        .unwrap()
        .expect("absence terminal fixture root hash");

    let prove_query_limit_fixture_path = db_path.join("grove_prove_query_limit_fixture_db");
    let _ = fs::remove_dir_all(&prove_query_limit_fixture_path);
    let prove_query_limit_fixture_db =
        GroveDb::open(&prove_query_limit_fixture_path).expect("open prove_query limit fixture db");
    for (k, v) in [(b"a".as_slice(), b"va".as_slice()), (b"b".as_slice(), b"vb".as_slice()), (b"c".as_slice(), b"vc".as_slice())] {
        prove_query_limit_fixture_db
            .insert(
                &[] as &[&[u8]],
                k,
                Element::Item(v.to_vec(), None),
                None,
                TransactionArg::None,
                &grove_version,
            )
            .unwrap()
            .expect("insert prove_query limit fixture item");
    }
    let mut prove_query_limit_query = Query::new();
    prove_query_limit_query.insert_range_inclusive(b"a".to_vec()..=b"c".to_vec());
    let prove_query_limit_path_query =
        PathQuery::new(Vec::<Vec<u8>>::new(), SizedQuery::new(prove_query_limit_query, Some(2), None));
    let prove_query_limit_fixture_proof = prove_query_limit_fixture_db
        .prove_query(&prove_query_limit_path_query, None, &grove_version)
        .unwrap()
        .expect("prove_query limit fixture proof");
    let prove_query_limit_fixture_root_hash = prove_query_limit_fixture_db
        .root_hash(TransactionArg::None, &grove_version)
        .unwrap()
        .expect("prove_query limit fixture root hash");

    write_file(db_path, "grove_proof_present.bin", &present_proof);
    write_file(db_path, "grove_proof_absent.bin", &absent_proof);
    write_file(
        db_path,
        "grove_proof_prove_query_for_version_rust.bin",
        &prove_query_fixture_proof,
    );
    write_file(
        db_path,
        "grove_proof_prove_query_for_version_root_hash.bin",
        &prove_query_fixture_root_hash,
    );
    write_file(
        db_path,
        "grove_proof_prove_query_for_version_layered_rust.bin",
        &prove_query_layered_fixture_proof,
    );
    write_file(
        db_path,
        "grove_proof_prove_query_for_version_layered_root_hash.bin",
        &prove_query_layered_fixture_root_hash,
    );
    write_file(
        db_path,
        "grove_proof_prove_query_for_version_limit_rust.bin",
        &prove_query_limit_fixture_proof,
    );
    write_file(
        db_path,
        "grove_proof_prove_query_for_version_limit_root_hash.bin",
        &prove_query_limit_fixture_root_hash,
    );
    write_file(
        db_path,
        "grove_proof_aggregate_trees.bin",
        &aggregate_proof_fixture_proof,
    );
    write_file(
        db_path,
        "grove_proof_aggregate_trees_root_hash.bin",
        &aggregate_proof_fixture_root_hash,
    );
    write_file(
        db_path,
        "grove_proof_absence_terminal_range.bin",
        &absence_terminal_proof,
    );
    write_file(
        db_path,
        "grove_proof_absence_terminal_range_root_hash.bin",
        &absence_terminal_root_hash,
    );
    write_file(db_path, "grove_proof_root_range.bin", &root_range_proof);
    write_file(
        db_path,
        "grove_proof_root_range_desc.bin",
        &root_range_desc_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_exclusive.bin",
        &root_range_exclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_to_inclusive.bin",
        &root_range_to_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_after.bin",
        &root_range_after_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_to.bin",
        &root_range_to_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_from.bin",
        &root_range_from_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_after_to.bin",
        &root_range_after_to_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_after_to_inclusive.bin",
        &root_range_after_to_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_full.bin",
        &root_range_full_proof,
    );
    write_file(
        db_path,
        "grove_proof_nested_path_range_full.bin",
        &nested_path_range_full_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_multi_key.bin",
        &root_multi_key_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_to_absent.bin",
        &root_range_to_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_to_inclusive_absent.bin",
        &root_range_to_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_from_absent.bin",
        &root_range_from_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_after_absent.bin",
        &root_range_after_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_absent.bin",
        &root_range_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_inclusive_absent.bin",
        &root_range_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_after_to_absent.bin",
        &root_range_after_to_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_root_range_after_to_inclusive_absent.bin",
        &root_range_after_to_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_without_parent.bin",
        &subquery_without_parent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_with_parent.bin",
        &subquery_with_parent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery.bin",
        &conditional_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path.bin",
        &subquery_path_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after.bin",
        &subquery_path_range_after_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_absent.bin",
        &subquery_path_range_after_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_limit.bin",
        &subquery_path_range_after_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_to_inclusive.bin",
        &subquery_path_range_to_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_to_inclusive_absent.bin",
        &subquery_path_range_to_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_to.bin",
        &subquery_path_range_to_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_to_absent.bin",
        &subquery_path_range_to_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_to_limit.bin",
        &subquery_path_range_to_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_from.bin",
        &subquery_path_range_from_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_from_limit.bin",
        &subquery_path_range_from_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_from_absent.bin",
        &subquery_path_range_from_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_from_limit_absent.bin",
        &subquery_path_range_from_limit_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range.bin",
        &subquery_path_range_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_absent.bin",
        &subquery_path_range_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_limit.bin",
        &subquery_path_range_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_inclusive.bin",
        &subquery_path_range_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_inclusive_absent.bin",
        &subquery_path_range_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_inclusive_limit.bin",
        &subquery_path_range_inclusive_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_to.bin",
        &subquery_path_range_after_to_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_to_absent.bin",
        &subquery_path_range_after_to_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_to_limit.bin",
        &subquery_path_range_after_to_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_to_inclusive.bin",
        &subquery_path_range_after_to_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_to_inclusive_absent.bin",
        &subquery_path_range_after_to_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_after_to_inclusive_limit.bin",
        &subquery_path_range_after_to_inclusive_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_with_parent.bin",
        &subquery_path_with_parent_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_limit.bin",
        &limited_subquery_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_limit_desc.bin",
        &limited_desc_subquery_proof,
    );
    write_file(
        db_path,
        "grove_proof_reference_chain_wrapped_limit.bin",
        &reference_chain_wrapped_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_reference_hop_limit.bin",
        &reference_hop_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_reference_hop_limit_materialized_chain.bin",
        &reference_hop_limit_materialized_chain_proof,
    );
    write_file(
        db_path,
        "grove_proof_reference_conditional_subquery_limit.bin",
        &reference_conditional_subquery_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_reference_empty_key_sibling.bin",
        &reference_empty_key_sibling_proof,
    );
    write_file(
        db_path,
        "grove_proof_reference_empty_key_sibling_root_hash.bin",
        &reference_empty_key_sibling_root_hash,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path.bin",
        &conditional_subquery_path_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after.bin",
        &conditional_subquery_path_range_after_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_absent.bin",
        &conditional_subquery_path_range_after_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_limit.bin",
        &conditional_subquery_path_range_after_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_limit_absent.bin",
        &conditional_subquery_path_range_after_limit_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_to_inclusive.bin",
        &conditional_subquery_path_range_to_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_to_inclusive_absent.bin",
        &conditional_subquery_path_range_to_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_from.bin",
        &conditional_subquery_path_range_from_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_from_absent.bin",
        &conditional_subquery_path_range_from_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_to.bin",
        &conditional_subquery_path_range_to_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_to_absent.bin",
        &conditional_subquery_path_range_to_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range.bin",
        &conditional_subquery_path_range_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_absent.bin",
        &conditional_subquery_path_range_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_inclusive.bin",
        &conditional_subquery_path_range_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_inclusive_absent.bin",
        &conditional_subquery_path_range_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_to.bin",
        &conditional_subquery_path_range_after_to_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_to_absent.bin",
        &conditional_subquery_path_range_after_to_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_to_inclusive.bin",
        &conditional_subquery_path_range_after_to_inclusive_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_to_inclusive_absent.bin",
        &conditional_subquery_path_range_after_to_inclusive_absent_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_to_inclusive_limit.bin",
        &conditional_subquery_path_range_to_inclusive_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_from_limit.bin",
        &conditional_subquery_path_range_from_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_to_limit.bin",
        &conditional_subquery_path_range_to_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_limit.bin",
        &conditional_subquery_path_range_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_inclusive_limit.bin",
        &conditional_subquery_path_range_inclusive_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_to_limit.bin",
        &conditional_subquery_path_range_after_to_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_conditional_subquery_path_range_after_to_inclusive_limit.bin",
        &conditional_subquery_path_range_after_to_inclusive_limit_proof,
    );
    write_file(
        db_path,
        "grove_proof_nested_conditional_subquery.bin",
        &nested_conditional_proof,
    );
    write_file(
        db_path,
        "grove_proof_reference_nested_conditional_subquery.bin",
        &reference_nested_conditional_proof,
    );
    write_file(
        db_path,
        "grove_proof_subquery_path_range_to_inclusive_limit.bin",
        &subquery_path_range_to_inclusive_limit_proof,
    );

    // Reference-chain subset verification fixture - validates subset proof verification
    // across reference chain with multiple hops (ref_hop_a -> ref_hop_b -> target)
    // This tests VerifySubsetQuery with reference resolution chain
    let mut reference_chain_subset_query = Query::new();
    reference_chain_subset_query.insert_key(b"ref_hop_a".to_vec());
    reference_chain_subset_query.insert_key(b"ref_hop_b".to_vec());
    let reference_chain_subset_path_query = PathQuery::new(
        vec![b"root".to_vec(), b"refs".to_vec()],
        SizedQuery::new(reference_chain_subset_query, None, None),
    );
    let reference_chain_subset_proof = db
        .prove_query(&reference_chain_subset_path_query, None, &grove_version)
        .value
        .expect("reference_chain_subset proof");
    write_file(
        db_path,
        "grove_proof_reference_chain_subset.bin",
        &reference_chain_subset_proof,
    );

    // verify_query_get_parent_tree_info fixture - subtree query with tree type
    let subtree_query = PathQuery {
        path: vec![b"root".to_vec(), b"key".to_vec()],
        query: SizedQuery::new(Query::new(), None, None),
    };
    let parent_tree_info_proof = db
        .prove_query(&subtree_query, None, &grove_version)
        .value
        .expect("prove subtree query");
    write_file(
        db_path,
        "grove_proof_subtree_parent_tree_info.bin",
        &parent_tree_info_proof,
    );

    write_file(db_path, "grove_root_hash.bin", &root_hash);
}
