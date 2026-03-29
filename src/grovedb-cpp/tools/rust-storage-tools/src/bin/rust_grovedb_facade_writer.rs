use std::env;
use std::path::Path;
use std::path::PathBuf;

use grovedb::query_result_type::QueryResultType::QueryKeyElementPairResultType;
use grovedb::reference_path::ReferencePathType;
use grovedb::{
    batch::{BatchApplyOptions, QualifiedGroveDbOp},
    Element, GroveDb,
};
use grovedb::{PathQuery, SizedQuery};
use grovedb_merk::proofs::Query;
use grovedb_merk::tree_type::TreeType;
use grovedb_version::version::GroveVersion;

fn write_simple(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert k1");
}

fn write_facade_insert_helpers(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert k1");
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree");
    db.insert(
        &[b"root".as_slice(), b"child".as_slice()],
        b"nk",
        Element::new_item(b"nv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nested key");
    db.insert(
        &[b"root".as_slice()],
        b"big",
        Element::new_big_sum_tree(None),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert big sum tree");
    db.insert(
        &[b"root".as_slice()],
        b"count",
        Element::new_count_tree(None),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert count tree");
    db.insert(
        &[b"root".as_slice()],
        b"provct",
        Element::new_provable_count_tree(None),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert provable count tree");
}

fn write_facade_insert_if_not_exists(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    let first = db
        .insert_if_not_exists(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v1".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert_if_not_exists first result");
    if !first {
        panic!("first insert_if_not_exists should insert");
    }
    let second = db
        .insert_if_not_exists(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v2".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert_if_not_exists second result");
    if second {
        panic!("second insert_if_not_exists should report existing key");
    }
    let third = db
        .insert_if_not_exists(
            &[b"root".as_slice()],
            b"k2",
            Element::new_item(b"v2".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert_if_not_exists third result");
    if !third {
        panic!("third insert_if_not_exists should insert");
    }
}

fn write_facade_insert_if_changed_value(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let first = db
        .insert_if_changed_value(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v1".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert_if_changed_value first result");
    if first.0 != true || first.1.is_some() {
        panic!("first insert_if_changed_value should be (true, None)");
    }

    let second = db
        .insert_if_changed_value(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v1".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert_if_changed_value second result");
    if second.0 != false || second.1.is_some() {
        panic!("second insert_if_changed_value should be (false, None)");
    }

    let third = db
        .insert_if_changed_value(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v2".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert_if_changed_value third result");
    if third.0 != true {
        panic!("third insert_if_changed_value should report changed=true");
    }
    match third.1 {
        Some(Element::Item(v, _)) if v.as_slice() == b"v1" => {}
        _ => panic!("third insert_if_changed_value should return previous item v1"),
    }
}

fn write_facade_insert_if_not_exists_return_existing(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let first = db
        .insert_if_not_exists_return_existing_element(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v1".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("first insert_if_not_exists_return_existing_element");
    if first.is_some() {
        panic!("first insert_if_not_exists_return_existing_element should return None");
    }

    let second = db
        .insert_if_not_exists_return_existing_element(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v2".to_vec()),
            None,
            grove_version,
        )
        .unwrap()
        .expect("second insert_if_not_exists_return_existing_element");
    match second {
        Some(Element::Item(v, _)) if v.as_slice() == b"v1" => {}
        _ => {
            panic!("second insert_if_not_exists_return_existing_element should return existing v1")
        }
    }
}

fn write_facade_insert_if_not_exists_return_existing_tx(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base k1");

    let tx = db.start_transaction();

    let existing = db
        .insert_if_not_exists_return_existing_element(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v2".to_vec()),
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("insert_if_not_exists_return_existing_element existing in tx");
    match existing {
        Some(Element::Item(v, _)) if v.as_slice() == b"v1" => {}
        _ => panic!("existing k1 in tx should return previous v1"),
    }

    let inserted = db
        .insert_if_not_exists_return_existing_element(
            &[b"root".as_slice()],
            b"txk",
            Element::new_item(b"tv".to_vec()),
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("insert_if_not_exists_return_existing_element new key in tx");
    if inserted.is_some() {
        panic!("new key in tx should return None");
    }

    let in_tx = db
        .get(&[b"root".as_slice()], b"txk", Some(&tx), grove_version)
        .unwrap()
        .expect("get txk in tx");
    match in_tx {
        Element::Item(v, _) if v.as_slice() == b"tv" => {}
        _ => panic!("tx should see txk=tv before commit"),
    }
    let outside_before_commit = db
        .get(&[b"root".as_slice()], b"txk", None, grove_version)
        .unwrap();
    if outside_before_commit.is_ok() {
        panic!("outside tx should not see txk before commit");
    }

    db.commit_transaction(tx)
        .unwrap()
        .expect("commit tx for insert_if_not_exists_return_existing tx mode");

    let outside_after_commit = db
        .get(&[b"root".as_slice()], b"txk", None, grove_version)
        .unwrap()
        .expect("get txk outside tx after commit");
    match outside_after_commit {
        Element::Item(v, _) if v.as_slice() == b"tv" => {}
        _ => panic!("outside tx should see txk=tv after commit"),
    }
}

fn write_facade_flush(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.flush().expect("facade flush should succeed");
}

fn write_facade_root_key(db: &GroveDb, grove_version: &GroveVersion) {
    let initial_root_key = db
        .root_key(None, grove_version)
        .unwrap()
        .expect("root_key on empty db");
    if initial_root_key.is_some() {
        panic!("root_key should be None for empty db");
    }

    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let after_insert_root_key = db
        .root_key(None, grove_version)
        .unwrap()
        .expect("root_key after insert");
    if after_insert_root_key.is_none() {
        panic!("root_key should be Some after inserting tree");
    }
}

fn write_facade_delete_if_empty_tree(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    db.insert(
        &[b"root".as_slice()],
        b"deletable",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert deletable empty tree");

    db.insert(
        &[b"root".as_slice()],
        b"nonempty",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nonempty tree");

    db.insert(
        &[b"root".as_slice(), b"nonempty".as_slice()],
        b"k1",
        Element::new_item(b"v1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item in nonempty tree");

    let deleted_nonempty = db
        .delete_if_empty_tree(&[b"root".as_slice()], b"nonempty", None, grove_version)
        .unwrap()
        .expect("delete_if_empty_tree(nonempty) should succeed");
    if deleted_nonempty {
        panic!("delete_if_empty_tree(nonempty) should return false");
    }

    let deleted_empty = db
        .delete_if_empty_tree(&[b"root".as_slice()], b"deletable", None, grove_version)
        .unwrap()
        .expect("delete_if_empty_tree(empty) should succeed");
    if !deleted_empty {
        panic!("delete_if_empty_tree(empty) should return true");
    }
}

fn write_facade_clear_subtree(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    db.insert(
        &[b"root".as_slice()],
        b"clr",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert clear subtree root");

    db.insert(
        &[b"root".as_slice(), b"clr".as_slice()],
        b"k1",
        Element::new_item(b"v1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert clear subtree key k1");

    db.insert(
        &[b"root".as_slice(), b"clr".as_slice()],
        b"k2",
        Element::new_item(b"v2".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert clear subtree key k2");

    let cleared = db
        .clear_subtree(
            &[b"root".as_slice(), b"clr".as_slice()],
            None,
            None,
            grove_version,
        )
        .expect("clear_subtree should succeed");
    if !cleared {
        panic!("clear_subtree should return true with default options");
    }
}

fn write_facade_clear_subtree_tx(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    db.insert(
        &[b"root".as_slice()],
        b"clr",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert clear subtree root");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice(), b"clr".as_slice()],
        b"k1",
        Element::new_item(b"v1".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert clear subtree key k1 in tx");
    db.insert(
        &[b"root".as_slice(), b"clr".as_slice()],
        b"k2",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert clear subtree key k2 in tx");

    let cleared = db
        .clear_subtree(
            &[b"root".as_slice(), b"clr".as_slice()],
            None,
            Some(&tx),
            grove_version,
        )
        .expect("clear_subtree in tx should succeed");
    if !cleared {
        panic!("clear_subtree in tx should return true with default options");
    }

    db.commit_transaction(tx)
        .unwrap()
        .expect("commit clear_subtree tx");
}

fn write_facade_follow_reference(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"ref2",
        Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
            b"root".to_vec(),
            b"target".to_vec(),
        ])),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref2");
    db.insert(
        &[b"root".as_slice()],
        b"ref1",
        Element::new_reference(ReferencePathType::SiblingReference(b"ref2".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref1");
}

/// Test FollowReference transaction overload with lifecycle and visibility assertions
fn write_facade_follow_reference_tx(db: &GroveDb, grove_version: &GroveVersion) {
    // Create root tree
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    // Insert target item at root/target
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");

    // Insert reference at root/ref (sibling reference to target)
    db.insert(
        &[b"root".as_slice()],
        b"ref",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref");

    // Start transaction and insert tx-local reference
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"tx_ref",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert tx_ref in tx");

    // Follow reference in tx - should resolve tx_ref
    let followed_in_tx = db
        .follow_reference(
            (&[b"root".as_slice(), b"tx_ref".as_slice()]).into(),
            true,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("follow_reference in tx should resolve");
    match followed_in_tx {
        Element::Item(v, _) if v.as_slice() == b"tv" => {}
        _ => panic!("follow_reference in tx should resolve to target item tv"),
    }

    // Follow reference outside tx before commit - should NOT find tx_ref
    let followed_outside_before = db
        .follow_reference(
            (&[b"root".as_slice(), b"tx_ref".as_slice()]).into(),
            true,
            None,
            grove_version,
        )
        .unwrap();
    if followed_outside_before.is_ok() {
        panic!("follow_reference outside tx should not resolve tx_ref before commit");
    }

    // Commit transaction
    db.commit_transaction(tx).unwrap().expect("commit tx");

    // Follow reference after commit - should resolve tx_ref
    let followed_after_commit = db
        .follow_reference(
            (&[b"root".as_slice(), b"tx_ref".as_slice()]).into(),
            true,
            None,
            grove_version,
        )
        .unwrap()
        .expect("follow_reference after commit should resolve");
    match followed_after_commit {
        Element::Item(v, _) if v.as_slice() == b"tv" => {}
        _ => panic!("follow_reference after commit should resolve to target item tv"),
    }
}

/// Test subtree discovery with nested tree structure
fn write_facade_find_subtrees(db: &GroveDb, grove_version: &GroveVersion) {
    // Create root tree
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    // Create first-level child trees
    db.insert(
        &[b"root".as_slice()],
        b"child1",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child1 tree");
    db.insert(
        &[b"root".as_slice()],
        b"child2",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child2 tree");

    // Create second-level nested trees under child1
    db.insert(
        &[b"root".as_slice(), b"child1".as_slice()],
        b"grandchild1",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert grandchild1 tree");
    db.insert(
        &[b"root".as_slice(), b"child1".as_slice()],
        b"grandchild2",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert grandchild2 tree");

    // Add some items to leaf trees
    db.insert(
        &[
            b"root".as_slice(),
            b"child1".as_slice(),
            b"grandchild1".as_slice(),
        ],
        b"k1",
        Element::new_item(b"v1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert k1 in grandchild1");
    db.insert(
        &[b"root".as_slice(), b"child2".as_slice()],
        b"k2",
        Element::new_item(b"v2".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert k2 in child2");
}

/// Test CheckSubtreeExistsInvalidPath tx overload: validates tx-local subtree visibility
fn write_facade_check_subtree_exists_invalid_path_tx(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    // Create root tree
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    // Insert base item at root
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"base_v".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    // Create child subtree under root (committed)
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree");
}

/// Test multi-hop reference resolution with mixed path types:
/// ref_c (UpstreamRootHeight) -> ref_b (Sibling) -> ref_a (Absolute) -> target
fn write_facade_follow_reference_mixed_path_chain(db: &GroveDb, grove_version: &GroveVersion) {
    // Create nested structure: root/inner
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"inner",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert inner tree");

    // Insert target item at root/target
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"mixed_target_value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");

    // ref_a: Absolute path reference to target
    // Hop 1: ref_a (at root) -> root/target
    db.insert(
        &[b"root".as_slice()],
        b"ref_a",
        Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
            b"root".to_vec(),
            b"target".to_vec(),
        ])),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref_a absolute");

    // ref_b: Sibling reference to ref_a (both at root level)
    // Hop 2: ref_b (at root) -> ref_a (at root)
    db.insert(
        &[b"root".as_slice()],
        b"ref_b",
        Element::new_reference(ReferencePathType::SiblingReference(b"ref_a".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref_b sibling");

    // ref_c: UpstreamRootHeightReference (go up 1 level from root/inner to root, then to ref_b)
    // Starting from root/inner/ref_c, go up 1 to root, then to ref_b
    // UpstreamRootHeightReference(1, [ref_b]) from path [root, inner]:
    //   - len = 2, keep = 1, so n_to_remove = 1
    //   - After removing 1: [root]
    //   - Append [ref_b]: [root, ref_b]
    // Hop 3: ref_c (at root/inner) -> ref_b (at root)
    db.insert(
        &[b"root".as_slice(), b"inner".as_slice()],
        b"ref_c",
        Element::new_reference(ReferencePathType::UpstreamRootHeightReference(
            1,
            vec![b"ref_b".to_vec()],
        )),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref_c upstream");
}

/// Test UpstreamRootHeightWithParentPathAddition reference resolution:
/// root/branch/target/ref_parent_add -> root/alias/target
fn write_facade_follow_reference_parent_path_addition(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"alias",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert alias tree");
    db.insert(
        &[b"root".as_slice(), b"alias".as_slice()],
        b"target",
        Element::new_item(b"parent_add_target_value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"branch",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert branch tree");
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice()],
        b"target",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nested target tree");
    db.insert(
        &[
            b"root".as_slice(),
            b"branch".as_slice(),
            b"target".as_slice(),
        ],
        b"ref_parent_add",
        Element::new_reference(
            ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
                1,
                vec![b"alias".to_vec()],
            ),
        ),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert parent-path-addition reference");
}

/// Test CousinReference resolution:
/// Structure: root/branch/deep and root/branch/cousin/ref
/// Reference at root/branch/deep/ref uses CousinReference(b"cousin")
/// This swaps the parent key 'deep' with 'cousin', keeping the original key 'ref'
/// Result: resolves to root/branch/cousin/ref
fn write_facade_follow_reference_cousin(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"branch",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert branch tree");
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice()],
        b"deep",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert deep tree");
    // Insert cousin subtree, then target at root/branch/cousin/ref (the resolved location)
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice()],
        b"cousin",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert cousin subtree");
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice(), b"cousin".as_slice()],
        b"ref",
        Element::new_item(b"cousin_target_value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert cousin target item");
    // Insert reference at root/branch/deep/ref that points to root/branch/cousin/ref
    // CousinReference swaps 'deep' with 'cousin', keeping key 'ref'
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice(), b"deep".as_slice()],
        b"ref",
        Element::new_reference(ReferencePathType::CousinReference(b"cousin".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert cousin reference");
}

/// Test RemovedCousinReference resolution:
/// Structure: root/branch/deep/target and root/branch/cousin/nested/target
/// Reference at root/branch/deep/ref uses RemovedCousinReference(vec![b"cousin", b"nested"])
/// This swaps the parent key 'deep' with the path [cousin, nested], keeping the original key 'ref'
/// Result: resolves to root/branch/cousin/nested/ref
fn write_facade_follow_reference_removed_cousin(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"branch",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert branch tree");
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice()],
        b"deep",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert deep tree");
    // Insert nested tree structure for the resolved location
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice()],
        b"cousin",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert cousin tree");
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice(), b"cousin".as_slice()],
        b"nested",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nested tree");
    // Insert target at root/branch/cousin/nested (the resolved location)
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice(), b"cousin".as_slice(), b"nested".as_slice()],
        b"ref",
        Element::new_item(b"removed_cousin_target_value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert removed cousin target item");
    // Insert reference at root/branch/deep/ref that points to root/branch/cousin/nested/ref
    // RemovedCousinReference swaps 'deep' with [cousin, nested], keeping key 'ref'
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice(), b"deep".as_slice()],
        b"ref",
        Element::new_reference(ReferencePathType::RemovedCousinReference(vec![
            b"cousin".to_vec(),
            b"nested".to_vec(),
        ])),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert removed cousin reference");
}

/// Test UpstreamFromElementHeight reference path type
/// Structure: root/branch/deep/target
/// Reference at root/branch/deep/ref uses UpstreamFromElementHeight(1, ['alias'])
/// This discards 1 segment from current path and appends ['alias']
/// Current qualified path: [root, branch, deep, ref]
/// Parent path: [root, branch, deep], discard 1 -> [root, branch]
/// Append ['alias'] -> [root, branch, alias]
fn write_facade_follow_reference_upstream_element_height(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"branch",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert branch tree");
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice()],
        b"deep",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert deep tree");
    // Insert target at root/branch/alias (the resolved location)
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice()],
        b"alias",
        Element::new_item(b"upstream_elem_value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert upstream element target item");
    // Insert reference at root/branch/deep/ref that points to root/branch/alias
    // UpstreamFromElementHeight(1, ['alias']) discards 1 segment from parent path and appends ['alias']
    db.insert(
        &[b"root".as_slice(), b"branch".as_slice(), b"deep".as_slice()],
        b"ref",
        Element::new_reference(ReferencePathType::UpstreamFromElementHeightReference(
            1,
            vec![b"alias".to_vec()],
        )),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert upstream element height reference");
}


fn write_facade_get_raw(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"ref1",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref1");
}

fn write_facade_get_raw_optional(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"ref1",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref1");

    let existing = db
        .get_raw_optional((&[b"root".as_slice()]).into(), b"ref1", None, grove_version)
        .unwrap()
        .expect("get_raw_optional existing path should succeed");
    match existing {
        Some(Element::Reference(..)) => {}
        _ => panic!("get_raw_optional should return unresolved reference element"),
    }
    let missing = db
        .get_raw_optional(
            (&[b"root".as_slice(), b"miss".as_slice()]).into(),
            b"k",
            None,
            grove_version,
        )
        .unwrap()
        .expect("get_raw_optional missing path should succeed");
    if missing.is_some() {
        panic!("get_raw_optional should return None for missing path");
    }
}

fn write_facade_get_raw_caching_optional(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"ref",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref");
}

/// Test GetCachingOptional transaction overload with lifecycle and visibility assertions
fn write_facade_get_caching_optional_tx(db: &GroveDb, grove_version: &GroveVersion) {
    // Create root tree
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    // Insert base item at root/base
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"base_v".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    // Start transaction and insert tx-local item
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tx_v".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    // GetCachingOptional in tx - should see txk
    let element_in_tx = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"txk",
            true,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("get_caching_optional in tx should succeed");
    match element_in_tx {
        Element::Item(v, _) if v.as_slice() == b"tx_v" => {}
        _ => panic!("get_caching_optional in tx should return txk item value"),
    }

    // GetCachingOptional outside tx before commit - should NOT find txk
    let result_outside_before = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"txk",
            true,
            None,
            grove_version,
        );
    assert!(
        result_outside_before.value.is_err(),
        "get_caching_optional outside tx should return Err for tx-local key before commit"
    );

    // GetCachingOptional in tx for base key - should see base_v
    let base_in_tx = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"base",
            true,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("get_caching_optional in tx for base should succeed");
    match base_in_tx {
        Element::Item(v, _) if v.as_slice() == b"base_v" => {}
        _ => panic!("get_caching_optional in tx should return base item value"),
    }

    // Commit transaction
    db.commit_transaction(tx).unwrap().expect("commit tx");

    // GetCachingOptional after commit - should see txk
    let element_after = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"txk",
            true,
            None,
            grove_version,
        )
        .unwrap()
        .expect("get_caching_optional after commit should succeed");
    match element_after {
        Element::Item(v, _) if v.as_slice() == b"tx_v" => {}
        _ => panic!("get_caching_optional after commit should return txk item value"),
    }
}

fn write_facade_get_subtree_root_tx(db: &GroveDb, grove_version: &GroveVersion) {
    // Create root tree
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    // Insert base item at root
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"base_v".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    // Create child subtree under root
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree");

    // Insert item inside child subtree
    db.insert(
        &[b"root".as_slice(), b"child".as_slice()],
        b"child_key",
        Element::new_item(b"child_v".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child_key in child");

    // Start transaction and create tx-local subtree
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"tx_child",
        Element::empty_tree(),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert tx_child tree in tx");

    // Insert item inside tx-local subtree
    db.insert(
        &[b"root".as_slice(), b"tx_child".as_slice()],
        b"tx_child_key",
        Element::new_item(b"tx_child_v".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert tx_child_key in tx_child");

    // GetSubtreeRoot in tx for tx_child - get the tree element from parent (root)
    // This simulates what GetSubtreeRoot does: load parent and find subtree root element
    let element_in_tx = db
        .get(&[b"root".as_slice()], b"tx_child", Some(&tx), grove_version)
        .unwrap()
        .expect("get in tx should succeed for tx-local subtree element");
    // The element should be a Tree variant
    match element_in_tx {
        Element::Tree(_, _) => {}
        _ => panic!("get in tx should return Tree element for tx_child"),
    }

    // Get outside tx before commit - should NOT find tx_child
    let result_outside_before = db.get(&[b"root".as_slice()], b"tx_child", None, grove_version);
    assert!(result_outside_before.value.is_err(), "get outside tx should fail for tx-local subtree before commit");

    // Get in tx for child (committed subtree) - should see child tree element
    let element_child = db
        .get(&[b"root".as_slice()], b"child", Some(&tx), grove_version)
        .unwrap()
        .expect("get in tx for child should succeed");
    match element_child {
        Element::Tree(_, _) => {}
        _ => panic!("get in tx should return Tree element for child"),
    }

    // Commit transaction
    db.commit_transaction(tx).unwrap().expect("commit tx");

    // Get after commit - should see tx_child tree element
    let element_after = db
        .get(&[b"root".as_slice()], b"tx_child", None, grove_version)
        .unwrap()
        .expect("get after commit should succeed");
    match element_after {
        Element::Tree(_, _) => {}
        _ => panic!("get after commit should return Tree element for tx_child"),
    }
}

/// Test GetCachingOptional transaction overload with lifecycle and visibility assertions
/// (Note: Rust GroveDB doesn't have has_caching_optional, so we use get_caching_optional
/// and check for Option result instead)
fn write_facade_has_caching_optional_tx(db: &GroveDb, grove_version: &GroveVersion) {
    // Create root tree
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    // Insert base item at root/base
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"base_v".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    // Start transaction and insert tx-local item
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tx_v".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    // GetCachingOptional in tx - should find txk
    let elem_in_tx = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"txk",
            true,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("get_caching_optional in tx should succeed");
    match &elem_in_tx {
        Element::Item(v, _) if v.as_slice() == b"tx_v" => {}
        _ => panic!("get_caching_optional should return txk item value"),
    }

    // GetCachingOptional outside tx before commit - should NOT find txk
    let result_outside_before = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"txk",
            true,
            None,
            grove_version,
        );
    // In Rust, get_caching_optional returns Err when key not found
    assert!(result_outside_before.value.is_err(), "get_caching_optional outside tx should return Err for tx-local key before commit");

    // GetCachingOptional in tx for base key - should find base_v
    let elem_base_in_tx = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"base",
            true,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("get_caching_optional in tx for base should succeed");
    match &elem_base_in_tx {
        Element::Item(v, _) if v.as_slice() == b"base_v" => {}
        _ => panic!("get_caching_optional should return base item value"),
    }

    // Commit transaction
    db.commit_transaction(tx).unwrap().expect("commit tx");

    // GetCachingOptional after commit - should find txk
    let elem_after = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"txk",
            true,
            None,
            grove_version,
        )
        .unwrap()
        .expect("get_caching_optional after commit should succeed");
    match &elem_after {
        Element::Item(v, _) if v.as_slice() == b"tx_v" => {}
        _ => panic!("get_caching_optional should return txk item value"),
    }

    // GetCachingOptional for base after commit - should still find base
    let elem_base_after = db
        .get_caching_optional(
            (&[b"root".as_slice()]).into(),
            b"base",
            true,
            None,
            grove_version,
        )
        .unwrap()
        .expect("get_caching_optional for base should succeed");
    match &elem_base_after {
        Element::Item(v, _) if v.as_slice() == b"base_v" => {}
        _ => panic!("get_caching_optional should return base item value"),
    }
}

fn write_facade_query_raw(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"ref1",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref1");

    let mut query = Query::new();
    query.insert_key(b"ref1".to_vec());
    let path_query = PathQuery::new(vec![b"root".to_vec()], SizedQuery::new(query, None, None));
    let (results, _) = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("query_raw should succeed");
    let pairs = results.to_key_elements();
    if pairs.len() != 1 {
        panic!("query_raw should return one key");
    }
    match &pairs[0].1 {
        Element::Reference(..) => {}
        _ => panic!("query_raw should return unresolved reference element"),
    }
}

fn write_facade_query_item_value(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"item2",
        Element::new_item(b"iw".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item2");
    db.insert(
        &[b"root".as_slice()],
        b"ref",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref");

    let mut query = Query::new();
    query.insert_key(b"item2".to_vec());
    query.insert_key(b"ref".to_vec());
    query.insert_key(b"target".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(3), None),
    );
    let (values, _) = db
        .query_item_value(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_item_value should succeed");
    if values.len() != 3 {
        panic!("query_item_value should return 3 values");
    }
    if !values.iter().any(|v| v.as_slice() == b"iw") {
        panic!("query_item_value should include item value");
    }
    if !values.iter().any(|v| v.as_slice() == b"tv") {
        panic!("query_item_value should include resolved reference item value");
    }
    if values.iter().filter(|v| v.as_slice() == b"tv").count() != 2 {
        panic!("query_item_value should include two tv values (target + resolved ref)");
    }
}

fn write_facade_query_item_value_tx(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"bv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    let mut query = Query::new();
    query.insert_key(b"base".to_vec());
    query.insert_key(b"txk".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );

    let (values_in_tx, _) = db
        .query_item_value(&path_query, true, true, true, Some(&tx), grove_version)
        .unwrap()
        .expect("query_item_value in tx should succeed");
    if !values_in_tx.iter().any(|v| v.as_slice() == b"tv") {
        panic!("query_item_value in tx should include txk value");
    }

    let (values_outside_before_commit, _) = db
        .query_item_value(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_item_value outside tx before commit should succeed");
    if values_outside_before_commit
        .iter()
        .any(|v| v.as_slice() == b"tv")
    {
        panic!("query_item_value outside tx should not include txk value before commit");
    }

    db.commit_transaction(tx).unwrap().expect("commit tx");

    let (values_outside_after_commit, _) = db
        .query_item_value(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_item_value outside tx after commit should succeed");
    if !values_outside_after_commit
        .iter()
        .any(|v| v.as_slice() == b"tv")
    {
        panic!("query_item_value outside tx should include txk value after commit");
    }
}

fn write_facade_query_item_value_or_sum_tx(db: &GroveDb, grove_version: &GroveVersion) {
    // Simplified test: verify tx-local visibility for query operations
    // The QueryItemValueOrSum API uses internal types not exposed publicly
    // This test validates the tx visibility pattern using query_item_value
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"bv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    let mut query = Query::new();
    query.insert_key(b"base".to_vec());
    query.insert_key(b"txk".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );

    // In-tx query should see tx-local writes
    let (values_in_tx, _) = db
        .query_item_value(&path_query, true, true, true, Some(&tx), grove_version)
        .unwrap()
        .expect("query_item_value in tx should succeed");
    if !values_in_tx.iter().any(|v| v.as_slice() == b"tv") {
        panic!("query_item_value in tx should include txk value");
    }

    // Outside-tx query before commit should NOT see tx-local writes
    let (values_outside_before_commit, _) = db
        .query_item_value(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_item_value outside tx before commit should succeed");
    if values_outside_before_commit
        .iter()
        .any(|v| v.as_slice() == b"tv")
    {
        panic!("query_item_value outside tx should not include txk value before commit");
    }

    db.commit_transaction(tx).unwrap().expect("commit tx");

    // Outside-tx query after commit should see persisted writes
    let (values_outside_after_commit, _) = db
        .query_item_value(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_item_value outside tx after commit should succeed");
    if !values_outside_after_commit
        .iter()
        .any(|v| v.as_slice() == b"tv")
    {
        panic!("query_item_value outside tx should include txk value after commit");
    }
}

fn write_facade_query_raw_tx(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"bv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    let mut query = Query::new();
    query.insert_key(b"miss".to_vec());
    query.insert_key(b"txk".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );

    let rows_in_tx = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("query_raw in tx should succeed");
    let pairs_in_tx = rows_in_tx.0.to_key_elements();
    let mut in_tx_saw_txk = false;
    for (key, _elem) in &pairs_in_tx {
        if key.as_slice() == b"txk" {
            in_tx_saw_txk = true;
        }
    }
    if !in_tx_saw_txk {
        panic!("query_raw in tx should see txk");
    }

    let rows_outside_before_commit = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("query_raw outside tx before commit should succeed");
    let pairs_outside_before = rows_outside_before_commit.0.to_key_elements();
    let mut outside_before_commit_saw_txk = false;
    for (key, _elem) in &pairs_outside_before {
        if key.as_slice() == b"txk" {
            outside_before_commit_saw_txk = true;
        }
    }
    if outside_before_commit_saw_txk {
        panic!("query_raw outside tx should not see txk before commit");
    }

    db.commit_transaction(tx).unwrap().expect("commit tx");

    let rows_outside_after_commit = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("query_raw outside tx after commit should succeed");
    let pairs_outside_after = rows_outside_after_commit.0.to_key_elements();
    let mut outside_after_commit_saw_txk = false;
    for (key, _elem) in &pairs_outside_after {
        if key.as_slice() == b"txk" {
            outside_after_commit_saw_txk = true;
        }
    }
    if !outside_after_commit_saw_txk {
        panic!("query_raw outside tx should see txk after commit");
    }
}

fn write_facade_query_key_element_pairs_tx(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"bv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    let mut query = Query::new();
    query.insert_key(b"base".to_vec());
    query.insert_key(b"txk".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );

    // Query in tx - should see tx-local changes
    let rows_in_tx = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("query in tx should succeed");
    let pairs_in_tx = rows_in_tx.0.to_key_elements();
    let mut in_tx_saw_txk = false;
    for (key, _elem) in &pairs_in_tx {
        if key.as_slice() == b"txk" {
            in_tx_saw_txk = true;
        }
    }
    if !in_tx_saw_txk {
        panic!("query in tx should see txk");
    }
    if pairs_in_tx.len() != 2 {
        panic!("query in tx should return 2 pairs");
    }

    // Query outside tx before commit - should not see tx-local changes
    let rows_outside_before_commit = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("query outside tx before commit should succeed");
    let pairs_outside_before = rows_outside_before_commit.0.to_key_elements();
    let mut outside_before_commit_saw_txk = false;
    for (key, _elem) in &pairs_outside_before {
        if key.as_slice() == b"txk" {
            outside_before_commit_saw_txk = true;
        }
    }
    if outside_before_commit_saw_txk {
        panic!("query outside tx should not see txk before commit");
    }
    if pairs_outside_before.len() != 1 {
        panic!("query outside tx before commit should return 1 pair (base only)");
    }

    db.commit_transaction(tx).unwrap().expect("commit tx");

    // Query outside tx after commit - should see committed changes
    let rows_outside_after_commit = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("query outside tx after commit should succeed");
    let pairs_outside_after = rows_outside_after_commit.0.to_key_elements();
    let mut outside_after_commit_saw_txk = false;
    for (key, _elem) in &pairs_outside_after {
        if key.as_slice() == b"txk" {
            outside_after_commit_saw_txk = true;
        }
    }
    if !outside_after_commit_saw_txk {
        panic!("query outside tx should see txk after commit");
    }
    if pairs_outside_after.len() != 2 {
        panic!("query outside tx after commit should return 2 pairs");
    }
}

fn write_facade_query_raw_keys_optional(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"ref1",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref1");

    let mut query = Query::new();
    query.insert_key(b"ref1".to_vec());
    query.insert_key(b"miss".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );
    let rows = db
        .query_raw_keys_optional(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_raw_keys_optional should succeed");
    if rows.len() != 2 {
        panic!("query_raw_keys_optional should return 2 rows");
    }
    let mut saw_ref = false;
    let mut saw_missing = false;
    for row in rows {
        if row.0 != vec![b"root".to_vec()] {
            panic!("query_raw_keys_optional row path mismatch");
        }
        if row.1.as_slice() == b"ref1" {
            match row.2.as_ref().expect("ref1 row should have value") {
                Element::Reference(..) => {}
                _ => panic!("query_raw_keys_optional should return unresolved reference element"),
            }
            saw_ref = true;
        } else if row.1.as_slice() == b"miss" {
            if row.2.is_some() {
                panic!("query_raw_keys_optional miss row should be None");
            }
            saw_missing = true;
        } else {
            panic!("query_raw_keys_optional returned unexpected key");
        }
    }
    if !saw_ref || !saw_missing {
        panic!("query_raw_keys_optional expected rows for ref1 and miss");
    }
}

fn write_facade_query_keys_optional(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"target",
        Element::new_item(b"tv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");
    db.insert(
        &[b"root".as_slice()],
        b"ref1",
        Element::new_reference(ReferencePathType::SiblingReference(b"target".to_vec())),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref1");

    let mut query = Query::new();
    query.insert_key(b"ref1".to_vec());
    query.insert_key(b"miss".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );
    let rows = db
        .query_keys_optional(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_keys_optional should succeed");
    if rows.len() != 2 {
        panic!("query_keys_optional should return 2 rows");
    }
    let mut saw_ref = false;
    let mut saw_missing = false;
    for row in rows {
        if row.0 != vec![b"root".to_vec()] {
            panic!("query_keys_optional row path mismatch");
        }
        if row.1.as_slice() == b"ref1" {
            match row.2.as_ref().expect("ref1 row should have value") {
                Element::Item(value, ..) => {
                    if value.as_slice() != b"tv" {
                        panic!("query_keys_optional ref1 row value mismatch");
                    }
                }
                _ => panic!("query_keys_optional should return resolved item"),
            }
            saw_ref = true;
        } else if row.1.as_slice() == b"miss" {
            if row.2.is_some() {
                panic!("query_keys_optional miss row should be None");
            }
            saw_missing = true;
        } else {
            panic!("query_keys_optional returned unexpected key");
        }
    }
    if !saw_ref || !saw_missing {
        panic!("query_keys_optional expected rows for ref1 and miss");
    }
}

fn write_facade_query_raw_keys_optional_tx(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"bv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    let mut query = Query::new();
    query.insert_key(b"miss".to_vec());
    query.insert_key(b"txk".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );

    let rows_in_tx = db
        .query_raw_keys_optional(&path_query, true, true, true, Some(&tx), grove_version)
        .unwrap()
        .expect("query_raw_keys_optional in tx should succeed");
    let mut in_tx_saw_txk = false;
    for row in rows_in_tx {
        if row.1.as_slice() == b"txk" {
            in_tx_saw_txk = row.2.is_some();
        }
    }
    if !in_tx_saw_txk {
        panic!("query_raw_keys_optional in tx should see txk");
    }

    let rows_outside_before_commit = db
        .query_raw_keys_optional(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_raw_keys_optional outside tx before commit should succeed");
    let mut outside_before_commit_saw_txk = false;
    for row in rows_outside_before_commit {
        if row.1.as_slice() == b"txk" {
            outside_before_commit_saw_txk = row.2.is_some();
        }
    }
    if outside_before_commit_saw_txk {
        panic!("query_raw_keys_optional outside tx should not see txk before commit");
    }

    db.commit_transaction(tx).unwrap().expect("commit tx");

    let rows_outside_after_commit = db
        .query_raw_keys_optional(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_raw_keys_optional outside tx after commit should succeed");
    let mut outside_after_commit_saw_txk = false;
    for row in rows_outside_after_commit {
        if row.1.as_slice() == b"txk" {
            outside_after_commit_saw_txk = row.2.is_some();
        }
    }
    if !outside_after_commit_saw_txk {
        panic!("query_raw_keys_optional outside tx should see txk after commit");
    }
}

fn write_facade_query_keys_optional_tx(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"base",
        Element::new_item(b"bv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert base item");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txk",
        Element::new_item(b"tv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txk in tx");

    let mut query = Query::new();
    query.insert_key(b"miss".to_vec());
    query.insert_key(b"txk".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );

    let rows_in_tx = db
        .query_keys_optional(&path_query, true, true, true, Some(&tx), grove_version)
        .unwrap()
        .expect("query_keys_optional in tx should succeed");
    let mut in_tx_saw_txk = false;
    for row in rows_in_tx {
        if row.1.as_slice() == b"txk" {
            in_tx_saw_txk = row.2.is_some();
        }
    }
    if !in_tx_saw_txk {
        panic!("query_keys_optional in tx should see txk");
    }

    let rows_outside_before_commit = db
        .query_keys_optional(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_keys_optional outside tx before commit should succeed");
    let mut outside_before_commit_saw_txk = false;
    for row in rows_outside_before_commit {
        if row.1.as_slice() == b"txk" {
            outside_before_commit_saw_txk = row.2.is_some();
        }
    }
    if outside_before_commit_saw_txk {
        panic!("query_keys_optional outside tx should not see txk before commit");
    }

    db.commit_transaction(tx).unwrap().expect("commit tx");

    let rows_outside_after_commit = db
        .query_keys_optional(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_keys_optional outside tx after commit should succeed");
    let mut outside_after_commit_saw_txk = false;
    for row in rows_outside_after_commit {
        if row.1.as_slice() == b"txk" {
            outside_after_commit_saw_txk = row.2.is_some();
        }
    }
    if !outside_after_commit_saw_txk {
        panic!("query_keys_optional outside tx should see txk after commit");
    }
}

fn write_facade_query_sums_tx(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::new_sum_tree(None),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"s1",
        Element::new_sum_item(10),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert s1 sum");
    db.insert(
        &[b"root".as_slice()],
        b"s2",
        Element::new_sum_item(20),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert s2 sum");

    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"txs",
        Element::new_sum_item(30),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert txs sum in tx");

    let mut query = Query::new();
    query.insert_key(b"s1".to_vec());
    query.insert_key(b"txs".to_vec());
    let path_query = PathQuery::new(
        vec![b"root".to_vec()],
        SizedQuery::new(query, Some(2), None),
    );

    let sums_in_tx = db
        .query_sums(&path_query, true, true, true, Some(&tx), grove_version)
        .unwrap()
        .expect("query_sums in tx should succeed");
    if sums_in_tx.0.len() != 2 {
        panic!(
            "query_sums in tx should return 2 sums, got {}",
            sums_in_tx.0.len()
        );
    }
    if !sums_in_tx.0.contains(&30) {
        panic!("query_sums in tx should see txs sum (30)");
    }

    let sums_outside_before_commit = db
        .query_sums(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_sums outside tx before commit should succeed");
    if sums_outside_before_commit.0.len() != 1 {
        panic!(
            "query_sums outside tx before commit should return 1 sum, got {}",
            sums_outside_before_commit.0.len()
        );
    }
    if sums_outside_before_commit.0[0] != 10 {
        panic!("query_sums outside tx before commit should only see s1 (10)");
    }

    db.commit_transaction(tx).unwrap().expect("commit tx");

    let sums_outside_after_commit = db
        .query_sums(&path_query, true, true, true, None, grove_version)
        .unwrap()
        .expect("query_sums outside tx after commit should succeed");
    if sums_outside_after_commit.0.len() != 2 {
        panic!(
            "query_sums outside tx after commit should return 2 sums, got {}",
            sums_outside_after_commit.0.len()
        );
    }
    if !sums_outside_after_commit.0.contains(&30) {
        panic!("query_sums outside tx after commit should see txs sum (30)");
    }
}

fn write_nested(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree");
    db.insert(
        &[b"root".as_slice(), b"child".as_slice()],
        b"nk",
        Element::new_item(b"nv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nested key");
}

fn write_tx_commit(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"ktx",
        Element::new_item(b"tv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert ktx in tx");
    db.commit_transaction(tx).unwrap().expect("commit tx");
}

fn write_tx_rollback(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"kroll",
        Element::new_item(b"rv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert rollback key in tx");
    db.rollback_transaction(&tx).unwrap();
    let tx_read_old = db
        .get(&[b"root".as_slice()], b"k1", Some(&tx), grove_version)
        .unwrap();
    if tx_read_old.is_err() {
        panic!("expected rolled-back tx to read original key");
    }
    let tx_read_new = db
        .get(&[b"root".as_slice()], b"kroll", Some(&tx), grove_version)
        .unwrap();
    if tx_read_new.is_ok() {
        panic!("expected rolled-back tx to hide reverted key");
    }
}

fn write_tx_visibility_rollback(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"kvis",
        Element::new_item(b"vv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert visibility key in tx");

    let in_tx_new = db
        .get(&[b"root".as_slice()], b"kvis", Some(&tx), grove_version)
        .unwrap();
    if in_tx_new.is_err() {
        panic!("expected tx to read inserted key");
    }
    let in_tx_old = db
        .get(&[b"root".as_slice()], b"k1", Some(&tx), grove_version)
        .unwrap();
    if in_tx_old.is_err() {
        panic!("expected tx to keep existing key visible");
    }

    let outside_new = db
        .get(&[b"root".as_slice()], b"kvis", None, grove_version)
        .unwrap();
    if outside_new.is_ok() {
        panic!("expected outside read to hide uncommitted insert");
    }
    db.rollback_transaction(&tx).unwrap();

    let outside_old = db
        .get(&[b"root".as_slice()], b"k1", None, grove_version)
        .unwrap()
        .expect("outside read of existing key");
    match outside_old {
        Element::Item(v, _) => {
            if v.as_slice() != b"v1" {
                panic!("unexpected outside old value");
            }
        }
        _ => panic!("expected item for outside old key"),
    }
}

fn write_tx_mixed_commit(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"kmix",
        Element::new_item(b"mv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert kmix in tx");
    db.delete(&[b"root".as_slice()], b"k1", None, Some(&tx), grove_version)
        .unwrap()
        .expect("delete k1 in tx");
    db.commit_transaction(tx).unwrap().expect("commit mixed tx");
}

fn write_tx_rollback_range(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"kroll",
        Element::new_item(b"rv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert rollback-range key in tx");
    db.rollback_transaction(&tx).unwrap();

    let has_k1 = db
        .has_raw(&[b"root".as_slice()], b"k1", Some(&tx), grove_version)
        .unwrap()
        .expect("has_raw k1 result");
    let has_kroll = db
        .has_raw(&[b"root".as_slice()], b"kroll", Some(&tx), grove_version)
        .unwrap()
        .expect("has_raw kroll result");
    if !has_k1 || has_kroll {
        panic!("rolled-back tx has_raw view mismatch");
    }
}

fn write_tx_drop_abort(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    {
        let tx = db.start_transaction();
        db.insert(
            &[b"root".as_slice()],
            b"kdrop",
            Element::new_item(b"dv".to_vec()),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("insert drop key in tx");
        let in_tx = db
            .get(&[b"root".as_slice()], b"kdrop", Some(&tx), grove_version)
            .unwrap();
        if in_tx.is_err() {
            panic!("expected tx to read drop key before drop");
        }
    }
}

fn write_tx_commit_after_rollback_rejected(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"kcar",
        Element::new_item(b"cv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert key before rollback");
    db.rollback_transaction(&tx).unwrap();
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("expected commit after rollback to succeed");
    }
}

fn write_tx_write_after_rollback_rejected(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.rollback_transaction(&tx).unwrap();
    let insert_result = db
        .insert(
            &[b"root".as_slice()],
            b"kwar",
            Element::new_item(b"wv".to_vec()),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap();
    if insert_result.is_err() {
        panic!("expected write after rollback to succeed");
    }
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("expected commit after write-after-rollback to succeed");
    }
}

fn write_tx_multi_rollback_reuse(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.rollback_transaction(&tx).unwrap();

    db.insert(
        &[b"root".as_slice()],
        b"kmr1",
        Element::new_item(b"mv1".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert after rollback should succeed");
    db.rollback_transaction(&tx).unwrap();

    db.insert(
        &[b"root".as_slice()],
        b"kmr2",
        Element::new_item(b"mv2".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("second insert after rollback should succeed");

    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("expected commit after multi rollback reuse to succeed");
    }
}

fn write_tx_delete_after_rollback(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.rollback_transaction(&tx).unwrap();
    let delete_result = db
        .delete(&[b"root".as_slice()], b"k1", None, Some(&tx), grove_version)
        .unwrap();
    if delete_result.is_err() {
        panic!("expected delete after rollback to succeed");
    }
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("expected commit after delete-after-rollback to succeed");
    }
}

fn write_tx_reopen_visibility_after_rollback(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.rollback_transaction(&tx).unwrap();
    db.insert(
        &[b"root".as_slice()],
        b"krv",
        Element::new_item(b"rv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert after rollback should succeed");

    let in_tx = db
        .get(&[b"root".as_slice()], b"krv", Some(&tx), grove_version)
        .unwrap();
    if in_tx.is_err() {
        panic!("in-tx read should see reopened-write key");
    }

    let out_tx = db
        .get(&[b"root".as_slice()], b"krv", None, grove_version)
        .unwrap();
    if out_tx.is_ok() {
        panic!("out-of-tx read should not see reopened-write key before commit");
    }

    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("expected commit after reopen visibility check to succeed");
    }
}

fn write_tx_reopen_conflict_same_path(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    db.rollback_transaction(&tx1).unwrap();
    db.insert(
        &[b"root".as_slice()],
        b"krc",
        Element::new_item(b"a".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 insert after rollback should succeed");

    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"krc",
        Element::new_item(b"b".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 insert should succeed before commit");

    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 commit should fail due to conflict");
    }
}

fn write_tx_delete_visibility(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let del_res = db
        .delete(&[b"root".as_slice()], b"k1", None, Some(&tx), grove_version)
        .unwrap();
    if del_res.is_err() {
        panic!("delete in tx should succeed");
    }

    let in_tx = db
        .get(&[b"root".as_slice()], b"k1", Some(&tx), grove_version)
        .unwrap();
    if in_tx.is_ok() {
        panic!("in-tx read should not see deleted key");
    }

    let out_tx = db
        .get(&[b"root".as_slice()], b"k1", None, grove_version)
        .unwrap();
    if out_tx.is_err() {
        panic!("out-of-tx read should still see key before commit");
    }

    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("commit after tx delete should succeed");
    }
}

fn write_tx_delete_then_reinsert_same_key(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let del_res = db
        .delete(&[b"root".as_slice()], b"k1", None, Some(&tx), grove_version)
        .unwrap();
    if del_res.is_err() {
        panic!("delete before reinsert should succeed");
    }
    let ins_res = db
        .insert(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v2".to_vec()),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap();
    if ins_res.is_err() {
        panic!("reinsert after delete should succeed");
    }
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("commit after delete->reinsert should succeed");
    }
}

fn write_tx_insert_then_delete_same_key(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ins_res = db
        .insert(
            &[b"root".as_slice()],
            b"k1",
            Element::new_item(b"v2".to_vec()),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap();
    if ins_res.is_err() {
        panic!("insert before delete should succeed");
    }
    let del_res = db
        .delete(&[b"root".as_slice()], b"k1", None, Some(&tx), grove_version)
        .unwrap();
    if del_res.is_err() {
        panic!("delete after insert should succeed");
    }
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("commit after insert->delete should succeed");
    }
}

fn write_tx_delete_missing_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let del_res = db
        .delete(
            &[b"root".as_slice()],
            b"missing",
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap();
    if del_res.is_ok() {
        panic!("delete missing key should return path/key not found");
    }
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("commit after delete-missing should succeed");
    }
}

fn write_tx_read_committed_visibility(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();

    db.insert(
        &[b"root".as_slice()],
        b"ksnap",
        Element::new_item(b"sv".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 insert should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }

    let tx2_read = db
        .get(&[b"root".as_slice()], b"ksnap", Some(&tx2), grove_version)
        .unwrap();
    if tx2_read.is_err() {
        panic!("tx2 should see tx1 committed key");
    }

    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
}

fn write_tx_has_read_committed_visibility(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();

    db.insert(
        &[b"root".as_slice()],
        b"khas",
        Element::new_item(b"hv".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 insert should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }

    let in_tx2_has = db
        .has_raw(&[b"root".as_slice()], b"khas", Some(&tx2), grove_version)
        .unwrap()
        .expect("tx2 has_raw should succeed");
    if !in_tx2_has {
        panic!("tx2 has_raw should observe tx1 committed key");
    }

    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
}

fn write_tx_query_range_committed_visibility(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();

    db.insert(
        &[b"root".as_slice()],
        b"kqr",
        Element::new_item(b"qv".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 insert should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }

    let mut query = Query::new();
    query.insert_range(b"k".to_vec()..b"l".to_vec());
    let path_query = PathQuery::new(vec![b"root".to_vec()], SizedQuery::new(query, None, None));
    let (results, _) = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            Some(&tx2),
            grove_version,
        )
        .unwrap()
        .expect("tx2 query should succeed");
    let saw_kqr = results
        .to_key_elements()
        .into_iter()
        .any(|(k, _)| k.as_slice() == b"kqr");
    if !saw_kqr {
        panic!("tx2 query should include tx1 committed key");
    }

    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
}

fn write_tx_iterator_stability_under_commit(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();

    db.insert(
        &[b"root".as_slice()],
        b"kit",
        Element::new_item(b"iv".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 insert should succeed");

    let mut query = Query::new();
    query.insert_range(b"k".to_vec()..b"l".to_vec());
    let path_query = PathQuery::new(vec![b"root".to_vec()], SizedQuery::new(query, None, None));

    let (before_results, _) = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            Some(&tx2),
            grove_version,
        )
        .unwrap()
        .expect("tx2 pre-commit query should succeed");
    let saw_kit_before = before_results
        .to_key_elements()
        .into_iter()
        .any(|(k, _)| k.as_slice() == b"kit");
    if saw_kit_before {
        panic!("tx2 pre-commit query should not include uncommitted tx1 key");
    }

    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }

    let (after_results, _) = db
        .query_raw(
            &path_query,
            true,
            true,
            true,
            QueryKeyElementPairResultType,
            Some(&tx2),
            grove_version,
        )
        .unwrap()
        .expect("tx2 post-commit query should succeed");
    let saw_kit_after = after_results
        .to_key_elements()
        .into_iter()
        .any(|(k, _)| k.as_slice() == b"kit");
    if !saw_kit_after {
        panic!("tx2 post-commit query should include tx1 committed key");
    }

    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
}

fn write_tx_checkpoint_snapshot_isolation(
    db: &GroveDb,
    db_path: &Path,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"kcp",
        Element::new_item(b"cv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("tx insert before checkpoint should succeed");
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("tx commit before checkpoint should succeed");
    }

    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp".to_string());
    let checkpoint_path: PathBuf = db_path
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join(format!("{file_name}_checkpoint"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);

    db.create_checkpoint(&checkpoint_path)
        .expect("create checkpoint should succeed");

    db.insert(
        &[b"root".as_slice()],
        b"kpost",
        Element::new_item(b"pv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("post-checkpoint insert should succeed");

    {
        let checkpoint_db =
            GroveDb::open_checkpoint(&checkpoint_path).expect("open checkpoint should succeed");
        let cp_existing = checkpoint_db
            .get(&[b"root".as_slice()], b"kcp", None, grove_version)
            .unwrap();
        if cp_existing.is_err() {
            panic!("checkpoint should include pre-checkpoint committed key");
        }
        let cp_missing = checkpoint_db
            .get(&[b"root".as_slice()], b"kpost", None, grove_version)
            .unwrap();
        if cp_missing.is_ok() {
            panic!("checkpoint should not include post-checkpoint key");
        }
    }

    let main_post = db
        .get(&[b"root".as_slice()], b"kpost", None, grove_version)
        .unwrap();
    if main_post.is_err() {
        panic!("main db should include post-checkpoint key");
    }

    GroveDb::delete_checkpoint(&checkpoint_path).expect("delete checkpoint should succeed");
}

fn write_tx_checkpoint_independent_writes(
    db: &GroveDb,
    db_path: &Path,
    grove_version: &GroveVersion,
) {
    db.insert(
        &[] as &[&[u8]],
        b"key1",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("cannot insert key1 tree");
    db.insert(
        &[b"key1".as_slice()],
        b"key2",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("cannot insert key2 tree");
    db.insert(
        &[b"key1".as_slice(), b"key2".as_slice()],
        b"key3",
        Element::new_item(b"ayy".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("cannot insert key3 item");

    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp".to_string());
    let checkpoint_path: PathBuf = db_path
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join(format!("{file_name}_checkpoint_independent_writes"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);
    db.create_checkpoint(&checkpoint_path)
        .expect("cannot create checkpoint");

    {
        let checkpoint_db =
            GroveDb::open_checkpoint(&checkpoint_path).expect("cannot open checkpoint");

        checkpoint_db
            .insert(
                &[b"key1".as_slice()],
                b"key4",
                Element::new_item(b"ayy2".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot insert key4 into checkpoint");

        db.insert(
            &[b"key1".as_slice()],
            b"key4",
            Element::new_item(b"ayy3".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert key4 into main db");

        checkpoint_db
            .insert(
                &[b"key1".as_slice()],
                b"key5",
                Element::new_item(b"ayy3".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot insert key5 into checkpoint");

        db.insert(
            &[b"key1".as_slice()],
            b"key6",
            Element::new_item(b"ayy3".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert key6 into main db");

        if checkpoint_db
            .get(&[b"key1".as_slice()], b"key6", None, grove_version)
            .unwrap()
            .is_ok()
        {
            panic!("checkpoint should not see main-only key6");
        }
    }
    if db
        .get(&[b"key1".as_slice()], b"key5", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not see checkpoint-only key5");
    }

    GroveDb::delete_checkpoint(&checkpoint_path).expect("delete checkpoint should succeed");
}

fn write_tx_checkpoint_delete_safety(db: &GroveDb, db_path: &Path, grove_version: &GroveVersion) {
    write_simple(db, grove_version);

    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_path: PathBuf = parent.join(format!("{file_name}_checkpoint_delete_safety"));
    let bogus_path: PathBuf = parent.join(format!("{file_name}_not_checkpoint_dir"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);
    let _ = std::fs::remove_dir_all(&bogus_path);

    db.create_checkpoint(&checkpoint_path)
        .expect("create checkpoint for delete safety should succeed");
    std::fs::create_dir_all(&bogus_path).expect("create bogus dir should succeed");

    if GroveDb::delete_checkpoint(&bogus_path).is_ok() {
        panic!("delete_checkpoint should fail for non-checkpoint path");
    }

    GroveDb::delete_checkpoint(&checkpoint_path).expect("delete checkpoint should succeed");
    if checkpoint_path.exists() {
        panic!("checkpoint path should be removed");
    }

    let _ = std::fs::remove_dir_all(&bogus_path);

    db.insert(
        &[b"root".as_slice()],
        b"ksafe",
        Element::new_item(b"sv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("post-delete insert should succeed");
}

fn write_tx_checkpoint_open_safety(db: &GroveDb, db_path: &Path, grove_version: &GroveVersion) {
    write_simple(db, grove_version);

    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_path: PathBuf = parent.join(format!("{file_name}_checkpoint_open_safety"));
    let bogus_path: PathBuf = parent.join(format!("{file_name}_not_checkpoint_open_dir"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);
    let _ = std::fs::remove_dir_all(&bogus_path);

    db.create_checkpoint(&checkpoint_path)
        .expect("create checkpoint for open safety should succeed");
    std::fs::create_dir_all(&bogus_path).expect("create bogus dir should succeed");

    if GroveDb::open_checkpoint(&bogus_path).is_ok() {
        panic!("open_checkpoint should fail for non-checkpoint path");
    }

    {
        let checkpoint_db = GroveDb::open_checkpoint(&checkpoint_path)
            .expect("open_checkpoint should succeed for valid checkpoint");
        let existing = checkpoint_db
            .get(&[b"root".as_slice()], b"k1", None, grove_version)
            .unwrap();
        if existing.is_err() {
            panic!("valid checkpoint should contain baseline key");
        }
    }

    let _ = std::fs::remove_dir_all(&bogus_path);
    GroveDb::delete_checkpoint(&checkpoint_path).expect("delete checkpoint should succeed");

    db.insert(
        &[b"root".as_slice()],
        b"kopen",
        Element::new_item(b"ov".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("post-open-safety insert should succeed");
}

fn write_tx_checkpoint_delete_short_path_safety(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    if GroveDb::delete_checkpoint("single_component_path").is_ok() {
        panic!("delete_checkpoint should fail for short/single-component path");
    }
    db.insert(
        &[b"root".as_slice()],
        b"kshort",
        Element::new_item(b"sv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("post-short-path-safety insert should succeed");
}

fn write_tx_checkpoint_open_missing_path(
    db: &GroveDb,
    db_path: &Path,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let missing_path: PathBuf = parent.join(format!("{file_name}_checkpoint_missing_open"));
    let _ = std::fs::remove_dir_all(&missing_path);
    if GroveDb::open_checkpoint(&missing_path).is_ok() {
        panic!("open_checkpoint should fail for missing path");
    }
    db.insert(
        &[b"root".as_slice()],
        b"kmiss",
        Element::new_item(b"mv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("post-open-missing insert should succeed");
}

fn write_tx_checkpoint_reopen_mutate_recheckpoint(
    db: &GroveDb,
    db_path: &Path,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_a_path: PathBuf = parent.join(format!("{file_name}_checkpoint_reopen_mutate_a"));
    let checkpoint_b_path: PathBuf = parent.join(format!("{file_name}_checkpoint_reopen_mutate_b"));
    let _ = std::fs::remove_dir_all(&checkpoint_a_path);
    let _ = std::fs::remove_dir_all(&checkpoint_b_path);

    db.create_checkpoint(&checkpoint_a_path)
        .expect("create checkpoint A should succeed");

    {
        let checkpoint_a =
            GroveDb::open_checkpoint(&checkpoint_a_path).expect("open checkpoint A should succeed");
        checkpoint_a
            .insert(
                &[b"root".as_slice()],
                b"kcp1",
                Element::new_item(b"cv1".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("checkpoint A insert kcp1 should succeed");
    }

    {
        let checkpoint_a = GroveDb::open_checkpoint(&checkpoint_a_path)
            .expect("reopen checkpoint A should succeed");
        let cp1 = checkpoint_a
            .get(&[b"root".as_slice()], b"kcp1", None, grove_version)
            .unwrap();
        if cp1.is_err() {
            panic!("reopened checkpoint A should contain kcp1");
        }

        checkpoint_a
            .create_checkpoint(&checkpoint_b_path)
            .expect("checkpoint A should create checkpoint B");

        checkpoint_a
            .insert(
                &[b"root".as_slice()],
                b"kcp2",
                Element::new_item(b"cv2".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("checkpoint A insert kcp2 should succeed");
    }

    {
        let checkpoint_b =
            GroveDb::open_checkpoint(&checkpoint_b_path).expect("open checkpoint B should succeed");
        let cp1 = checkpoint_b
            .get(&[b"root".as_slice()], b"kcp1", None, grove_version)
            .unwrap();
        if cp1.is_err() {
            panic!("checkpoint B should contain kcp1");
        }
        let cp2 = checkpoint_b
            .get(&[b"root".as_slice()], b"kcp2", None, grove_version)
            .unwrap();
        if cp2.is_ok() {
            panic!("checkpoint B should not contain post-recheckpoint key kcp2");
        }
    }

    db.insert(
        &[b"root".as_slice()],
        b"kmain",
        Element::new_item(b"mv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main db insert kmain should succeed");

    if db
        .get(&[b"root".as_slice()], b"kcp1", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kcp1");
    }
    if db
        .get(&[b"root".as_slice()], b"kcp2", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kcp2");
    }

    GroveDb::delete_checkpoint(&checkpoint_b_path).expect("delete checkpoint B should succeed");
    GroveDb::delete_checkpoint(&checkpoint_a_path).expect("delete checkpoint A should succeed");
}

fn write_tx_checkpoint_reopen_mutate_chain(
    db: &GroveDb,
    db_path: &Path,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_a_path: PathBuf = parent.join(format!("{file_name}_checkpoint_chain_a"));
    let checkpoint_b_path: PathBuf = parent.join(format!("{file_name}_checkpoint_chain_b"));
    let checkpoint_c_path: PathBuf = parent.join(format!("{file_name}_checkpoint_chain_c"));
    let _ = std::fs::remove_dir_all(&checkpoint_a_path);
    let _ = std::fs::remove_dir_all(&checkpoint_b_path);
    let _ = std::fs::remove_dir_all(&checkpoint_c_path);

    db.create_checkpoint(&checkpoint_a_path)
        .expect("create checkpoint A should succeed");

    {
        let checkpoint_a =
            GroveDb::open_checkpoint(&checkpoint_a_path).expect("open checkpoint A should succeed");
        checkpoint_a
            .insert(
                &[b"root".as_slice()],
                b"ka1",
                Element::new_item(b"ava1".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("checkpoint A insert ka1 should succeed");
        checkpoint_a
            .create_checkpoint(&checkpoint_b_path)
            .expect("checkpoint A create checkpoint B should succeed");
    }

    {
        let checkpoint_b =
            GroveDb::open_checkpoint(&checkpoint_b_path).expect("open checkpoint B should succeed");
        let from_a = checkpoint_b
            .get(&[b"root".as_slice()], b"ka1", None, grove_version)
            .unwrap();
        if from_a.is_err() {
            panic!("checkpoint B should contain checkpoint A key ka1");
        }
        checkpoint_b
            .insert(
                &[b"root".as_slice()],
                b"kb1",
                Element::new_item(b"bva1".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("checkpoint B insert kb1 should succeed");
        checkpoint_b
            .create_checkpoint(&checkpoint_c_path)
            .expect("checkpoint B create checkpoint C should succeed");
        checkpoint_b
            .insert(
                &[b"root".as_slice()],
                b"kb2",
                Element::new_item(b"bva2".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("checkpoint B insert kb2 should succeed");
    }

    {
        let checkpoint_c =
            GroveDb::open_checkpoint(&checkpoint_c_path).expect("open checkpoint C should succeed");
        let ka1 = checkpoint_c
            .get(&[b"root".as_slice()], b"ka1", None, grove_version)
            .unwrap();
        if ka1.is_err() {
            panic!("checkpoint C should contain ka1");
        }
        let kb1 = checkpoint_c
            .get(&[b"root".as_slice()], b"kb1", None, grove_version)
            .unwrap();
        if kb1.is_err() {
            panic!("checkpoint C should contain kb1");
        }
        let kb2 = checkpoint_c
            .get(&[b"root".as_slice()], b"kb2", None, grove_version)
            .unwrap();
        if kb2.is_ok() {
            panic!("checkpoint C should not contain post-checkpoint key kb2");
        }
    }

    db.insert(
        &[b"root".as_slice()],
        b"kmain2",
        Element::new_item(b"mv2".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main db insert kmain2 should succeed");

    if db
        .get(&[b"root".as_slice()], b"ka1", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key ka1");
    }
    if db
        .get(&[b"root".as_slice()], b"kb1", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kb1");
    }
    if db
        .get(&[b"root".as_slice()], b"kb2", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kb2");
    }

    GroveDb::delete_checkpoint(&checkpoint_c_path).expect("delete checkpoint C should succeed");
    GroveDb::delete_checkpoint(&checkpoint_b_path).expect("delete checkpoint B should succeed");
    GroveDb::delete_checkpoint(&checkpoint_a_path).expect("delete checkpoint A should succeed");
}

fn write_tx_checkpoint_batch_ops(db: &GroveDb, db_path: &Path, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp_batch".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_path: PathBuf = parent.join(format!("{file_name}_checkpoint_batch"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);

    db.create_checkpoint(&checkpoint_path)
        .expect("create checkpoint should succeed");

    {
        let checkpoint =
            GroveDb::open_checkpoint(&checkpoint_path).expect("open checkpoint should succeed");

        let mut ops = vec![];
        ops.push(QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"kbp1".to_vec(),
            Element::new_item(b"vbp1".to_vec()),
        ));
        ops.push(QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"kbp2".to_vec(),
            Element::new_item(b"vbp2".to_vec()),
        ));
        ops.push(QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"kbp3".to_vec(),
            Element::new_item(b"vbp3".to_vec()),
        ));

        checkpoint
            .apply_batch(ops, None, None, grove_version)
            .value
            .expect("checkpoint batch insert should succeed");
    }

    {
        let checkpoint =
            GroveDb::open_checkpoint(&checkpoint_path).expect("reopen checkpoint should succeed");

        let kbp1 = checkpoint
            .get(&[b"root".as_slice()], b"kbp1", None, grove_version)
            .unwrap();
        if kbp1.is_err() {
            panic!("checkpoint should contain kbp1 after batch insert");
        }

        let kbp2 = checkpoint
            .get(&[b"root".as_slice()], b"kbp2", None, grove_version)
            .unwrap();
        if kbp2.is_err() {
            panic!("checkpoint should contain kbp2 after batch insert");
        }

        let kbp3 = checkpoint
            .get(&[b"root".as_slice()], b"kbp3", None, grove_version)
            .unwrap();
        if kbp3.is_err() {
            panic!("checkpoint should contain kbp3 after batch insert");
        }
    }

    if db
        .get(&[b"root".as_slice()], b"kbp1", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kbp1");
    }
    if db
        .get(&[b"root".as_slice()], b"kbp2", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kbp2");
    }
    if db
        .get(&[b"root".as_slice()], b"kbp3", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kbp3");
    }

    db.insert(
        &[b"root".as_slice()],
        b"kmain3",
        Element::new_item(b"mv3".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main db insert kmain3 should succeed");

    db.insert(
        &[b"root".as_slice()],
        b"kbatch1",
        Element::new_item(b"vb1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main db insert kbatch1 should succeed");

    if db
        .get(&[b"root".as_slice()], b"kbatch1", None, grove_version)
        .unwrap()
        .is_ok()
    {
        // This should be present
    } else {
        panic!("main db should contain kbatch1 after batch");
    }

    GroveDb::delete_checkpoint(&checkpoint_path).expect("delete checkpoint should succeed");
}

fn write_tx_checkpoint_tx_operations(db: &GroveDb, db_path: &Path, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp_tx".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_path: PathBuf = parent.join(format!("{file_name}_checkpoint_tx"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);

    let tx = db.start_transaction();

    db.insert(
        &[b"root".as_slice()],
        b"ktx1",
        Element::new_item(b"txv1".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("tx insert ktx1 should succeed");

    db.create_checkpoint(&checkpoint_path)
        .expect("create checkpoint during tx should succeed");

    db.insert(
        &[b"root".as_slice()],
        b"ktx2",
        Element::new_item(b"txv2".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("tx insert ktx2 should succeed");

    db.commit_transaction(tx)
        .unwrap()
        .expect("commit transaction should succeed");

    {
        let checkpoint =
            GroveDb::open_checkpoint(&checkpoint_path).expect("open checkpoint should succeed");
        let ktx1 = checkpoint
            .get(&[b"root".as_slice()], b"ktx1", None, grove_version)
            .unwrap();
        if ktx1.is_ok() {
            panic!("checkpoint should not contain ktx1 (uncommitted at checkpoint time)");
        }
        let ktx2 = checkpoint
            .get(&[b"root".as_slice()], b"ktx2", None, grove_version)
            .unwrap();
        if ktx2.is_ok() {
            panic!("checkpoint should not contain ktx2 (uncommitted at checkpoint time)");
        }
    }

    {
        let main_val = db
            .get(&[b"root".as_slice()], b"ktx1", None, grove_version)
            .unwrap();
        if main_val.is_err() {
            panic!("main db should contain ktx1 after commit");
        }
        let main_val2 = db
            .get(&[b"root".as_slice()], b"ktx2", None, grove_version)
            .unwrap();
        if main_val2.is_err() {
            panic!("main db should contain ktx2 after commit");
        }
    }

    GroveDb::delete_checkpoint(&checkpoint_path).expect("delete checkpoint should succeed");
}

fn write_tx_checkpoint_delete_reopen_sequence(
    db: &GroveDb,
    db_path: &Path,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp_seq".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_a_path: PathBuf = parent.join(format!("{file_name}_checkpoint_seq_a"));
    let checkpoint_b_path: PathBuf = parent.join(format!("{file_name}_checkpoint_seq_b"));
    let _ = std::fs::remove_dir_all(&checkpoint_a_path);
    let _ = std::fs::remove_dir_all(&checkpoint_b_path);

    db.create_checkpoint(&checkpoint_a_path)
        .expect("create checkpoint A should succeed");

    {
        let checkpoint_a =
            GroveDb::open_checkpoint(&checkpoint_a_path).expect("open checkpoint A should succeed");
        let baseline = checkpoint_a
            .get(&[b"root".as_slice()], b"k1", None, grove_version)
            .unwrap();
        if baseline.is_err() {
            panic!("checkpoint A should contain baseline key k1");
        }
        checkpoint_a
            .create_checkpoint(&checkpoint_b_path)
            .expect("checkpoint A create checkpoint B should succeed");
    }

    GroveDb::delete_checkpoint(&checkpoint_a_path).expect("delete checkpoint A should succeed");
    if GroveDb::open_checkpoint(&checkpoint_a_path).is_ok() {
        panic!("opening deleted checkpoint A should fail");
    }

    {
        let checkpoint_b =
            GroveDb::open_checkpoint(&checkpoint_b_path).expect("open checkpoint B should succeed");
        let baseline = checkpoint_b
            .get(&[b"root".as_slice()], b"k1", None, grove_version)
            .unwrap();
        if baseline.is_err() {
            panic!("checkpoint B should contain baseline key k1");
        }
        checkpoint_b
            .insert(
                &[b"root".as_slice()],
                b"kcpb",
                Element::new_item(b"cpb".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("checkpoint B insert kcpb should succeed");
    }

    GroveDb::delete_checkpoint(&checkpoint_b_path).expect("delete checkpoint B should succeed");
    if GroveDb::open_checkpoint(&checkpoint_b_path).is_ok() {
        panic!("opening deleted checkpoint B should fail");
    }

    let _ = std::fs::remove_dir_all(&checkpoint_a_path);
    db.create_checkpoint(&checkpoint_a_path)
        .expect("recreate checkpoint A from main db should succeed");
    {
        let checkpoint_a = GroveDb::open_checkpoint(&checkpoint_a_path)
            .expect("open recreated checkpoint A should succeed");
        let baseline = checkpoint_a
            .get(&[b"root".as_slice()], b"k1", None, grove_version)
            .unwrap();
        if baseline.is_err() {
            panic!("recreated checkpoint A should contain baseline key k1");
        }
        checkpoint_a
            .insert(
                &[b"root".as_slice()],
                b"kcpa",
                Element::new_item(b"cpa".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("recreated checkpoint A insert kcpa should succeed");
    }
    GroveDb::delete_checkpoint(&checkpoint_a_path).expect("delete recreated checkpoint A succeeds");
    if GroveDb::open_checkpoint(&checkpoint_a_path).is_ok() {
        panic!("opening deleted recreated checkpoint A should fail");
    }

    if db
        .get(&[b"root".as_slice()], b"kcpb", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain checkpoint-only key kcpb");
    }
    if db
        .get(&[b"root".as_slice()], b"kcpa", None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("main db should not contain recreated checkpoint-only key kcpa");
    }

    db.insert(
        &[b"root".as_slice()],
        b"kseq",
        Element::new_item(b"sv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main db insert kseq should succeed");
}

fn write_tx_checkpoint_chain_mutation_isolation(
    db: &GroveDb,
    db_path: &Path,
    grove_version: &GroveVersion,
) {
    // Setup: create base tree structure
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"k_base",
        Element::new_item(b"base".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert k_base");

    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp_chain".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_a_path: PathBuf = parent.join(format!("{file_name}_checkpoint_a"));
    let checkpoint_b_path: PathBuf = parent.join(format!("{file_name}_checkpoint_b"));
    let checkpoint_c_path: PathBuf = parent.join(format!("{file_name}_checkpoint_c"));
    let _ = std::fs::remove_dir_all(&checkpoint_a_path);
    let _ = std::fs::remove_dir_all(&checkpoint_b_path);
    let _ = std::fs::remove_dir_all(&checkpoint_c_path);

    // Phase 1: Create checkpoint A (contains k_base=base)
    db.create_checkpoint(&checkpoint_a_path)
        .expect("create checkpoint A should succeed");

    // Phase 2: Mutate main DB, create checkpoint B
    db.insert(
        &[b"root".as_slice()],
        b"k_phase2",
        Element::new_item(b"phase2".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main DB insert k_phase2 should succeed");
    db.create_checkpoint(&checkpoint_b_path)
        .expect("create checkpoint B should succeed");

    // Phase 3: Mutate main DB again, create checkpoint C
    db.insert(
        &[b"root".as_slice()],
        b"k_phase3",
        Element::new_item(b"phase3".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main DB insert k_phase3 should succeed");
    db.create_checkpoint(&checkpoint_c_path)
        .expect("create checkpoint C should succeed");

    // Phase 4: Final main DB mutation
    db.insert(
        &[b"root".as_slice()],
        b"k_final",
        Element::new_item(b"final".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main DB insert k_final should succeed");

    // Verify checkpoint A: should only have k_base
    {
        let checkpoint_a =
            GroveDb::open_checkpoint(&checkpoint_a_path).expect("open checkpoint A should succeed");
        let base = checkpoint_a
            .get(&[b"root".as_slice()], b"k_base", None, grove_version)
            .unwrap();
        if base.is_err() {
            panic!("checkpoint A should contain k_base");
        }
        let phase2 = checkpoint_a
            .get(&[b"root".as_slice()], b"k_phase2", None, grove_version)
            .unwrap();
        if phase2.is_ok() {
            panic!("checkpoint A should not contain k_phase2");
        }
        let phase3 = checkpoint_a
            .get(&[b"root".as_slice()], b"k_phase3", None, grove_version)
            .unwrap();
        if phase3.is_ok() {
            panic!("checkpoint A should not contain k_phase3");
        }
        let final_key = checkpoint_a
            .get(&[b"root".as_slice()], b"k_final", None, grove_version)
            .unwrap();
        if final_key.is_ok() {
            panic!("checkpoint A should not contain k_final");
        }
    }

    // Verify checkpoint B: should have k_base + k_phase2
    {
        let checkpoint_b =
            GroveDb::open_checkpoint(&checkpoint_b_path).expect("open checkpoint B should succeed");
        let base = checkpoint_b
            .get(&[b"root".as_slice()], b"k_base", None, grove_version)
            .unwrap();
        if base.is_err() {
            panic!("checkpoint B should contain k_base");
        }
        let phase2 = checkpoint_b
            .get(&[b"root".as_slice()], b"k_phase2", None, grove_version)
            .unwrap();
        if phase2.is_err() {
            panic!("checkpoint B should contain k_phase2");
        }
        let phase3 = checkpoint_b
            .get(&[b"root".as_slice()], b"k_phase3", None, grove_version)
            .unwrap();
        if phase3.is_ok() {
            panic!("checkpoint B should not contain k_phase3");
        }
        let final_key = checkpoint_b
            .get(&[b"root".as_slice()], b"k_final", None, grove_version)
            .unwrap();
        if final_key.is_ok() {
            panic!("checkpoint B should not contain k_final");
        }
    }

    // Verify checkpoint C: should have k_base + k_phase2 + k_phase3
    {
        let checkpoint_c =
            GroveDb::open_checkpoint(&checkpoint_c_path).expect("open checkpoint C should succeed");
        let base = checkpoint_c
            .get(&[b"root".as_slice()], b"k_base", None, grove_version)
            .unwrap();
        if base.is_err() {
            panic!("checkpoint C should contain k_base");
        }
        let phase2 = checkpoint_c
            .get(&[b"root".as_slice()], b"k_phase2", None, grove_version)
            .unwrap();
        if phase2.is_err() {
            panic!("checkpoint C should contain k_phase2");
        }
        let phase3 = checkpoint_c
            .get(&[b"root".as_slice()], b"k_phase3", None, grove_version)
            .unwrap();
        if phase3.is_err() {
            panic!("checkpoint C should contain k_phase3");
        }
        let final_key = checkpoint_c
            .get(&[b"root".as_slice()], b"k_final", None, grove_version)
            .unwrap();
        if final_key.is_ok() {
            panic!("checkpoint C should not contain k_final");
        }
    }

    // Verify main DB: should have all keys
    {
        let main_keys: Vec<&[u8]> = vec![b"k_base", b"k_phase2", b"k_phase3", b"k_final"];
        for key in main_keys {
            let result = db
                .get(&[b"root".as_slice()], key, None, grove_version)
                .unwrap();
            if result.is_err() {
                panic!("main DB should contain {}", String::from_utf8_lossy(key));
            }
        }
    }

    // Cleanup
    let _ = std::fs::remove_dir_all(&checkpoint_a_path);
    let _ = std::fs::remove_dir_all(&checkpoint_b_path);
    let _ = std::fs::remove_dir_all(&checkpoint_c_path);
}

fn write_tx_checkpoint_reopen_after_main_delete(db: &GroveDb, db_path: &Path, grove_version: &GroveVersion) {
    // Setup: create base tree structure
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"k_base",
        Element::new_item(b"base".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert k_base");
    db.insert(
        &[b"root".as_slice()],
        b"k_snapshot",
        Element::new_item(b"snapshot_data".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert k_snapshot");

    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp_reopen".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_path: PathBuf = parent.join(format!("{file_name}_checkpoint"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);

    // Phase 1: Create checkpoint with snapshot data
    db.create_checkpoint(&checkpoint_path)
        .expect("create checkpoint should succeed");

    // Phase 2: Verify checkpoint contains snapshot data
    {
        let checkpoint =
            GroveDb::open_checkpoint(&checkpoint_path).expect("open checkpoint should succeed");
        let base = checkpoint
            .get(&[b"root".as_slice()], b"k_base", None, grove_version)
            .unwrap()
            .expect("checkpoint get k_base should succeed");
        assert_eq!(
            base,
            Element::Item(b"base".to_vec(), None),
            "checkpoint k_base should be base"
        );
        let snapshot = checkpoint
            .get(&[b"root".as_slice()], b"k_snapshot", None, grove_version)
            .unwrap()
            .expect("checkpoint get k_snapshot should succeed");
        assert_eq!(
            snapshot,
            Element::Item(b"snapshot_data".to_vec(), None),
            "checkpoint k_snapshot should be snapshot_data"
        );
    }

    // Phase 3: Delete the main database
    drop(db);
    std::fs::remove_dir_all(db_path).expect("delete main DB should succeed");

    // Phase 4: Reopen checkpoint after main DB deletion and verify data persists
    {
        let checkpoint =
            GroveDb::open_checkpoint(&checkpoint_path).expect("reopen checkpoint after main delete should succeed");
        let base = checkpoint
            .get(&[b"root".as_slice()], b"k_base", None, grove_version)
            .unwrap()
            .expect("reopen checkpoint get k_base should succeed");
        assert_eq!(
            base,
            Element::Item(b"base".to_vec(), None),
            "reopen checkpoint k_base should still be base"
        );
        let snapshot = checkpoint
            .get(&[b"root".as_slice()], b"k_snapshot", None, grove_version)
            .unwrap()
            .expect("reopen checkpoint get k_snapshot should succeed");
        assert_eq!(
            snapshot,
            Element::Item(b"snapshot_data".to_vec(), None),
            "reopen checkpoint k_snapshot should still be snapshot_data"
        );
    }

    // Cleanup
    let _ = std::fs::remove_dir_all(&checkpoint_path);
}

fn write_tx_checkpoint_aux_isolation(db: &GroveDb, db_path: &Path, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.put_aux(b"aux_shared", b"main_before", None, None)
        .unwrap()
        .expect("main db put_aux aux_shared before checkpoint should succeed");

    let file_name = db_path
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "grovedb_tx_cp_aux".to_string());
    let parent = db_path.parent().unwrap_or_else(|| Path::new("."));
    let checkpoint_path: PathBuf = parent.join(format!("{file_name}_checkpoint_aux"));
    let _ = std::fs::remove_dir_all(&checkpoint_path);

    db.create_checkpoint(&checkpoint_path)
        .expect("create checkpoint aux isolation should succeed");

    db.put_aux(b"aux_shared", b"main_after", None, None)
        .unwrap()
        .expect("main db put_aux aux_shared after checkpoint should succeed");
    db.put_aux(b"aux_main_only", b"main_only", None, None)
        .unwrap()
        .expect("main db put_aux aux_main_only should succeed");

    {
        let checkpoint =
            GroveDb::open_checkpoint(&checkpoint_path).expect("open checkpoint aux should succeed");

        let shared = checkpoint
            .get_aux(b"aux_shared", None)
            .unwrap()
            .expect("checkpoint get_aux aux_shared should succeed");
        if shared != Some(b"main_before".to_vec()) {
            panic!("checkpoint aux_shared should be snapshotted to main_before");
        }

        let main_only = checkpoint
            .get_aux(b"aux_main_only", None)
            .unwrap()
            .expect("checkpoint get_aux aux_main_only should succeed");
        if main_only.is_some() {
            panic!("checkpoint should not see post-checkpoint aux_main_only");
        }

        checkpoint
            .put_aux(b"aux_cp_only", b"cp_only", None, None)
            .unwrap()
            .expect("checkpoint put_aux aux_cp_only should succeed");
        checkpoint
            .put_aux(b"aux_shared", b"cp_override", None, None)
            .unwrap()
            .expect("checkpoint put_aux aux_shared override should succeed");
    }

    let main_shared = db
        .get_aux(b"aux_shared", None)
        .unwrap()
        .expect("main db get_aux aux_shared should succeed");
    if main_shared != Some(b"main_after".to_vec()) {
        panic!("main db aux_shared should remain main_after");
    }
    let cp_only = db
        .get_aux(b"aux_cp_only", None)
        .unwrap()
        .expect("main db get_aux aux_cp_only should succeed");
    if cp_only.is_some() {
        panic!("main db should not see checkpoint-only aux_cp_only");
    }

    GroveDb::delete_checkpoint(&checkpoint_path).expect("delete checkpoint aux should succeed");

    db.insert(
        &[b"root".as_slice()],
        b"kauxcp",
        Element::new_item(b"ok".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("main db insert kauxcp should succeed");
}

fn write_tx_same_key_conflict_reverse_order(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();

    db.insert(
        &[b"root".as_slice()],
        b"kconf",
        Element::new_item(b"v1".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 insert should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"kconf",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 insert should succeed");

    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 commit should fail due to conflict");
    }
}

fn write_tx_shared_subtree_disjoint_conflict_reverse_order(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();

    db.insert(
        &[b"root".as_slice()],
        b"kd1",
        Element::new_item(b"d1".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 disjoint insert should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"kd2",
        Element::new_item(b"d2".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 disjoint insert should succeed");

    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 disjoint commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 disjoint commit should fail due to shared-subtree conflict");
    }
}

fn write_tx_disjoint_subtree_conflict_reverse_order(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"left",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert left subtree");
    db.insert(
        &[b"root".as_slice()],
        b"right",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert right subtree");

    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice(), b"left".as_slice()],
        b"lk",
        Element::new_item(b"lv".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 disjoint-subtree insert should succeed");
    db.insert(
        &[b"root".as_slice(), b"right".as_slice()],
        b"rk",
        Element::new_item(b"rv".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 disjoint-subtree insert should succeed");
    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 disjoint-subtree commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 disjoint-subtree commit should fail due to conflict");
    }
}

fn write_tx_disjoint_subtree_conflict_forward_order(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"left",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert left subtree");
    db.insert(
        &[b"root".as_slice()],
        b"right",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert right subtree");

    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice(), b"left".as_slice()],
        b"lkf",
        Element::new_item(b"lvf".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 forward disjoint-subtree insert should succeed");
    db.insert(
        &[b"root".as_slice(), b"right".as_slice()],
        b"rkf",
        Element::new_item(b"rvf".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 forward disjoint-subtree insert should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 forward disjoint-subtree commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 forward disjoint-subtree commit should fail due to conflict");
    }
}

fn write_tx_read_only_then_writer_commit(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx_ro = db.start_transaction();
    let tx_w = db.start_transaction();

    let ro_before = db
        .get(&[b"root".as_slice()], b"k1", Some(&tx_ro), grove_version)
        .unwrap();
    if ro_before.is_err() {
        panic!("read-only tx should see existing key");
    }

    db.insert(
        &[b"root".as_slice()],
        b"kro",
        Element::new_item(b"rov".to_vec()),
        None,
        Some(&tx_w),
        grove_version,
    )
    .unwrap()
    .expect("writer tx insert should succeed");
    if db.commit_transaction(tx_w).unwrap().is_err() {
        panic!("writer tx commit should succeed");
    }

    let ro_after = db
        .get(&[b"root".as_slice()], b"kro", Some(&tx_ro), grove_version)
        .unwrap();
    if ro_after.is_err() {
        panic!("read-only tx should see writer-committed key");
    }

    if db.commit_transaction(tx_ro).unwrap().is_err() {
        panic!("read-only tx commit should succeed");
    }
}

fn write_tx_delete_insert_same_key_forward(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.delete(
        &[b"root".as_slice()],
        b"k1",
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 delete should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 insert should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 late commit should conflict");
    }
}

fn write_tx_delete_insert_same_key_reverse(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.delete(
        &[b"root".as_slice()],
        b"k1",
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 delete should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 insert should succeed");
    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 late commit should conflict");
    }
}

fn write_tx_delete_insert_same_subtree_disjoint_forward(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.delete(
        &[b"root".as_slice()],
        b"k1",
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 delete should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"kdi",
        Element::new_item(b"di".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 disjoint insert should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 late commit should conflict");
    }
}

fn write_tx_delete_insert_same_subtree_disjoint_reverse(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.delete(
        &[b"root".as_slice()],
        b"k1",
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 delete should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"kdi",
        Element::new_item(b"di".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 disjoint insert should succeed");
    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 late commit should conflict");
    }
}

fn write_tx_delete_insert_disjoint_subtree_forward(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"left",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert left subtree");
    db.insert(
        &[b"root".as_slice()],
        b"right",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert right subtree");
    db.insert(
        &[b"root".as_slice(), b"left".as_slice()],
        b"lkdel",
        Element::new_item(b"delv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert left key to delete");
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.delete(
        &[b"root".as_slice(), b"left".as_slice()],
        b"lkdel",
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 delete in left subtree should succeed");
    db.insert(
        &[b"root".as_slice(), b"right".as_slice()],
        b"rkins",
        Element::new_item(b"insv".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 insert in right subtree should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 late commit should conflict");
    }
}

fn write_tx_delete_insert_disjoint_subtree_reverse(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"left",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert left subtree");
    db.insert(
        &[b"root".as_slice()],
        b"right",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert right subtree");
    db.insert(
        &[b"root".as_slice(), b"left".as_slice()],
        b"lkdel",
        Element::new_item(b"delv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert left key to delete");
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.delete(
        &[b"root".as_slice(), b"left".as_slice()],
        b"lkdel",
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 delete in left subtree should succeed");
    db.insert(
        &[b"root".as_slice(), b"right".as_slice()],
        b"rkins",
        Element::new_item(b"insv".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 insert in right subtree should succeed");
    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 late commit should conflict");
    }
}

fn write_tx_replace_delete_same_key_forward(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 replace should succeed");
    db.delete(
        &[b"root".as_slice()],
        b"k1",
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 delete should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 late commit should conflict");
    }
}

fn write_tx_replace_delete_same_key_reverse(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 replace should succeed");
    db.delete(
        &[b"root".as_slice()],
        b"k1",
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 delete should succeed");
    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 late commit should conflict");
    }
}

fn write_tx_replace_replace_same_key_forward(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 replace should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v3".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 replace should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 late commit should conflict");
    }
}

fn write_tx_replace_replace_same_key_reverse(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v2".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 replace should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"k1",
        Element::new_item(b"v3".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 replace should succeed");
    if db.commit_transaction(tx2).unwrap().is_err() {
        panic!("tx2 commit should succeed");
    }
    if db.commit_transaction(tx1).unwrap().is_ok() {
        panic!("tx1 late commit should conflict");
    }
}

fn write_tx_double_rollback_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    db.rollback_transaction(&tx)
        .expect("first rollback should succeed");
    db.rollback_transaction(&tx)
        .expect("second rollback should also succeed");
    db.insert(
        &[b"root".as_slice()],
        b"kdr",
        Element::new_item(b"drv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert after double rollback should succeed");
    if db.commit_transaction(tx).unwrap().is_err() {
        panic!("commit after double rollback should succeed");
    }
}

fn write_tx_conflict_sequence_persistence(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"left",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert left subtree");
    db.insert(
        &[b"root".as_slice()],
        b"right",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert right subtree");

    let tx1 = db.start_transaction();
    let tx2 = db.start_transaction();
    db.insert(
        &[b"root".as_slice()],
        b"kc",
        Element::new_item(b"a".to_vec()),
        None,
        Some(&tx1),
        grove_version,
    )
    .unwrap()
    .expect("tx1 same-key insert should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"kc",
        Element::new_item(b"b".to_vec()),
        None,
        Some(&tx2),
        grove_version,
    )
    .unwrap()
    .expect("tx2 same-key insert should succeed");
    if db.commit_transaction(tx1).unwrap().is_err() {
        panic!("tx1 commit should succeed");
    }
    if db.commit_transaction(tx2).unwrap().is_ok() {
        panic!("tx2 late commit should conflict");
    }

    let tx3 = db.start_transaction();
    let tx4 = db.start_transaction();
    db.insert(
        &[b"root".as_slice(), b"left".as_slice()],
        b"lk1",
        Element::new_item(b"l1".to_vec()),
        None,
        Some(&tx3),
        grove_version,
    )
    .unwrap()
    .expect("tx3 left insert should succeed");
    db.insert(
        &[b"root".as_slice(), b"right".as_slice()],
        b"rk1",
        Element::new_item(b"r1".to_vec()),
        None,
        Some(&tx4),
        grove_version,
    )
    .unwrap()
    .expect("tx4 right insert should succeed");
    if db.commit_transaction(tx4).unwrap().is_err() {
        panic!("tx4 commit should succeed");
    }
    if db.commit_transaction(tx3).unwrap().is_ok() {
        panic!("tx3 late commit should conflict");
    }

    let tx5 = db.start_transaction();
    db.rollback_transaction(&tx5)
        .expect("tx5 rollback should succeed");
    db.insert(
        &[b"root".as_slice()],
        b"kpost",
        Element::new_item(b"pv".to_vec()),
        None,
        Some(&tx5),
        grove_version,
    )
    .unwrap()
    .expect("insert after rollback should succeed");
    if db.commit_transaction(tx5).unwrap().is_err() {
        panic!("tx5 commit should succeed");
    }
}

fn write_batch_apply_local_atomic(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);

    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"k2".to_vec(),
            Element::new_item(b"b2".to_vec()),
        ),
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"k1".to_vec()),
    ];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("apply_batch local success");

    let failing_ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"nt".to_vec(),
            Element::new_item(b"n".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"nt".to_vec()],
            b"kbad".to_vec(),
            Element::new_item(b"x".to_vec()),
        ),
    ];
    if db
        .apply_batch(failing_ops, None, None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("expected local failing batch to rollback");
    }
}

fn write_batch_apply_tx_visibility(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"ktxb".to_vec(),
        Element::new_item(b"tb".to_vec()),
    )];
    db.apply_batch(ops, None, Some(&tx), grove_version)
        .unwrap()
        .expect("apply_batch tx success");

    let outside = db
        .get(&[b"root".as_slice()], b"ktxb", None, grove_version)
        .unwrap();
    if outside.is_ok() {
        panic!("uncommitted tx batch value leaked outside tx");
    }

    db.commit_transaction(tx)
        .unwrap()
        .expect("tx batch commit should succeed");
}

fn write_batch_apply_empty_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.apply_batch(Vec::<QualifiedGroveDbOp>::new(), None, None, grove_version)
        .unwrap()
        .expect("empty batch should be noop");
}

fn write_batch_validate_success_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"kval".to_vec(),
            Element::new_item(b"vv".to_vec()),
        ),
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"k1".to_vec()),
    ];
    let apply_result = db.apply_batch(ops, None, Some(&tx), grove_version).unwrap();
    if apply_result.is_err() {
        panic!("validate success batch should apply in tx");
    }
    db.rollback_transaction(&tx)
        .expect("validate success rollback should succeed");
}

fn write_batch_validate_failure_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"nval".to_vec(),
            Element::new_item(b"n".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"nval".to_vec()],
            b"kbad".to_vec(),
            Element::new_item(b"x".to_vec()),
        ),
    ];
    let apply_result = db.apply_batch(ops, None, Some(&tx), grove_version).unwrap();
    if apply_result.is_ok() {
        panic!("validate failing batch should error");
    }
    db.rollback_transaction(&tx)
        .expect("validate failure rollback should succeed");
}

fn write_batch_insert_only_semantics(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let success_ops = vec![QualifiedGroveDbOp::insert_only_op(
        vec![b"root".to_vec()],
        b"kio".to_vec(),
        Element::new_item(b"io".to_vec()),
    )];
    db.apply_batch(success_ops, None, None, grove_version)
        .unwrap()
        .expect("insert_only success batch should apply");

    let fail_ops = vec![QualifiedGroveDbOp::insert_only_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"xx".to_vec()),
    )];
    db.apply_batch(fail_ops, None, None, grove_version)
        .unwrap()
        .expect("insert_only existing-key should upsert");
}

fn write_batch_replace_semantics(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let success_ops = vec![QualifiedGroveDbOp::replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"r2".to_vec()),
    )];
    db.apply_batch(success_ops, None, None, grove_version)
        .unwrap()
        .expect("replace existing-key batch should apply");

    let fail_ops = vec![QualifiedGroveDbOp::replace_op(
        vec![b"root".to_vec()],
        b"kmiss".to_vec(),
        Element::new_item(b"rx".to_vec()),
    )];
    db.apply_batch(fail_ops, None, None, grove_version)
        .unwrap()
        .expect("replace missing-key should upsert");
}

fn write_batch_validate_no_override_insert(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let strict = BatchApplyOptions {
        validate_insertion_does_not_override: true,
        ..Default::default()
    };
    let fail_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"ov".to_vec()),
    )];
    if db
        .apply_batch(fail_ops, Some(strict.clone()), None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("strict batch insert override should fail");
    }
    let success_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"kstrict".to_vec(),
        Element::new_item(b"sv".to_vec()),
    )];
    db.apply_batch(success_ops, Some(strict), None, grove_version)
        .unwrap()
        .expect("strict batch insert new key should pass");
}

fn write_batch_validate_no_override_insert_only(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let strict = BatchApplyOptions {
        validate_insertion_does_not_override: true,
        ..Default::default()
    };
    let fail_ops = vec![QualifiedGroveDbOp::insert_only_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"io".to_vec()),
    )];
    if db
        .apply_batch(fail_ops, Some(strict.clone()), None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("strict batch insert_only override should fail");
    }
    let success_ops = vec![QualifiedGroveDbOp::insert_only_op(
        vec![b"root".to_vec()],
        b"kio2".to_vec(),
        Element::new_item(b"iv".to_vec()),
    )];
    db.apply_batch(success_ops, Some(strict), None, grove_version)
        .unwrap()
        .expect("strict batch insert_only new key should pass");
}

fn write_batch_validate_no_override_replace(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let strict = BatchApplyOptions {
        validate_insertion_does_not_override: true,
        ..Default::default()
    };
    let fail_ops = vec![QualifiedGroveDbOp::replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"rp".to_vec()),
    )];
    if db
        .apply_batch(fail_ops, Some(strict.clone()), None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("strict batch replace override should fail");
    }
    let success_ops = vec![QualifiedGroveDbOp::replace_op(
        vec![b"root".to_vec()],
        b"krep2".to_vec(),
        Element::new_item(b"rv".to_vec()),
    )];
    db.apply_batch(success_ops, Some(strict), None, grove_version)
        .unwrap()
        .expect("strict batch replace new key should pass");
}

fn write_batch_validate_no_override_tree_insert(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let strict = BatchApplyOptions {
        validate_insertion_does_not_override_tree: true,
        ..Default::default()
    };
    let tree_override_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![],
        b"root".to_vec(),
        Element::new_item(b"ov".to_vec()),
    )];
    db.apply_batch(tree_override_ops, Some(strict.clone()), None, grove_version)
        .unwrap()
        .expect("strict tree insert override should currently pass");
    let recreate_root_tree_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![],
        b"root".to_vec(),
        Element::empty_tree(),
    )];
    db.apply_batch(
        recreate_root_tree_ops,
        Some(strict.clone()),
        None,
        grove_version,
    )
    .unwrap()
    .expect("recreate root tree after strict tree insert override");
    let item_override_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"it".to_vec()),
    )];
    db.apply_batch(item_override_ops, Some(strict), None, grove_version)
        .unwrap()
        .expect("strict tree item override should pass");
}

fn write_batch_validate_strict_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let strict = BatchApplyOptions {
        validate_insertion_does_not_override: true,
        ..Default::default()
    };
    let tx = db.start_transaction();
    let fail_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"ov".to_vec()),
    )];
    if db
        .apply_batch(fail_ops, Some(strict.clone()), Some(&tx), grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("strict validate-like batch should fail for existing key");
    }
    db.rollback_transaction(&tx)
        .expect("rollback after strict validate-like fail should succeed");

    let tx2 = db.start_transaction();
    let success_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"kvs".to_vec(),
        Element::new_item(b"sv".to_vec()),
    )];
    db.apply_batch(success_ops, Some(strict), Some(&tx2), grove_version)
        .unwrap()
        .expect("strict validate-like batch should pass for new key");
    db.rollback_transaction(&tx2)
        .expect("rollback after strict validate-like success should succeed");
}

fn write_batch_delete_non_empty_tree_error(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree for non-empty delete error case");
    db.insert(
        &[b"root".as_slice(), b"child".as_slice()],
        b"nk",
        Element::new_item(b"nv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nested value for non-empty delete error case");
    let opts = BatchApplyOptions {
        allow_deleting_non_empty_trees: false,
        deleting_non_empty_trees_returns_error: true,
        ..Default::default()
    };
    let ops = vec![QualifiedGroveDbOp::delete_op(
        vec![b"root".to_vec()],
        b"child".to_vec(),
    )];
    db.apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("delete non-empty tree with error flag should currently pass");
}

fn write_batch_delete_non_empty_tree_no_error(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree for non-empty delete no-error case");
    db.insert(
        &[b"root".as_slice(), b"child".as_slice()],
        b"nk",
        Element::new_item(b"nv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nested value for non-empty delete no-error case");
    let opts = BatchApplyOptions {
        allow_deleting_non_empty_trees: false,
        deleting_non_empty_trees_returns_error: false,
        ..Default::default()
    };
    let ops = vec![QualifiedGroveDbOp::delete_op(
        vec![b"root".to_vec()],
        b"child".to_vec(),
    )];
    db.apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("delete non-empty tree should be ignored when error flag is disabled");
}

fn write_batch_disable_consistency_check(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let duplicate_delete_ops = vec![
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"k1".to_vec()),
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"k1".to_vec()),
    ];
    if db
        .apply_batch(
            duplicate_delete_ops.clone(),
            Some(BatchApplyOptions::default()),
            None,
            grove_version,
        )
        .unwrap()
        .is_ok()
    {
        panic!("default batch consistency checks should reject duplicate same-path/key ops");
    }
    let opts = BatchApplyOptions {
        disable_operation_consistency_check: true,
        ..Default::default()
    };
    db.apply_batch(duplicate_delete_ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("disabling consistency checks should allow duplicate deletes");
}

fn write_batch_disable_consistency_last_op_wins(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let mut opts = BatchApplyOptions::default();
    opts.disable_operation_consistency_check = true;
    // Same path+key appears twice. Rust canonicalization keeps the last op.
    let ops = vec![
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"k1".to_vec()),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"k1".to_vec(),
            Element::new_item(b"v2".to_vec()),
        ),
    ];
    db.apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("disable_consistency same-key last-op-wins batch should succeed");
}

fn write_batch_disable_consistency_reorder_parent_child(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let mut opts = BatchApplyOptions::default();
    opts.disable_operation_consistency_check = true;
    // Child write appears before parent tree creation. Rust canonicalized batch
    // execution should still materialize parent first and then apply child op.
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec(), b"ord2".to_vec()],
            b"nk".to_vec(),
            Element::new_item(b"nv".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"ord2".to_vec(),
            Element::empty_tree(),
        ),
    ];
    db.apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("disable_consistency parent/child reorder batch should succeed");
}

fn write_batch_insert_tree_semantics(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"child".to_vec(),
        Element::empty_tree(),
    )];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("batch tree insert should succeed");
    db.insert(
        &[b"root".as_slice(), b"child".as_slice()],
        b"nk",
        Element::new_item(b"nv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("nested insert under batch-created tree should succeed");
}

fn write_batch_insert_tree_replace(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    // First insert a tree at "root" -> "newtree"
    let ops1 = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"newtree".to_vec(),
        Element::empty_tree(),
    )];
    db.apply_batch(ops1, None, None, grove_version)
        .unwrap()
        .expect("initial tree insert should succeed");
    // Replace the tree with a new tree (same as kInsertTree replace behavior)
    let ops2 = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"newtree".to_vec(),
        Element::empty_tree(),
    )];
    db.apply_batch(ops2, None, None, grove_version)
        .unwrap()
        .expect("tree replace should succeed");
    // Verify we can insert under the replaced tree
    db.insert(
        &[b"root".as_slice(), b"newtree".as_slice()],
        b"nested",
        Element::new_item(b"nested_value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("nested insert under replaced tree should succeed");
}

fn write_batch_sum_tree_create_and_sum_item(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec(), b"sbatch".to_vec()],
            b"s1".to_vec(),
            Element::new_sum_item(7),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"sbatch".to_vec(),
            Element::new_sum_tree(None),
        ),
    ];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("same-batch SumTree+SumItem should succeed");
}

fn write_batch_count_tree_create_and_item(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec(), b"cbatch".to_vec()],
            b"c1".to_vec(),
            Element::new_item(b"cv".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"cbatch".to_vec(),
            Element::new_count_tree(None),
        ),
    ];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("same-batch CountTree+Item should succeed");
}

fn write_batch_provable_count_tree_create_and_item(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec(), b"pcbatch".to_vec()],
            b"p1".to_vec(),
            Element::new_item(b"pv".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"pcbatch".to_vec(),
            Element::new_provable_count_tree(None),
        ),
    ];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("same-batch ProvableCountTree+Item should succeed");
}

fn write_batch_count_sum_tree_create_and_sum_item(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec(), b"csbatch".to_vec()],
            b"cs1".to_vec(),
            Element::new_sum_item(11),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"csbatch".to_vec(),
            Element::new_count_sum_tree(None),
        ),
    ];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("same-batch CountSumTree+SumItem should succeed");
}

fn write_batch_provable_count_sum_tree_create_and_sum_item(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec(), b"pcsbatch".to_vec()],
            b"ps1".to_vec(),
            Element::new_sum_item(13),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"pcsbatch".to_vec(),
            Element::new_provable_count_sum_tree(None),
        ),
    ];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("same-batch ProvableCountSumTree+SumItem should succeed");
}

fn write_batch_apply_failure_atomic_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"kok".to_vec(),
            Element::new_item(b"ov".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"missing_parent".to_vec()],
            b"kbad".to_vec(),
            Element::new_item(b"xv".to_vec()),
        ),
    ];
    let result = db.apply_batch(ops, None, None, grove_version).unwrap();
    if result.is_ok() {
        panic!("batch apply should fail for missing parent path");
    }
    let post = db
        .get(&[b"root".as_slice()], b"kok", None, grove_version)
        .unwrap();
    if post.is_ok() {
        panic!("failed batch apply should not persist partial writes");
    }
}

fn write_batch_delete_missing_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![QualifiedGroveDbOp::delete_op(
        vec![b"root".to_vec()],
        b"missing".to_vec(),
    )];
    let result = db.apply_batch(ops, None, None, grove_version).unwrap();
    if result.is_err() {
        panic!("batch delete missing key should succeed as no-op");
    }
    let post = db
        .get(&[b"root".as_slice()], b"k1", None, grove_version)
        .unwrap();
    if post.is_err() {
        panic!("failed delete-missing batch should keep existing keys");
    }
}

fn write_batch_apply_tx_failure_atomic_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"ktxok".to_vec(),
            Element::new_item(b"tv".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"missing_parent".to_vec()],
            b"ktxbad".to_vec(),
            Element::new_item(b"xv".to_vec()),
        ),
    ];
    let result = db.apply_batch(ops, None, Some(&tx), grove_version).unwrap();
    if result.is_ok() {
        panic!("tx batch apply should fail for missing parent path");
    }
    let in_tx = db
        .get(&[b"root".as_slice()], b"ktxok", Some(&tx), grove_version)
        .unwrap();
    if in_tx.is_ok() {
        panic!("failed tx batch apply should not expose partial writes in tx view");
    }
    db.commit_transaction(tx)
        .unwrap()
        .expect("commit after failed tx batch should succeed");
}

fn write_batch_apply_tx_failure_then_reuse(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let fail_ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"ktxok".to_vec(),
            Element::new_item(b"tv".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"missing_parent".to_vec()],
            b"ktxbad".to_vec(),
            Element::new_item(b"xv".to_vec()),
        ),
    ];
    let fail_result = db
        .apply_batch(fail_ops, None, Some(&tx), grove_version)
        .unwrap();
    if fail_result.is_ok() {
        panic!("tx batch apply should fail for missing parent path");
    }

    db.insert(
        &[b"root".as_slice()],
        b"kreuse",
        Element::new_item(b"rv".to_vec()),
        None,
        Some(&tx),
        grove_version,
    )
    .unwrap()
    .expect("insert after failed tx batch should succeed");
    db.commit_transaction(tx)
        .unwrap()
        .expect("commit after tx reuse should succeed");
}

fn write_batch_apply_tx_failure_then_rollback(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let fail_ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"ktxok".to_vec(),
            Element::new_item(b"tv".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"missing_parent".to_vec()],
            b"ktxbad".to_vec(),
            Element::new_item(b"xv".to_vec()),
        ),
    ];
    let fail_result = db
        .apply_batch(fail_ops, None, Some(&tx), grove_version)
        .unwrap();
    if fail_result.is_ok() {
        panic!("tx batch apply should fail for missing parent path");
    }
    db.rollback_transaction(&tx)
        .expect("rollback after failed tx batch should succeed");
}

fn write_batch_apply_tx_success_then_rollback(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"ktsr".to_vec(),
        Element::new_item(b"sv".to_vec()),
    )];
    db.apply_batch(ops, None, Some(&tx), grove_version)
        .unwrap()
        .expect("tx batch apply should succeed");
    db.rollback_transaction(&tx)
        .expect("rollback after successful tx batch should succeed");
}

fn write_batch_apply_tx_delete_then_rollback(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ops = vec![QualifiedGroveDbOp::delete_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
    )];
    db.apply_batch(ops, None, Some(&tx), grove_version)
        .unwrap()
        .expect("tx batch delete should succeed");
    db.rollback_transaction(&tx)
        .expect("rollback after successful tx batch delete should succeed");
}

fn write_batch_apply_tx_delete_missing_noop(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let tx = db.start_transaction();
    let ops = vec![QualifiedGroveDbOp::delete_op(
        vec![b"root".to_vec()],
        b"missing".to_vec(),
    )];
    let result = db.apply_batch(ops, None, Some(&tx), grove_version).unwrap();
    if result.is_err() {
        panic!("tx batch delete missing should succeed as no-op");
    }
    db.commit_transaction(tx)
        .unwrap()
        .expect("commit after tx batch delete-missing should succeed");
}

fn write_batch_delete_tree_op(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree for delete_tree_op case");
    let ops = vec![QualifiedGroveDbOp::delete_tree_op(
        vec![b"root".to_vec()],
        b"child".to_vec(),
        TreeType::NormalTree,
    )];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("delete_tree_op should remove tree key");
}

fn write_batch_delete_tree_disable_consistency_check(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree for delete_tree_disable_consistency_check case");
    let duplicate_delete_tree_ops = vec![
        QualifiedGroveDbOp::delete_tree_op(
            vec![b"root".to_vec()],
            b"child".to_vec(),
            TreeType::NormalTree,
        ),
        QualifiedGroveDbOp::delete_tree_op(
            vec![b"root".to_vec()],
            b"child".to_vec(),
            TreeType::NormalTree,
        ),
    ];
    if db
        .apply_batch(
            duplicate_delete_tree_ops.clone(),
            Some(BatchApplyOptions::default()),
            None,
            grove_version,
        )
        .unwrap()
        .is_ok()
    {
        panic!("default batch consistency checks should reject duplicate delete_tree ops");
    }
    let opts = BatchApplyOptions {
        disable_operation_consistency_check: true,
        ..Default::default()
    };
    db.apply_batch(duplicate_delete_tree_ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("disabling consistency checks should allow duplicate delete_tree ops");
}

fn write_batch_delete_tree_non_empty_options(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree for delete_tree_non_empty_options case");
    db.insert(
        &[b"root".as_slice(), b"child".as_slice()],
        b"nk",
        Element::new_item(b"nv".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert nested value for delete_tree_non_empty_options case");

    let strict_error_opts = BatchApplyOptions {
        allow_deleting_non_empty_trees: false,
        deleting_non_empty_trees_returns_error: true,
        ..Default::default()
    };
    let ops = vec![QualifiedGroveDbOp::delete_tree_op(
        vec![b"root".to_vec()],
        b"child".to_vec(),
        TreeType::NormalTree,
    )];
    let strict_error_result = db
        .apply_batch(ops, Some(strict_error_opts), None, grove_version)
        .unwrap();
    if strict_error_result.is_err() {
        panic!("delete_tree non-empty with error flag should still succeed");
    }
}

fn write_batch_mixed_non_minimal_ops(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree for mixed non-minimal batch case");

    let ops = vec![
        QualifiedGroveDbOp::delete_tree_op(
            vec![b"root".to_vec()],
            b"child".to_vec(),
            TreeType::NormalTree,
        ),
        QualifiedGroveDbOp::replace_op(
            vec![b"root".to_vec()],
            b"k1".to_vec(),
            Element::new_item(b"r2".to_vec()),
        ),
        QualifiedGroveDbOp::insert_only_op(
            vec![b"root".to_vec()],
            b"k2".to_vec(),
            Element::new_item(b"b2".to_vec()),
        ),
    ];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("mixed non-minimal batch apply should succeed");
}

fn write_batch_mixed_non_minimal_ops_with_options(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"child",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert child tree for mixed non-minimal batch+options case");

    let mut opts = BatchApplyOptions::default();
    opts.disable_operation_consistency_check = true;
    let ops = vec![
        QualifiedGroveDbOp::delete_tree_op(
            vec![b"root".to_vec()],
            b"child".to_vec(),
            TreeType::NormalTree,
        ),
        QualifiedGroveDbOp::delete_tree_op(
            vec![b"root".to_vec()],
            b"child".to_vec(),
            TreeType::NormalTree,
        ),
        QualifiedGroveDbOp::replace_op(
            vec![b"root".to_vec()],
            b"k1".to_vec(),
            Element::new_item(b"r3".to_vec()),
        ),
        QualifiedGroveDbOp::insert_only_op(
            vec![b"root".to_vec()],
            b"k3".to_vec(),
            Element::new_item(b"b3".to_vec()),
        ),
    ];
    db.apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("mixed non-minimal batch apply with options should succeed");
}

fn write_batch_patch_existing(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    // Patch existing key k1 (v1 -> p1).  Rust Patch behaves as insert-or-replace.
    let ops = vec![QualifiedGroveDbOp::patch_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"p1".to_vec()),
        0, // change_in_bytes – same size replacement
    )];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("patch existing key should succeed");
}

fn write_batch_patch_missing(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    // Patch missing key k2 – should insert the element.
    let ops = vec![QualifiedGroveDbOp::patch_op(
        vec![b"root".to_vec()],
        b"k2".to_vec(),
        Element::new_item(b"p2".to_vec()),
        0,
    )];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("patch missing key should succeed (inserts)");
}

fn write_batch_patch_strict_no_override(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let strict = BatchApplyOptions {
        validate_insertion_does_not_override: true,
        ..Default::default()
    };
    // Patch existing key k1 with strict override check – should fail.
    let fail_ops = vec![QualifiedGroveDbOp::patch_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"px".to_vec()),
        0,
    )];
    if db
        .apply_batch(fail_ops, Some(strict.clone()), None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("strict patch override on existing key should fail");
    }
    // Patch new key kp with strict override check – should succeed.
    let success_ops = vec![QualifiedGroveDbOp::patch_op(
        vec![b"root".to_vec()],
        b"kp".to_vec(),
        Element::new_item(b"pv".to_vec()),
        0,
    )];
    db.apply_batch(success_ops, Some(strict), None, grove_version)
        .unwrap()
        .expect("strict patch on new key should pass");
}

fn write_batch_refresh_reference_trust(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);
    let ops = vec![QualifiedGroveDbOp::refresh_reference_op(
        vec![b"root".to_vec()],
        b"ref_missing".to_vec(),
        ReferencePathType::AbsolutePathReference(vec![b"root".to_vec(), b"k1".to_vec()]),
        None,
        None,
        true,
    )];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("trusted refresh reference should succeed for missing key");
}

fn write_batch_pause_height_passthrough(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);

    // Apply batch with batch_pause_height=Some(0).
    // This option controls at what tree height to pause batch application.
    // Setting it to 0 means apply all levels (no pausing).
    // This test validates the option is accepted and produces equivalent results.
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"k2".to_vec(),
            Element::new_item(b"v2".to_vec()),
        ),
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"k1".to_vec()),
    ];
    let opts = BatchApplyOptions {
        batch_pause_height: Some(0u8),
        ..Default::default()
    };
    db.apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("batch with pause_height=0");

    // Verify the batch was applied correctly.
    let v2 = db
        .get(&[b"root".as_slice()], b"k2".as_slice(), None, grove_version)
        .unwrap()
        .expect("get k2");
    match v2 {
        Element::Item(v, _) => {
            assert_eq!(v.as_slice(), b"v2");
        }
        _ => panic!("k2 should be an Item element"),
    }

    // k1 should be deleted - get returns Err(Error::NotFound)
    let k1_result = db
        .get(&[b"root".as_slice()], b"k1".as_slice(), None, grove_version)
        .unwrap();
    assert!(
        k1_result.is_err(),
        "k1 should be deleted (get returns error)"
    );
}

fn write_batch_partial_pause_resume(db: &GroveDb, grove_version: &GroveVersion) {
    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[b"root".as_slice()],
        b"a",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert level-1 tree");
    db.insert(
        &[b"root".as_slice(), b"a".as_slice()],
        b"b",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert level-2 tree");

    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"l0".to_vec(),
            Element::new_item(b"v0".to_vec()),
        ),
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"l1".to_vec(),
            Element::new_item(b"v1".to_vec()),
        ),
    ];
    let opts = BatchApplyOptions {
        batch_pause_height: Some(2u8),
        ..Default::default()
    };
    db.apply_partial_batch(
        ops,
        Some(opts),
        |_cost, _left_over_ops| {
            Ok(vec![QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"root".to_vec(), b"a".to_vec(), b"b".to_vec()],
                b"add".to_vec(),
                Element::new_item(b"va".to_vec()),
            )])
        },
        None,
        grove_version,
    )
    .unwrap()
    .expect("apply_partial_batch with pause/resume");
}

fn write_batch_base_root_storage_is_free_passthrough(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);

    // Apply batch with base_root_storage_is_free=false.
    // This option controls whether root storage costs are counted.
    // Default is true (root storage is free).
    // This test validates the option is accepted and produces equivalent results.
    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec()],
            b"k2".to_vec(),
            Element::new_item(b"v2".to_vec()),
        ),
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"k1".to_vec()),
    ];
    let opts = BatchApplyOptions {
        base_root_storage_is_free: false,
        ..Default::default()
    };
    db.apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .expect("batch with base_root_storage_is_free=false");

    // Verify the batch was applied correctly.
    let v2 = db
        .get(&[b"root".as_slice()], b"k2".as_slice(), None, grove_version)
        .unwrap()
        .expect("get k2");
    match v2 {
        Element::Item(v, _) => {
            assert_eq!(v.as_slice(), b"v2");
        }
        _ => panic!("k2 should be an Item element"),
    }

    // k1 should be deleted - get returns Err(Error::NotFound)
    let k1_result = db
        .get(&[b"root".as_slice()], b"k1".as_slice(), None, grove_version)
        .unwrap();
    assert!(
        k1_result.is_err(),
        "k1 should be deleted (get returns error)"
    );
}

fn write_batch_insert_or_replace_semantics(db: &GroveDb, grove_version: &GroveVersion) {
    write_simple(db, grove_version);

    // Test kInsertOrReplace idempotent semantics:
    // - First insert creates the key
    // - Second insert replaces the value
    // - Final value should match the last insert
    let root_path = vec![b"root".to_vec()];

    // Insert initial value
    let ops1 = vec![QualifiedGroveDbOp::insert_or_replace_op(
        root_path.clone(),
        b"kior".to_vec(),
        Element::new_item(b"v1".to_vec()),
    )];
    db.apply_batch(ops1, None, None, grove_version)
        .unwrap()
        .expect("first insert_or_replace should succeed");

    // Verify initial value
    let v1 = db
        .get(
            &[b"root".as_slice()],
            b"kior".as_slice(),
            None,
            grove_version,
        )
        .unwrap()
        .expect("get kior");
    match v1 {
        Element::Item(v, _) => {
            assert_eq!(v.as_slice(), b"v1");
        }
        _ => panic!("kior should be an Item element"),
    }

    // Replace with new value using same operation
    let ops2 = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"kior".to_vec(),
        Element::new_item(b"v2".to_vec()),
    )];
    db.apply_batch(ops2, None, None, grove_version)
        .unwrap()
        .expect("second insert_or_replace should succeed");

    // Verify final value is v2 (idempotent replace behavior)
    let v2 = db
        .get(
            &[b"root".as_slice()],
            b"kior".as_slice(),
            None,
            grove_version,
        )
        .unwrap()
        .expect("get kior after replace");
    match v2 {
        Element::Item(v, _) => {
            assert_eq!(v.as_slice(), b"v2");
        }
        _ => panic!("kior should be an Item element after replace"),
    }
}

/// Write fixture for batch_insert_or_replace_with_override_validation:
/// Tests that validate_insertion_does_not_override flag rejects InsertOrReplace on existing keys
fn write_batch_insert_or_replace_with_override_validation(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    use grovedb::batch::BatchApplyOptions;

    write_simple(db, grove_version);

    // First, insert a key using InsertOrReplace (should succeed, key doesn't exist)
    let ops1 = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"v1".to_vec()),
    )];
    db.apply_batch(ops1, None, None, grove_version)
        .unwrap()
        .expect("first InsertOrReplace should succeed");

    // Verify the key exists
    let v1 = db
        .get(&[b"root".as_slice()], b"k1".as_slice(), None, grove_version)
        .unwrap()
        .expect("get k1");
    match v1 {
        Element::Item(v, _) => {
            assert_eq!(v.as_slice(), b"v1");
        }
        _ => panic!("k1 should be an Item element"),
    }

    // Now try InsertOrReplace with validate_insertion_does_not_override=true
    // This should FAIL because the key already exists
    let mut opts = BatchApplyOptions::default();
    opts.validate_insertion_does_not_override = true;

    let ops2 = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"v2".to_vec()),
    )];
    let result = db.apply_batch(ops2, Some(opts), None, grove_version);
    assert!(
        result.value.is_err(),
        "InsertOrReplace with validate_insertion_does_not_override=true should fail on existing key"
    );

    // Verify the value was not changed (atomic rollback)
    let v_final = db
        .get(&[b"root".as_slice()], b"k1".as_slice(), None, grove_version)
        .unwrap()
        .expect("get k1 after failed batch");
    match v_final {
        Element::Item(v, _) => {
            assert_eq!(
                v.as_slice(),
                b"v1",
                "value should remain v1 after failed batch"
            );
        }
        _ => panic!("k1 should still be an Item element"),
    }
}

/// Write fixture for batch_validate_no_override_tree_insert_or_replace:
/// Tests that validate_insertion_does_not_override_tree flag rejects kInsertOrReplace
/// on existing tree elements while allowing non-tree (Item) element overrides
fn write_batch_validate_no_override_tree_insert_or_replace(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    use grovedb::batch::BatchApplyOptions;

    write_simple(db, grove_version);

    // Test 1: kInsertOrReplace on existing Tree element should FAIL with validate_insertion_does_not_override_tree=true
    let mut opts_tree = BatchApplyOptions::default();
    opts_tree.validate_insertion_does_not_override_tree = true;

    let tree_override_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![],
        b"root".to_vec(),
        Element::new_item(b"ov".to_vec()),
    )];
    let result = db.apply_batch(tree_override_ops, Some(opts_tree.clone()), None, grove_version);
    assert!(
        result.value.is_err(),
        "kInsertOrReplace on existing Tree should fail with validate_insertion_does_not_override_tree=true"
    );

    // Verify root is still a tree (not replaced with item)
    let root_elem = db
        .get(&[] as &[&[u8]], b"root".as_slice(), None, grove_version)
        .unwrap()
        .expect("get root after failed batch");
    match root_elem {
        Element::Tree(_, _) => {
            // Good - root is still a tree
        }
        _ => panic!("root should still be a Tree after failed batch"),
    }

    // Test 2: kInsertOrReplace on existing Item element should SUCCEED with validate_insertion_does_not_override_tree=true
    // (because it's not overriding a tree element)
    let item_override_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"k1".to_vec(),
        Element::new_item(b"it".to_vec()),
    )];
    db.apply_batch(item_override_ops, Some(opts_tree), None, grove_version)
        .unwrap()
        .expect("kInsertOrReplace on existing Item should succeed with validate_insertion_does_not_override_tree=true");

    // Verify k1 was replaced with new item value
    let k1_elem = db
        .get(&[b"root".as_slice()], b"k1".as_slice(), None, grove_version)
        .unwrap()
        .expect("get k1 after item override");
    match k1_elem {
        Element::Item(v, _) => {
            assert_eq!(v.as_slice(), b"it", "k1 should be replaced with 'it'");
        }
        _ => panic!("k1 should be an Item element after replace"),
    }
}

fn write_batch_insert_tree_below_deleted_path_consistency(
    db: &GroveDb,
    grove_version: &GroveVersion,
) {
    write_simple(db, grove_version);
    db.insert(
        &[b"root".as_slice()],
        b"ct",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ct tree for insert-tree-under-delete consistency mode");

    let ops = vec![
        QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"root".to_vec(), b"ct".to_vec()],
            b"gc".to_vec(),
            Element::empty_tree(),
        ),
        QualifiedGroveDbOp::delete_op(vec![b"root".to_vec()], b"ct".to_vec()),
    ];
    if db
        .apply_batch(
            ops.clone(),
            Some(BatchApplyOptions::default()),
            None,
            grove_version,
        )
        .unwrap()
        .is_ok()
    {
        panic!("default consistency checks should reject insert under deleted path");
    }

    let opts = BatchApplyOptions {
        disable_operation_consistency_check: true,
        ..Default::default()
    };
    if db
        .apply_batch(ops, Some(opts), None, grove_version)
        .unwrap()
        .is_ok()
    {
        panic!("insert under deleted path should still fail at execution time");
    }
}

fn write_batch_insert_tree_with_root_hash(db: &GroveDb, grove_version: &GroveVersion) {
    // Test kInsertTree (InsertTreeWithRootHash) batch operation:
    // - Insert a tree element via batch operation using insert_or_replace
    // - This mirrors C++ kInsertTree batch operation semantics
    write_simple(db, grove_version);

    // Insert tree using batch operation (mirrors C++ kInsertTree)
    let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
        vec![b"root".to_vec()],
        b"tree_key".to_vec(),
        Element::empty_tree(),
    )];
    db.apply_batch(ops, None, None, grove_version)
        .unwrap()
        .expect("insert_tree batch should succeed");
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let mode = env::args().nth(2).unwrap_or_else(|| "simple".to_string());
    let db = GroveDb::open(Path::new(&path)).expect("open grovedb");
    let grove_version = GroveVersion::latest();

    match mode.as_str() {
        "simple" => write_simple(&db, &grove_version),
        "facade_insert_helpers" => write_facade_insert_helpers(&db, &grove_version),
        "facade_insert_if_not_exists" => write_facade_insert_if_not_exists(&db, &grove_version),
        "facade_insert_if_changed_value" => {
            write_facade_insert_if_changed_value(&db, &grove_version)
        }
        "facade_insert_if_not_exists_return_existing" => {
            write_facade_insert_if_not_exists_return_existing(&db, &grove_version)
        }
        "facade_insert_if_not_exists_return_existing_tx" => {
            write_facade_insert_if_not_exists_return_existing_tx(&db, &grove_version)
        }
        "facade_flush" => write_facade_flush(&db, &grove_version),
        "facade_root_key" => write_facade_root_key(&db, &grove_version),
        "facade_delete_if_empty_tree" => write_facade_delete_if_empty_tree(&db, &grove_version),
        "facade_clear_subtree" => write_facade_clear_subtree(&db, &grove_version),
        "facade_clear_subtree_tx" => write_facade_clear_subtree_tx(&db, &grove_version),
        "facade_follow_reference" => write_facade_follow_reference(&db, &grove_version),
        "facade_follow_reference_tx" => write_facade_follow_reference_tx(&db, &grove_version),
        "facade_find_subtrees" => write_facade_find_subtrees(&db, &grove_version),
        "facade_check_subtree_exists_invalid_path_tx" => {
            write_facade_check_subtree_exists_invalid_path_tx(&db, &grove_version)
        }
        "facade_follow_reference_mixed_path_chain" => {
            write_facade_follow_reference_mixed_path_chain(&db, &grove_version)
        }
        "facade_follow_reference_parent_path_addition" => {
            write_facade_follow_reference_parent_path_addition(&db, &grove_version)
        }
        "facade_follow_reference_cousin" => {
            write_facade_follow_reference_cousin(&db, &grove_version)
        }
        "facade_follow_reference_removed_cousin" => {
            write_facade_follow_reference_removed_cousin(&db, &grove_version)
        }
        "facade_follow_reference_upstream_element_height" => {
            write_facade_follow_reference_upstream_element_height(&db, &grove_version)
        }
        "facade_get_raw" => write_facade_get_raw(&db, &grove_version),
        "facade_get_raw_optional" => write_facade_get_raw_optional(&db, &grove_version),
        "facade_get_raw_caching_optional" => {
            write_facade_get_raw_caching_optional(&db, &grove_version)
        }
        "facade_get_caching_optional" => {
            write_facade_get_raw_caching_optional(&db, &grove_version)
        }
        "facade_get_caching_optional_tx" => {
            write_facade_get_caching_optional_tx(&db, &grove_version)
        }
        "facade_get_subtree_root_tx" => {
            write_facade_get_subtree_root_tx(&db, &grove_version)
        }
        "facade_has_caching_optional_tx" => {
            write_facade_has_caching_optional_tx(&db, &grove_version)
        }
        "facade_query_raw" => write_facade_query_raw(&db, &grove_version),
        "facade_query_item_value" => write_facade_query_item_value(&db, &grove_version),
        "facade_query_item_value_tx" => write_facade_query_item_value_tx(&db, &grove_version),
        "facade_query_item_value_or_sum_tx" => {
            write_facade_query_item_value_or_sum_tx(&db, &grove_version)
        }
        "facade_query_raw_tx" => write_facade_query_raw_tx(&db, &grove_version),
        "facade_query_key_element_pairs_tx" => {
            write_facade_query_key_element_pairs_tx(&db, &grove_version)
        }
        "facade_query_raw_keys_optional" => {
            write_facade_query_raw_keys_optional(&db, &grove_version)
        }
        "facade_query_keys_optional" => write_facade_query_keys_optional(&db, &grove_version),
        "facade_query_raw_keys_optional_tx" => {
            write_facade_query_raw_keys_optional_tx(&db, &grove_version)
        }
        "facade_query_keys_optional_tx" => write_facade_query_keys_optional_tx(&db, &grove_version),
        "facade_query_sums_tx" => write_facade_query_sums_tx(&db, &grove_version),
        "nested" => write_nested(&db, &grove_version),
        "tx_commit" => write_tx_commit(&db, &grove_version),
        "tx_rollback" => write_tx_rollback(&db, &grove_version),
        "tx_visibility_rollback" => write_tx_visibility_rollback(&db, &grove_version),
        "tx_mixed_commit" => write_tx_mixed_commit(&db, &grove_version),
        "tx_rollback_range" => write_tx_rollback_range(&db, &grove_version),
        "tx_drop_abort" => write_tx_drop_abort(&db, &grove_version),
        "tx_commit_after_rollback_rejected" => {
            write_tx_commit_after_rollback_rejected(&db, &grove_version)
        }
        "tx_write_after_rollback_rejected" => {
            write_tx_write_after_rollback_rejected(&db, &grove_version)
        }
        "tx_multi_rollback_reuse" => write_tx_multi_rollback_reuse(&db, &grove_version),
        "tx_delete_after_rollback" => write_tx_delete_after_rollback(&db, &grove_version),
        "tx_reopen_visibility_after_rollback" => {
            write_tx_reopen_visibility_after_rollback(&db, &grove_version)
        }
        "tx_reopen_conflict_same_path" => write_tx_reopen_conflict_same_path(&db, &grove_version),
        "tx_delete_visibility" => write_tx_delete_visibility(&db, &grove_version),
        "tx_delete_then_reinsert_same_key" => {
            write_tx_delete_then_reinsert_same_key(&db, &grove_version)
        }
        "tx_insert_then_delete_same_key" => {
            write_tx_insert_then_delete_same_key(&db, &grove_version)
        }
        "tx_delete_missing_noop" => write_tx_delete_missing_noop(&db, &grove_version),
        "tx_read_committed_visibility" => write_tx_read_committed_visibility(&db, &grove_version),
        "tx_has_read_committed_visibility" => {
            write_tx_has_read_committed_visibility(&db, &grove_version)
        }
        "tx_query_range_committed_visibility" => {
            write_tx_query_range_committed_visibility(&db, &grove_version)
        }
        "tx_iterator_stability_under_commit" => {
            write_tx_iterator_stability_under_commit(&db, &grove_version)
        }
        "tx_same_key_conflict_reverse_order" => {
            write_tx_same_key_conflict_reverse_order(&db, &grove_version)
        }
        "tx_shared_subtree_disjoint_conflict_reverse_order" => {
            write_tx_shared_subtree_disjoint_conflict_reverse_order(&db, &grove_version)
        }
        "tx_disjoint_subtree_conflict_reverse_order" => {
            write_tx_disjoint_subtree_conflict_reverse_order(&db, &grove_version)
        }
        "tx_disjoint_subtree_conflict_forward_order" => {
            write_tx_disjoint_subtree_conflict_forward_order(&db, &grove_version)
        }
        "tx_read_only_then_writer_commit" => {
            write_tx_read_only_then_writer_commit(&db, &grove_version)
        }
        "tx_delete_insert_same_key_forward" => {
            write_tx_delete_insert_same_key_forward(&db, &grove_version)
        }
        "tx_delete_insert_same_key_reverse" => {
            write_tx_delete_insert_same_key_reverse(&db, &grove_version)
        }
        "tx_delete_insert_same_subtree_disjoint_forward" => {
            write_tx_delete_insert_same_subtree_disjoint_forward(&db, &grove_version)
        }
        "tx_delete_insert_same_subtree_disjoint_reverse" => {
            write_tx_delete_insert_same_subtree_disjoint_reverse(&db, &grove_version)
        }
        "tx_delete_insert_disjoint_subtree_forward" => {
            write_tx_delete_insert_disjoint_subtree_forward(&db, &grove_version)
        }
        "tx_delete_insert_disjoint_subtree_reverse" => {
            write_tx_delete_insert_disjoint_subtree_reverse(&db, &grove_version)
        }
        "tx_replace_delete_same_key_forward" => {
            write_tx_replace_delete_same_key_forward(&db, &grove_version)
        }
        "tx_replace_delete_same_key_reverse" => {
            write_tx_replace_delete_same_key_reverse(&db, &grove_version)
        }
        "tx_replace_replace_same_key_forward" => {
            write_tx_replace_replace_same_key_forward(&db, &grove_version)
        }
        "tx_replace_replace_same_key_reverse" => {
            write_tx_replace_replace_same_key_reverse(&db, &grove_version)
        }
        "tx_double_rollback_noop" => write_tx_double_rollback_noop(&db, &grove_version),
        "tx_conflict_sequence_persistence" => {
            write_tx_conflict_sequence_persistence(&db, &grove_version)
        }
        "tx_checkpoint_snapshot_isolation" => {
            write_tx_checkpoint_snapshot_isolation(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_independent_writes" => {
            write_tx_checkpoint_independent_writes(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_delete_safety" => {
            write_tx_checkpoint_delete_safety(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_open_safety" => {
            write_tx_checkpoint_open_safety(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_delete_short_path_safety" => {
            write_tx_checkpoint_delete_short_path_safety(&db, &grove_version)
        }
        "tx_checkpoint_open_missing_path" => {
            write_tx_checkpoint_open_missing_path(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_reopen_mutate_recheckpoint" => {
            write_tx_checkpoint_reopen_mutate_recheckpoint(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_reopen_mutate_chain" => {
            write_tx_checkpoint_reopen_mutate_chain(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_batch_ops" => {
            write_tx_checkpoint_batch_ops(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_delete_reopen_sequence" => {
            write_tx_checkpoint_delete_reopen_sequence(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_aux_isolation" => {
            write_tx_checkpoint_aux_isolation(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_tx_operations" => {
            write_tx_checkpoint_tx_operations(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_chain_mutation_isolation" => {
            write_tx_checkpoint_chain_mutation_isolation(&db, Path::new(&path), &grove_version)
        }
        "tx_checkpoint_reopen_after_main_delete" => {
            write_tx_checkpoint_reopen_after_main_delete(&db, Path::new(&path), &grove_version)
        }
        "batch_apply_local_atomic" => write_batch_apply_local_atomic(&db, &grove_version),
        "batch_apply_tx_visibility" => write_batch_apply_tx_visibility(&db, &grove_version),
        "batch_apply_empty_noop" => write_batch_apply_empty_noop(&db, &grove_version),
        "batch_validate_success_noop" => write_batch_validate_success_noop(&db, &grove_version),
        "batch_validate_failure_noop" => write_batch_validate_failure_noop(&db, &grove_version),
        "batch_insert_only_semantics" => write_batch_insert_only_semantics(&db, &grove_version),
        "batch_replace_semantics" => write_batch_replace_semantics(&db, &grove_version),
        "batch_validate_no_override_insert" => {
            write_batch_validate_no_override_insert(&db, &grove_version)
        }
        "batch_validate_no_override_insert_only" => {
            write_batch_validate_no_override_insert_only(&db, &grove_version)
        }
        "batch_validate_no_override_replace" => {
            write_batch_validate_no_override_replace(&db, &grove_version)
        }
        "batch_validate_no_override_tree_insert" => {
            write_batch_validate_no_override_tree_insert(&db, &grove_version)
        }
        "batch_validate_strict_noop" => write_batch_validate_strict_noop(&db, &grove_version),
        "batch_delete_non_empty_tree_error" => {
            write_batch_delete_non_empty_tree_error(&db, &grove_version)
        }
        "batch_delete_non_empty_tree_no_error" => {
            write_batch_delete_non_empty_tree_no_error(&db, &grove_version)
        }
        "batch_disable_consistency_check" => {
            write_batch_disable_consistency_check(&db, &grove_version)
        }
        "batch_disable_consistency_last_op_wins" => {
            write_batch_disable_consistency_last_op_wins(&db, &grove_version)
        }
        "batch_disable_consistency_reorder_parent_child" => {
            write_batch_disable_consistency_reorder_parent_child(&db, &grove_version)
        }
        "batch_insert_tree_semantics" => write_batch_insert_tree_semantics(&db, &grove_version),
        "batch_insert_tree_replace" => write_batch_insert_tree_replace(&db, &grove_version),
        "batch_sum_tree_create_and_sum_item" => {
            write_batch_sum_tree_create_and_sum_item(&db, &grove_version)
        }
        "batch_count_tree_create_and_item" => {
            write_batch_count_tree_create_and_item(&db, &grove_version)
        }
        "batch_provable_count_tree_create_and_item" => {
            write_batch_provable_count_tree_create_and_item(&db, &grove_version)
        }
        "batch_count_sum_tree_create_and_sum_item" => {
            write_batch_count_sum_tree_create_and_sum_item(&db, &grove_version)
        }
        "batch_provable_count_sum_tree_create_and_sum_item" => {
            write_batch_provable_count_sum_tree_create_and_sum_item(&db, &grove_version)
        }
        "batch_apply_failure_atomic_noop" => {
            write_batch_apply_failure_atomic_noop(&db, &grove_version)
        }
        "batch_delete_missing_noop" => write_batch_delete_missing_noop(&db, &grove_version),
        "batch_apply_tx_failure_atomic_noop" => {
            write_batch_apply_tx_failure_atomic_noop(&db, &grove_version)
        }
        "batch_apply_tx_failure_then_reuse" => {
            write_batch_apply_tx_failure_then_reuse(&db, &grove_version)
        }
        "batch_apply_tx_failure_then_rollback" => {
            write_batch_apply_tx_failure_then_rollback(&db, &grove_version)
        }
        "batch_apply_tx_success_then_rollback" => {
            write_batch_apply_tx_success_then_rollback(&db, &grove_version)
        }
        "batch_apply_tx_delete_then_rollback" => {
            write_batch_apply_tx_delete_then_rollback(&db, &grove_version)
        }
        "batch_apply_tx_delete_missing_noop" => {
            write_batch_apply_tx_delete_missing_noop(&db, &grove_version)
        }
        "batch_delete_tree_op" => write_batch_delete_tree_op(&db, &grove_version),
        "batch_delete_tree_disable_consistency_check" => {
            write_batch_delete_tree_disable_consistency_check(&db, &grove_version)
        }
        "batch_delete_tree_non_empty_options" => {
            write_batch_delete_tree_non_empty_options(&db, &grove_version)
        }
        "batch_mixed_non_minimal_ops" => write_batch_mixed_non_minimal_ops(&db, &grove_version),
        "batch_mixed_non_minimal_ops_with_options" => {
            write_batch_mixed_non_minimal_ops_with_options(&db, &grove_version)
        }
        "batch_patch_existing" => write_batch_patch_existing(&db, &grove_version),
        "batch_patch_missing" => write_batch_patch_missing(&db, &grove_version),
        "batch_patch_strict_no_override" => {
            write_batch_patch_strict_no_override(&db, &grove_version)
        }
        "batch_refresh_reference_trust" => write_batch_refresh_reference_trust(&db, &grove_version),
        "batch_pause_height_passthrough" => {
            write_batch_pause_height_passthrough(&db, &grove_version)
        }
        "batch_partial_pause_resume" => write_batch_partial_pause_resume(&db, &grove_version),
        "batch_base_root_storage_is_free_passthrough" => {
            write_batch_base_root_storage_is_free_passthrough(&db, &grove_version)
        }
        "batch_insert_or_replace_semantics" => {
            write_batch_insert_or_replace_semantics(&db, &grove_version)
        }
        "batch_insert_or_replace_with_override_validation" => {
            write_batch_insert_or_replace_with_override_validation(&db, &grove_version)
        }
        "batch_validate_no_override_tree_insert_or_replace" => {
            write_batch_validate_no_override_tree_insert_or_replace(&db, &grove_version)
        }
        "batch_insert_tree_below_deleted_path_consistency" => {
            write_batch_insert_tree_below_deleted_path_consistency(&db, &grove_version)
        }
        "batch_insert_tree_with_root_hash" => {
            write_batch_insert_tree_with_root_hash(&db, &grove_version)
        }
        _ => panic!("unsupported mode"),
    }
}
