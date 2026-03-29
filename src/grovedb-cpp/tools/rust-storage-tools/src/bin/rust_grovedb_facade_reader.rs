use std::env;
use std::path::Path;

use grovedb::query_result_type::QueryResultType::QueryKeyElementPairResultType;
use grovedb::{Element, GroveDb, PathQuery, SizedQuery};
use grovedb_merk::proofs::Query;
use grovedb_version::version::GroveVersion;

fn expect_item(db: &GroveDb, path: &[&[u8]], key: &[u8], expected: &[u8], gv: &GroveVersion) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::Item(v, _) => {
            if v.as_slice() != expected {
                panic!("item value mismatch");
            }
        }
        _ => panic!("expected item element"),
    }
}

fn expect_missing(db: &GroveDb, path: &[&[u8]], key: &[u8], gv: &GroveVersion) {
    if db.get(path, key, None, gv).unwrap().is_ok() {
        panic!("expected missing element");
    }
}

fn expect_tree(db: &GroveDb, path: &[&[u8]], key: &[u8], gv: &GroveVersion) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::Tree(..) => {}
        _ => panic!("expected tree element"),
    }
}

fn expect_sum_tree_sum(db: &GroveDb, path: &[&[u8]], key: &[u8], expected_sum: i64, gv: &GroveVersion) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::SumTree(_, sum, _) => {
            if sum != expected_sum {
                panic!("expected sum tree with sum={expected_sum}, got {sum}");
            }
        }
        _ => panic!("expected sum tree element"),
    }
}

fn expect_sum_item(db: &GroveDb, path: &[&[u8]], key: &[u8], expected_sum: i64, gv: &GroveVersion) {
    let elem = db
        .get_raw(path.into(), key, None, gv)
        .unwrap()
        .expect("expected raw element");
    match elem {
        Element::SumItem(sum, _) => {
            if sum != expected_sum {
                panic!("expected sum item with sum={expected_sum}, got {sum}");
            }
        }
        _ => panic!("expected sum item element"),
    }
}

fn expect_count_tree_count(
    db: &GroveDb,
    path: &[&[u8]],
    key: &[u8],
    expected_count: u64,
    gv: &GroveVersion,
) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::CountTree(_, count, _) => {
            if count != expected_count {
                panic!("expected count tree with count={expected_count}, got {count}");
            }
        }
        _ => panic!("expected count tree element"),
    }
}

fn expect_count_sum_tree(
    db: &GroveDb,
    path: &[&[u8]],
    key: &[u8],
    expected_count: u64,
    expected_sum: i64,
    gv: &GroveVersion,
) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::CountSumTree(_, count, sum, _) => {
            if count != expected_count || sum != expected_sum {
                panic!(
                    "expected count sum tree (count={}, sum={}), got (count={}, sum={})",
                    expected_count, expected_sum, count, sum
                );
            }
        }
        _ => panic!("expected count sum tree element"),
    }
}

fn expect_provable_count_sum_tree(
    db: &GroveDb,
    path: &[&[u8]],
    key: &[u8],
    expected_count: u64,
    expected_sum: i64,
    gv: &GroveVersion,
) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::ProvableCountSumTree(_, count, sum, _) => {
            if count != expected_count || sum != expected_sum {
                panic!(
                    "expected provable count sum tree (count={}, sum={}), got (count={}, sum={})",
                    expected_count, expected_sum, count, sum
                );
            }
        }
        _ => panic!("expected provable count sum tree element"),
    }
}

fn expect_big_sum_tree(db: &GroveDb, path: &[&[u8]], key: &[u8], gv: &GroveVersion) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::BigSumTree(_, sum, _) => {
            if sum != 0 {
                panic!("expected big sum tree with sum=0");
            }
        }
        _ => panic!("expected big sum tree element"),
    }
}

fn expect_count_tree(db: &GroveDb, path: &[&[u8]], key: &[u8], gv: &GroveVersion) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::CountTree(_, count, _) => {
            if count != 0 {
                panic!("expected count tree with count=0");
            }
        }
        _ => panic!("expected count tree element"),
    }
}

fn expect_provable_count_tree(db: &GroveDb, path: &[&[u8]], key: &[u8], gv: &GroveVersion) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::ProvableCountTree(_, count, _) => {
            if count != 0 {
                panic!("expected provable count tree with count=0");
            }
        }
        _ => panic!("expected provable count tree element"),
    }
}

fn expect_provable_count_tree_count(
    db: &GroveDb,
    path: &[&[u8]],
    key: &[u8],
    expected_count: u64,
    gv: &GroveVersion,
) {
    let elem = db
        .get(path, key, None, gv)
        .unwrap()
        .expect("expected element");
    match elem {
        Element::ProvableCountTree(_, count, _) => {
            if count != expected_count {
                panic!("expected provable count tree with count={expected_count}, got {count}");
            }
        }
        _ => panic!("expected provable count tree element"),
    }
}

fn expect_aux_value(db: &GroveDb, key: &[u8], expected: &[u8]) {
    let value = db
        .get_aux(key, None)
        .unwrap()
        .expect("expected aux get to succeed");
    if value != Some(expected.to_vec()) {
        panic!("aux value mismatch");
    }
}

fn expect_aux_missing(db: &GroveDb, key: &[u8]) {
    let value = db
        .get_aux(key, None)
        .unwrap()
        .expect("expected aux get to succeed");
    if value.is_some() {
        panic!("expected missing aux value");
    }
}

fn main() {
    let path = env::args().nth(1).expect("db path required");
    let mode = env::args().nth(2).unwrap_or_else(|| "simple".to_string());
    let db = GroveDb::open(Path::new(&path)).expect("open grovedb");
    let gv = GroveVersion::latest();

    match mode.as_str() {
        "simple" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
        }
        "facade_insert_helpers" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_tree(&db, &[b"root"], b"child", &gv);
            expect_item(&db, &[b"root", b"child"], b"nk", b"nv", &gv);
            expect_big_sum_tree(&db, &[b"root"], b"big", &gv);
            expect_count_tree(&db, &[b"root"], b"count", &gv);
            expect_provable_count_tree(&db, &[b"root"], b"provct", &gv);
        }
        "facade_insert_if_not_exists" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"k2", b"v2", &gv);
        }
        "facade_insert_if_changed_value" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v2", &gv);
        }
        "facade_insert_if_not_exists_return_existing" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
        }
        "facade_insert_if_not_exists_return_existing_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"txk", b"tv", &gv);
        }
        "facade_flush" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
        }
        "facade_root_key" => {
            expect_tree(&db, &[], b"root", &gv);
        }
        "facade_delete_if_empty_tree" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"deletable", &gv);
            expect_tree(&db, &[b"root"], b"nonempty", &gv);
            expect_item(&db, &[b"root", b"nonempty"], b"k1", b"v1", &gv);
        }
        "facade_clear_subtree" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"clr", &gv);
            expect_missing(&db, &[b"root", b"clr"], b"k1", &gv);
            expect_missing(&db, &[b"root", b"clr"], b"k2", &gv);
        }
        "facade_clear_subtree_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"clr", &gv);
            expect_missing(&db, &[b"root", b"clr"], b"k1", &gv);
            expect_missing(&db, &[b"root", b"clr"], b"k2", &gv);
        }
        "facade_follow_reference" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let followed = db
                .follow_reference(
                    (&[b"root".as_slice(), b"ref1".as_slice()]).into(),
                    true,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("follow_reference should resolve");
            match followed {
                Element::Item(v, _) if v.as_slice() == b"tv" => {}
                _ => panic!("follow_reference should resolve to target item"),
            }
        }
        "facade_follow_reference_tx" => {
            // Verify root tree and base structures
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            expect_item(&db, &[b"root"], b"ref", b"tv", &gv); // ref resolves to target

            // Follow tx_ref - should resolve to target item (tx is committed by now)
            let followed = db
                .follow_reference(
                    (&[b"root".as_slice(), b"tx_ref".as_slice()]).into(),
                    true,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("follow_reference tx should resolve after commit");
            match followed {
                Element::Item(v, _) if v.as_slice() == b"tv" => {}
                _ => panic!("follow_reference tx_ref should resolve to target item tv"),
            }
        }
        "facade_find_subtrees" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"child1", &gv);
            expect_tree(&db, &[b"root"], b"child2", &gv);
            expect_tree(&db, &[b"root", b"child1"], b"grandchild1", &gv);
            expect_tree(&db, &[b"root", b"child1"], b"grandchild2", &gv);
            expect_item(&db, &[b"root", b"child1", b"grandchild1"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root", b"child2"], b"k2", b"v2", &gv);
        }
        "facade_check_subtree_exists_invalid_path_tx" => {
            // Verify root tree and base structures written by writer
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"base_v", &gv);
            expect_tree(&db, &[b"root"], b"child", &gv);
        }
        "facade_follow_reference_mixed_path_chain" => {
            // Verify nested structure: root/inner
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"inner", &gv);
            // Verify target item
            expect_item(&db, &[b"root"], b"target", b"mixed_target_value", &gv);
            // Follow reference chain: ref_c (root/inner) -> ref_b (root) -> ref_a (root) -> target (root)
            // UpstreamRootHeightReference(1, [ref_b]) from path [root, inner]:
            //   - len = 2, keep = 1, n_to_remove = 1
            //   - After removing 1: [root]
            //   - Append [ref_b]: [root, ref_b]
            let followed = db
                .follow_reference(
                    (&[b"root".as_slice(), b"inner".as_slice(), b"ref_c".as_slice()]).into(),
                    true,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("follow_reference mixed path chain should resolve");
            match followed {
                Element::Item(v, _) if v.as_slice() == b"mixed_target_value" => {}
                _ => panic!("follow_reference mixed path chain should resolve to target item"),
            }
        }
        "facade_follow_reference_parent_path_addition" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"alias", &gv);
            expect_tree(&db, &[b"root"], b"branch", &gv);
            expect_tree(&db, &[b"root", b"branch"], b"target", &gv);
            expect_item(
                &db,
                &[b"root", b"alias"],
                b"target",
                b"parent_add_target_value",
                &gv,
            );
            let followed = db
                .follow_reference(
                    (&[
                        b"root".as_slice(),
                        b"branch".as_slice(),
                        b"target".as_slice(),
                        b"ref_parent_add".as_slice(),
                    ])
                        .into(),
                    true,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("follow_reference parent path addition should resolve");
            match followed {
                Element::Item(v, _) if v.as_slice() == b"parent_add_target_value" => {}
                _ => panic!("follow_reference parent path addition should resolve to target item"),
            }
        }
        "facade_follow_reference_cousin" => {
            // Verify structure: root/branch/deep and root/branch/cousin
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"branch", &gv);
            expect_tree(&db, &[b"root", b"branch"], b"deep", &gv);
            // CousinReference at root/branch/deep/ref should resolve to root/branch/cousin/ref
            // which contains "cousin_target_value"
            let followed = db
                .follow_reference(
                    (&[
                        b"root".as_slice(),
                        b"branch".as_slice(),
                        b"deep".as_slice(),
                        b"ref".as_slice(),
                    ])
                        .into(),
                    true,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("follow_reference cousin should resolve");
            match followed {
                Element::Item(v, _) if v.as_slice() == b"cousin_target_value" => {}
                _ => panic!("follow_reference cousin should resolve to cousin_target_value"),
            }
        }
        "facade_follow_reference_removed_cousin" => {
            // Verify structure: root/branch/deep and root/branch/cousin/nested
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"branch", &gv);
            expect_tree(&db, &[b"root", b"branch"], b"deep", &gv);
            expect_tree(&db, &[b"root", b"branch"], b"cousin", &gv);
            expect_tree(&db, &[b"root", b"branch", b"cousin"], b"nested", &gv);
            // RemovedCousinReference at root/branch/deep/ref should resolve to root/branch/cousin/nested/ref
            // which contains "removed_cousin_target_value"
            let followed = db
                .follow_reference(
                    (&[
                        b"root".as_slice(),
                        b"branch".as_slice(),
                        b"deep".as_slice(),
                        b"ref".as_slice(),
                    ])
                        .into(),
                    true,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("follow_reference removed_cousin should resolve");
            match followed {
                Element::Item(v, _) if v.as_slice() == b"removed_cousin_target_value" => {}
                _ => panic!("follow_reference removed_cousin should resolve to removed_cousin_target_value"),
            }
        }
        "facade_follow_reference_upstream_element_height" => {
            // Verify structure: root/branch/deep
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"branch", &gv);
            expect_tree(&db, &[b"root", b"branch"], b"deep", &gv);
            // UpstreamFromElementHeightReference at root/branch/deep/ref should resolve to root/branch/alias
            // which contains "upstream_elem_value"
            expect_item(&db, &[b"root", b"branch"], b"alias", b"upstream_elem_value", &gv);
            let followed = db
                .follow_reference(
                    (&[
                        b"root".as_slice(),
                        b"branch".as_slice(),
                        b"deep".as_slice(),
                        b"ref".as_slice(),
                    ])
                        .into(),
                    true,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("follow_reference upstream_element_height should resolve");
            match followed {
                Element::Item(v, _) if v.as_slice() == b"upstream_elem_value" => {}
                _ => panic!("follow_reference upstream_element_height should resolve to upstream_elem_value"),
            }
        }
        "facade_get_raw" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let raw = db
                .get_raw((&[b"root".as_slice()]).into(), b"ref1", None, &gv)
                .unwrap()
                .expect("get_raw should return stored element");
            match raw {
                Element::Reference(..) => {}
                _ => panic!("get_raw should return unresolved reference element"),
            }
        }
        "facade_get_raw_optional" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let raw = db
                .get_raw_optional((&[b"root".as_slice()]).into(), b"ref1", None, &gv)
                .unwrap()
                .expect("get_raw_optional existing path should succeed");
            match raw {
                Some(Element::Reference(..)) => {}
                _ => panic!("get_raw_optional should return unresolved reference element"),
            }
            let missing = db
                .get_raw_optional(
                    (&[b"root".as_slice(), b"miss".as_slice()]).into(),
                    b"k",
                    None,
                    &gv,
                )
                .unwrap()
                .expect("get_raw_optional missing path should succeed");
            if missing.is_some() {
                panic!("get_raw_optional should return None for missing path");
            }
        }
        "facade_get_raw_caching_optional" => {
            // Rust API does not expose a direct get_raw_caching_optional helper; verify final state shape.
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let raw = db
                .get_raw_optional((&[b"root".as_slice()]).into(), b"ref", None, &gv)
                .unwrap()
                .expect("get_raw_optional existing path should succeed");
            match raw {
                Some(Element::Reference(..)) => {}
                _ => panic!("facade_get_raw_caching_optional should store unresolved reference at ref"),
            }
            let missing = db
                .get_raw_optional(
                    (&[b"root".as_slice(), b"miss".as_slice()]).into(),
                    b"k",
                    None,
                    &gv,
                )
                .unwrap()
                .expect("get_raw_optional missing path should succeed");
            if missing.is_some() {
                panic!("facade_get_raw_caching_optional missing path should return None");
            }
        }
        "facade_get_caching_optional" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let resolved = db
                .get_caching_optional((&[b"root".as_slice()]).into(), b"ref", true, None, &gv)
                .unwrap()
                .expect("get_caching_optional existing path should succeed");
            match resolved {
                Element::Item(v, _) if v.as_slice() == b"tv" => {}
                _ => panic!("facade_get_caching_optional should resolve ref to target item"),
            }
            let resolved_no_cache = db
                .get_caching_optional((&[b"root".as_slice()]).into(), b"ref", false, None, &gv)
                .unwrap()
                .expect("get_caching_optional cache-bypass should succeed");
            match resolved_no_cache {
                Element::Item(v, _) if v.as_slice() == b"tv" => {}
                _ => panic!("facade_get_caching_optional cache-bypass should resolve ref to target item"),
            }
        }
        "facade_get_caching_optional_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"base_v", &gv);
            expect_item(&db, &[b"root"], b"txk", b"tx_v", &gv);

            // Verify txk is visible without tx (after commit)
            let element_bytes = db
                .get_caching_optional((&[b"root".as_slice()]).into(), b"txk", true, None, &gv)
                .unwrap()
                .expect("get_caching_optional after commit should succeed");
            match &element_bytes {
                Element::Item(v, _) if v.as_slice() == b"tx_v" => {}
                _ => panic!("get_caching_optional should return txk item value"),
            }

            // Verify base is still visible
            let base_bytes = db
                .get_caching_optional((&[b"root".as_slice()]).into(), b"base", true, None, &gv)
                .unwrap()
                .expect("get_caching_optional for base should succeed");
            match &base_bytes {
                Element::Item(v, _) if v.as_slice() == b"base_v" => {}
                _ => panic!("get_caching_optional should return base item value"),
            }
        }
        "facade_get_subtree_root_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"child", &gv);
            expect_tree(&db, &[b"root"], b"tx_child", &gv);
        }
        "facade_has_caching_optional_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"base_v", &gv);

            // Verify txk is visible (use get_caching_optional since Rust doesn't have has_caching_optional)
            let txk_elem = db
                .get_caching_optional((&[b"root".as_slice()]).into(), b"txk", true, None, &gv)
                .unwrap()
                .expect("get_caching_optional after commit should succeed");
            match &txk_elem {
                Element::Item(v, _) if v.as_slice() == b"tx_v" => {}
                _ => panic!("get_caching_optional should return txk item value"),
            }

            // Verify base is still visible
            let base_elem = db
                .get_caching_optional((&[b"root".as_slice()]).into(), b"base", true, None, &gv)
                .unwrap()
                .expect("get_caching_optional for base should succeed");
            match &base_elem {
                Element::Item(v, _) if v.as_slice() == b"base_v" => {}
                _ => panic!("get_caching_optional should return base item value"),
            }
        }
        "facade_query_raw" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"ref1".to_vec());
            let path_query =
                PathQuery::new(vec![b"root".to_vec()], SizedQuery::new(query, None, None));
            let (results, _) = db
                .query_raw(
                    &path_query,
                    true,
                    true,
                    true,
                    QueryKeyElementPairResultType,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("query_raw should return result");
            let pairs = results.to_key_elements();
            if pairs.len() != 1 {
                panic!("query_raw should return exactly one key");
            }
            if pairs[0].0.as_slice() != b"ref1" {
                panic!("query_raw should return ref1 key");
            }
            match &pairs[0].1 {
                Element::Reference(..) => {}
                _ => panic!("query_raw should return unresolved reference element"),
            }
        }
        "facade_query_item_value" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"item2".to_vec());
            query.insert_key(b"ref".to_vec());
            query.insert_key(b"target".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(3), None),
            );
            let (values, _) = db
                .query_item_value(&path_query, true, true, true, None, &gv)
                .unwrap()
                .expect("query_item_value should return result");
            if values.len() != 3 {
                panic!("query_item_value should return exactly three values");
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
        "facade_query_item_value_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"bv", &gv);
            expect_item(&db, &[b"root"], b"txk", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"base".to_vec());
            query.insert_key(b"txk".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(2), None),
            );
            let (values, _) = db
                .query_item_value(&path_query, true, true, true, None, &gv)
                .unwrap()
                .expect("query_item_value should return result");
            if values.len() != 2 {
                panic!("query_item_value should return exactly two values");
            }
            if !values.iter().any(|v| v.as_slice() == b"bv") {
                panic!("query_item_value should include base value");
            }
            if !values.iter().any(|v| v.as_slice() == b"tv") {
                panic!("query_item_value should include tx value");
            }
        }
        "facade_query_item_value_or_sum_tx" => {
            // Verify the tree state after writer committed
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"bv", &gv);
            // Sum items are stored as Item elements with sum metadata
            // Just verify the keys exist
            expect_item(&db, &[b"root"], b"txk", b"tv", &gv);
        }
        "facade_query_raw_keys_optional" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"ref1".to_vec());
            query.insert_key(b"miss".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(2), None),
            );
            let rows = db
                .query_raw_keys_optional(&path_query, true, true, true, None, &gv)
                .unwrap()
                .expect("query_raw_keys_optional should return result");
            if rows.len() != 2 {
                panic!("query_raw_keys_optional should return exactly two rows");
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
                        _ => {
                            panic!(
                                "query_raw_keys_optional ref1 row should keep unresolved reference"
                            )
                        }
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
        "facade_query_keys_optional" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"target", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"ref1".to_vec());
            query.insert_key(b"miss".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(2), None),
            );
            let rows = db
                .query_keys_optional(&path_query, true, true, true, None, &gv)
                .unwrap()
                .expect("query_keys_optional should return result");
            if rows.len() != 2 {
                panic!("query_keys_optional should return exactly two rows");
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
                        _ => panic!("query_keys_optional ref1 row should be resolved item"),
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
        "facade_query_raw_keys_optional_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"bv", &gv);
            expect_item(&db, &[b"root"], b"txk", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"miss".to_vec());
            query.insert_key(b"txk".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(2), None),
            );
            let rows = db
                .query_raw_keys_optional(&path_query, true, true, true, None, &gv)
                .unwrap()
                .expect("query_raw_keys_optional should return result");
            if rows.len() != 2 {
                panic!("query_raw_keys_optional should return exactly two rows");
            }
            let mut saw_txk = false;
            let mut saw_missing = false;
            for row in rows {
                if row.0 != vec![b"root".to_vec()] {
                    panic!("query_raw_keys_optional row path mismatch");
                }
                if row.1.as_slice() == b"txk" {
                    match row.2.as_ref().expect("txk row should have value") {
                        Element::Item(value, ..) => {
                            if value.as_slice() != b"tv" {
                                panic!("query_raw_keys_optional txk row value mismatch");
                            }
                        }
                        _ => panic!("query_raw_keys_optional txk row should be item"),
                    }
                    saw_txk = true;
                } else if row.1.as_slice() == b"miss" {
                    if row.2.is_some() {
                        panic!("query_raw_keys_optional miss row should be None");
                    }
                    saw_missing = true;
                } else {
                    panic!("query_raw_keys_optional returned unexpected key");
                }
            }
            if !saw_txk || !saw_missing {
                panic!("query_raw_keys_optional expected rows for txk and miss");
            }
        }
        "facade_query_raw_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"bv", &gv);
            expect_item(&db, &[b"root"], b"txk", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"miss".to_vec());
            query.insert_key(b"txk".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(2), None),
            );
            let (results, _) = db
                .query_raw(
                    &path_query,
                    true,
                    true,
                    true,
                    QueryKeyElementPairResultType,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("query_raw should return result");
            let pairs = results.to_key_elements();
            if pairs.len() != 1 {
                panic!("query_raw should return exactly one key");
            }
            let mut saw_txk = false;
            for (key, elem) in &pairs {
                if key.as_slice() == b"txk" {
                    match elem {
                        Element::Item(value, ..) => {
                            if value.as_slice() != b"tv" {
                                panic!("query_raw txk value mismatch");
                            }
                        }
                        _ => panic!("query_raw txk should be item"),
                    }
                    saw_txk = true;
                } else {
                    panic!("query_raw returned unexpected key");
                }
            }
            if !saw_txk {
                panic!("query_raw expected row for txk");
            }
        }
        "facade_query_key_element_pairs_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"bv", &gv);
            expect_item(&db, &[b"root"], b"txk", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"base".to_vec());
            query.insert_key(b"txk".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(2), None),
            );
            let (results, _) = db
                .query_raw(
                    &path_query,
                    true,
                    true,
                    true,
                    QueryKeyElementPairResultType,
                    None,
                    &gv,
                )
                .unwrap()
                .expect("query should return result");
            let pairs = results.to_key_elements();
            if pairs.len() != 2 {
                panic!("query should return exactly two pairs");
            }
            let mut saw_base = false;
            let mut saw_txk = false;
            for (key, elem) in &pairs {
                if key.as_slice() == b"base" {
                    match elem {
                        Element::Item(value, ..) => {
                            if value.as_slice() != b"bv" {
                                panic!("query base value mismatch");
                            }
                        }
                        _ => panic!("query base should be item"),
                    }
                    saw_base = true;
                } else if key.as_slice() == b"txk" {
                    match elem {
                        Element::Item(value, ..) => {
                            if value.as_slice() != b"tv" {
                                panic!("query txk value mismatch");
                            }
                        }
                        _ => panic!("query txk should be item"),
                    }
                    saw_txk = true;
                } else {
                    panic!("query returned unexpected key");
                }
            }
            if !saw_base || !saw_txk {
                panic!("query expected rows for base and txk");
            }
        }
        "facade_query_keys_optional_tx" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"base", b"bv", &gv);
            expect_item(&db, &[b"root"], b"txk", b"tv", &gv);
            let mut query = Query::new();
            query.insert_key(b"miss".to_vec());
            query.insert_key(b"txk".to_vec());
            let path_query = PathQuery::new(
                vec![b"root".to_vec()],
                SizedQuery::new(query, Some(2), None),
            );
            let rows = db
                .query_keys_optional(&path_query, true, true, true, None, &gv)
                .unwrap()
                .expect("query_keys_optional should return result");
            if rows.len() != 2 {
                panic!("query_keys_optional should return exactly two rows");
            }
            let mut saw_txk = false;
            let mut saw_missing = false;
            for row in rows {
                if row.0 != vec![b"root".to_vec()] {
                    panic!("query_keys_optional row path mismatch");
                }
                if row.1.as_slice() == b"txk" {
                    match row.2.as_ref().expect("txk row should have value") {
                        Element::Item(value, ..) => {
                            if value.as_slice() != b"tv" {
                                panic!("query_keys_optional txk row value mismatch");
                            }
                        }
                        _ => panic!("query_keys_optional txk row should be item"),
                    }
                    saw_txk = true;
                } else if row.1.as_slice() == b"miss" {
                    if row.2.is_some() {
                        panic!("query_keys_optional miss row should be None");
                    }
                    saw_missing = true;
                } else {
                    panic!("query_keys_optional returned unexpected key");
                }
            }
            if !saw_txk || !saw_missing {
                panic!("query_keys_optional expected rows for txk and miss");
            }
        }
        "facade_query_sums_tx" => {
            let root = db
                .get(&[] as &[&[u8]], b"root", None, &gv)
                .unwrap()
                .expect("root should exist");
            match root {
                Element::SumTree(..) => {}
                _ => panic!("root should be a sum tree"),
            }
            // Verify sum items exist with correct values
            let s1 = db
                .get_raw((&[b"root".as_slice()]).into(), b"s1", None, &gv)
                .unwrap()
                .expect("s1 should exist");
            match s1 {
                Element::SumItem(sum, ..) => {
                    if sum != 10 {
                        panic!("s1 sum value mismatch: expected 10, got {}", sum);
                    }
                }
                _ => panic!("s1 should be a sum item"),
            }
            let s2 = db
                .get_raw((&[b"root".as_slice()]).into(), b"s2", None, &gv)
                .unwrap()
                .expect("s2 should exist");
            match s2 {
                Element::SumItem(sum, ..) => {
                    if sum != 20 {
                        panic!("s2 sum value mismatch: expected 20, got {}", sum);
                    }
                }
                _ => panic!("s2 should be a sum item"),
            }
            let txs = db
                .get_raw((&[b"root".as_slice()]).into(), b"txs", None, &gv)
                .unwrap()
                .expect("txs should exist");
            match txs {
                Element::SumItem(sum, ..) => {
                    if sum != 30 {
                        panic!("txs sum value mismatch: expected 30, got {}", sum);
                    }
                }
                _ => panic!("txs should be a sum item"),
            }
        }
        "nested" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"child", &gv);
            expect_item(&db, &[b"root", b"child"], b"nk", b"nv", &gv);
        }
        "tx_commit" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"ktx", b"tv", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
        }
        "tx_rollback" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kroll", &gv);
        }
        "tx_visibility_rollback" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kvis", &gv);
        }
        "tx_mixed_commit" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"kmix", b"mv", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
        }
        "tx_rollback_range" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kroll", &gv);
        }
        "tx_drop_abort" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kdrop", &gv);
        }
        "tx_commit_after_rollback_rejected" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kcar", &gv);
        }
        "tx_write_after_rollback_rejected" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kwar", b"wv", &gv);
        }
        "tx_multi_rollback_reuse" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kmr1", &gv);
            expect_item(&db, &[b"root"], b"kmr2", b"mv2", &gv);
        }
        "tx_delete_after_rollback" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
        }
        "tx_reopen_visibility_after_rollback" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"krv", b"rv", &gv);
        }
        "tx_reopen_conflict_same_path" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"krc", b"a", &gv);
        }
        "tx_delete_visibility" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
        }
        "tx_delete_then_reinsert_same_key" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v2", &gv);
        }
        "tx_insert_then_delete_same_key" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
        }
        "tx_delete_missing_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"missing", &gv);
        }
        "tx_read_committed_visibility" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"ksnap", b"sv", &gv);
        }
        "tx_has_read_committed_visibility" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"khas", b"hv", &gv);
        }
        "tx_query_range_committed_visibility" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kqr", b"qv", &gv);
        }
        "tx_iterator_stability_under_commit" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kit", b"iv", &gv);
        }
        "tx_same_key_conflict_reverse_order" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kconf", b"v2", &gv);
        }
        "tx_shared_subtree_disjoint_conflict_reverse_order" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kd2", b"d2", &gv);
            expect_missing(&db, &[b"root"], b"kd1", &gv);
        }
        "tx_disjoint_subtree_conflict_reverse_order" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"left", &gv);
            expect_tree(&db, &[b"root"], b"right", &gv);
            expect_item(&db, &[b"root", b"right"], b"rk", b"rv", &gv);
            expect_missing(&db, &[b"root", b"left"], b"lk", &gv);
        }
        "tx_disjoint_subtree_conflict_forward_order" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"left", &gv);
            expect_tree(&db, &[b"root"], b"right", &gv);
            expect_item(&db, &[b"root", b"left"], b"lkf", b"lvf", &gv);
            expect_missing(&db, &[b"root", b"right"], b"rkf", &gv);
        }
        "tx_read_only_then_writer_commit" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kro", b"rov", &gv);
        }
        "tx_delete_insert_same_key_forward" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
        }
        "tx_delete_insert_same_key_reverse" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v2", &gv);
        }
        "tx_delete_insert_same_subtree_disjoint_forward" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
            expect_missing(&db, &[b"root"], b"kdi", &gv);
        }
        "tx_delete_insert_same_subtree_disjoint_reverse" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kdi", b"di", &gv);
        }
        "tx_delete_insert_disjoint_subtree_forward" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"left", &gv);
            expect_tree(&db, &[b"root"], b"right", &gv);
            expect_missing(&db, &[b"root", b"left"], b"lkdel", &gv);
            expect_missing(&db, &[b"root", b"right"], b"rkins", &gv);
        }
        "tx_delete_insert_disjoint_subtree_reverse" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"left", &gv);
            expect_tree(&db, &[b"root"], b"right", &gv);
            expect_item(&db, &[b"root", b"left"], b"lkdel", b"delv", &gv);
            expect_item(&db, &[b"root", b"right"], b"rkins", b"insv", &gv);
        }
        "tx_replace_delete_same_key_forward" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v2", &gv);
        }
        "tx_replace_delete_same_key_reverse" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
        }
        "tx_replace_replace_same_key_forward" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v2", &gv);
        }
        "tx_replace_replace_same_key_reverse" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v3", &gv);
        }
        "tx_double_rollback_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kdr", b"drv", &gv);
        }
        "tx_conflict_sequence_persistence" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"left", &gv);
            expect_tree(&db, &[b"root"], b"right", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kc", b"a", &gv);
            expect_item(&db, &[b"root", b"right"], b"rk1", b"r1", &gv);
            expect_missing(&db, &[b"root", b"left"], b"lk1", &gv);
            expect_item(&db, &[b"root"], b"kpost", b"pv", &gv);
        }
        "tx_checkpoint_snapshot_isolation" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kcp", b"cv", &gv);
            expect_item(&db, &[b"root"], b"kpost", b"pv", &gv);
        }
        "tx_checkpoint_independent_writes" => {
            expect_tree(&db, &[], b"key1", &gv);
            expect_tree(&db, &[b"key1"], b"key2", &gv);
            expect_item(&db, &[b"key1", b"key2"], b"key3", b"ayy", &gv);
            expect_item(&db, &[b"key1"], b"key4", b"ayy3", &gv);
            expect_item(&db, &[b"key1"], b"key6", b"ayy3", &gv);
            expect_missing(&db, &[b"key1"], b"key5", &gv);
        }
        "tx_checkpoint_delete_safety" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"ksafe", b"sv", &gv);
        }
        "tx_checkpoint_open_safety" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kopen", b"ov", &gv);
        }
        "tx_checkpoint_delete_short_path_safety" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kshort", b"sv", &gv);
        }
        "tx_checkpoint_open_missing_path" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kmiss", b"mv", &gv);
        }
        "tx_checkpoint_reopen_mutate_recheckpoint" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kmain", b"mv", &gv);
            expect_missing(&db, &[b"root"], b"kcp1", &gv);
            expect_missing(&db, &[b"root"], b"kcp2", &gv);
        }
        "tx_checkpoint_reopen_mutate_chain" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kmain2", b"mv2", &gv);
            expect_missing(&db, &[b"root"], b"ka1", &gv);
            expect_missing(&db, &[b"root"], b"kb1", &gv);
            expect_missing(&db, &[b"root"], b"kb2", &gv);
        }
        "tx_checkpoint_batch_ops" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kmain3", b"mv3", &gv);
            expect_item(&db, &[b"root"], b"kbatch1", b"vb1", &gv);
            expect_missing(&db, &[b"root"], b"kbp1", &gv);
            expect_missing(&db, &[b"root"], b"kbp2", &gv);
            expect_missing(&db, &[b"root"], b"kbp3", &gv);
        }
        "tx_checkpoint_delete_reopen_sequence" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kseq", b"sv", &gv);
            expect_missing(&db, &[b"root"], b"kcpb", &gv);
            expect_missing(&db, &[b"root"], b"kcpa", &gv);
        }
        "tx_checkpoint_aux_isolation" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kauxcp", b"ok", &gv);
            expect_aux_value(&db, b"aux_shared", b"main_after");
            expect_aux_value(&db, b"aux_main_only", b"main_only");
            expect_aux_missing(&db, b"aux_cp_only");
        }
        "tx_checkpoint_tx_operations" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"ktx1", b"txv1", &gv);
            expect_item(&db, &[b"root"], b"ktx2", b"txv2", &gv);
        }
        "tx_checkpoint_chain_mutation_isolation" => {
            // Verify main DB has all keys: k_base, k_phase2, k_phase3, k_final
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k_base", b"base", &gv);
            expect_item(&db, &[b"root"], b"k_phase2", b"phase2", &gv);
            expect_item(&db, &[b"root"], b"k_phase3", b"phase3", &gv);
            expect_item(&db, &[b"root"], b"k_final", b"final", &gv);
        }
        "tx_checkpoint_reopen_after_main_delete" => {
            // Verify checkpoint contains k_base and k_snapshot after main DB deletion
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k_base", b"base", &gv);
            expect_item(&db, &[b"root"], b"k_snapshot", b"snapshot_data", &gv);
        }
        "batch_apply_local_atomic" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k2", b"b2", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
            expect_missing(&db, &[], b"nt", &gv);
            expect_missing(&db, &[b"nt"], b"kbad", &gv);
        }
        "batch_apply_tx_visibility" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"ktxb", b"tb", &gv);
        }
        "batch_apply_empty_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"k2", &gv);
        }
        "batch_validate_success_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kval", &gv);
        }
        "batch_validate_failure_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[], b"nval", &gv);
        }
        "batch_insert_only_semantics" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"xx", &gv);
            expect_item(&db, &[b"root"], b"kio", b"io", &gv);
        }
        "batch_replace_semantics" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"r2", &gv);
            expect_item(&db, &[b"root"], b"kmiss", b"rx", &gv);
        }
        "batch_validate_no_override_insert" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kstrict", b"sv", &gv);
        }
        "batch_validate_no_override_insert_only" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kio2", b"iv", &gv);
        }
        "batch_validate_no_override_replace" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"krep2", b"rv", &gv);
        }
        "batch_validate_no_override_tree_insert" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"it", &gv);
        }
        "batch_validate_strict_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kvs", &gv);
        }
        "batch_delete_non_empty_tree_error" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"child", &gv);
        }
        "batch_delete_non_empty_tree_no_error" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"child", &gv);
        }
        "batch_disable_consistency_check" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"k1", &gv);
        }
        "batch_disable_consistency_last_op_wins" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v2", &gv);
        }
        "batch_disable_consistency_reorder_parent_child" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"ord2", &gv);
            expect_item(&db, &[b"root", b"ord2"], b"nk", b"nv", &gv);
        }
        "batch_insert_tree_semantics" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"child", &gv);
            expect_item(&db, &[b"root", b"child"], b"nk", b"nv", &gv);
        }
        "batch_insert_tree_replace" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"newtree", &gv);
            expect_item(&db, &[b"root", b"newtree"], b"nested", b"nested_value", &gv);
        }
        "batch_sum_tree_create_and_sum_item" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_sum_tree_sum(&db, &[b"root"], b"sbatch", 7, &gv);
            expect_sum_item(&db, &[b"root", b"sbatch"], b"s1", 7, &gv);
        }
        "batch_count_tree_create_and_item" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_count_tree_count(&db, &[b"root"], b"cbatch", 1, &gv);
            expect_item(&db, &[b"root", b"cbatch"], b"c1", b"cv", &gv);
        }
        "batch_provable_count_tree_create_and_item" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_provable_count_tree_count(&db, &[b"root"], b"pcbatch", 1, &gv);
            expect_item(&db, &[b"root", b"pcbatch"], b"p1", b"pv", &gv);
        }
        "batch_count_sum_tree_create_and_sum_item" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_count_sum_tree(&db, &[b"root"], b"csbatch", 1, 11, &gv);
            expect_sum_item(&db, &[b"root", b"csbatch"], b"cs1", 11, &gv);
        }
        "batch_provable_count_sum_tree_create_and_sum_item" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_provable_count_sum_tree(&db, &[b"root"], b"pcsbatch", 1, 13, &gv);
            expect_sum_item(&db, &[b"root", b"pcsbatch"], b"ps1", 13, &gv);
        }
        "batch_apply_failure_atomic_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"kok", &gv);
        }
        "batch_delete_missing_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"missing", &gv);
        }
        "batch_apply_tx_failure_atomic_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"ktxok", &gv);
        }
        "batch_apply_tx_failure_then_reuse" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"ktxok", &gv);
            expect_item(&db, &[b"root"], b"kreuse", b"rv", &gv);
        }
        "batch_apply_tx_failure_then_rollback" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"ktxok", &gv);
        }
        "batch_apply_tx_success_then_rollback" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"ktsr", &gv);
        }
        "batch_apply_tx_delete_then_rollback" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
        }
        "batch_apply_tx_delete_missing_noop" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_missing(&db, &[b"root"], b"missing", &gv);
        }
        "batch_delete_tree_op" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"child", &gv);
        }
        "batch_delete_tree_disable_consistency_check" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"child", &gv);
        }
        "batch_delete_tree_non_empty_options" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_missing(&db, &[b"root"], b"child", &gv);
        }
        "batch_mixed_non_minimal_ops" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"r2", &gv);
            expect_item(&db, &[b"root"], b"k2", b"b2", &gv);
            expect_missing(&db, &[b"root"], b"child", &gv);
        }
        "batch_mixed_non_minimal_ops_with_options" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"r3", &gv);
            expect_item(&db, &[b"root"], b"k3", b"b3", &gv);
            expect_missing(&db, &[b"root"], b"child", &gv);
        }
        "batch_patch_existing" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"p1", &gv);
        }
        "batch_patch_missing" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"k2", b"p2", &gv);
        }
        "batch_patch_strict_no_override" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
            expect_item(&db, &[b"root"], b"kp", b"pv", &gv);
        }
        "batch_refresh_reference_trust" => {
            expect_tree(&db, &[], b"root", &gv);
            let elem = db
                .get_raw((&[b"root".as_slice()]).into(), b"ref_missing", None, &gv)
                .unwrap()
                .expect("trusted refresh reference should exist");
            match elem {
                Element::Reference(path, max_hop, _) => {
                    if path
                        != grovedb::reference_path::ReferencePathType::AbsolutePathReference(vec![
                            b"root".to_vec(),
                            b"k1".to_vec(),
                        ])
                    {
                        panic!("trusted refresh reference path mismatch");
                    }
                    if max_hop.is_some() {
                        panic!("trusted refresh reference max_hop mismatch");
                    }
                }
                _ => panic!("trusted refresh should store reference element"),
            }
        }
        "batch_pause_height_passthrough" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k2", b"v2", &gv);
            // k1 should be deleted.
            let k1 = db
                .get_raw((&[b"root".as_slice()]).into(), b"k1", None, &gv)
                .unwrap();
            if k1.is_ok() {
                panic!("k1 should be deleted after batch_pause_height batch");
            }
        }
        "batch_partial_pause_resume" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"a", &gv);
            expect_tree(&db, &[b"root", b"a"], b"b", &gv);
            // Rust apply_partial_batch continuation callback only applies the
            // returned add-on ops in this scenario; paused leftovers are not
            // present in the final state.
            let l0 = db.get_raw((&[] as &[&[u8]]).into(), b"l0", None, &gv).unwrap();
            if l0.is_ok() {
                panic!("l0 should be absent after batch_partial_pause_resume");
            }
            let l1 = db
                .get_raw((&[b"root".as_slice()]).into(), b"l1", None, &gv)
                .unwrap();
            if l1.is_ok() {
                panic!("l1 should be absent after batch_partial_pause_resume");
            }
            expect_item(&db, &[b"root", b"a", b"b"], b"add", b"va", &gv);
        }
        "batch_base_root_storage_is_free_passthrough" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k2", b"v2", &gv);
            // k1 should be deleted.
            let k1 = db
                .get_raw((&[b"root".as_slice()]).into(), b"k1", None, &gv)
                .unwrap();
            if k1.is_ok() {
                panic!("k1 should be deleted after batch_base_root_storage_is_free batch");
            }
        }
        "batch_insert_or_replace_semantics" => {
            expect_tree(&db, &[], b"root", &gv);
            // kior should have final value v2 (idempotent replace behavior)
            expect_item(&db, &[b"root"], b"kior", b"v2", &gv);
        }
        "batch_insert_or_replace_with_override_validation" => {
            // After failed batch, state should remain unchanged
            expect_tree(&db, &[], b"root", &gv);
            expect_item(&db, &[b"root"], b"k1", b"v1", &gv);
        }
        "batch_validate_no_override_tree_insert_or_replace" => {
            // After tree override failure, root should still be a tree
            expect_tree(&db, &[], b"root", &gv);
            // k1 should have been replaced with 'it' value
            expect_item(&db, &[b"root"], b"k1", b"it", &gv);
        }
        "batch_insert_tree_below_deleted_path_consistency" => {
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"ct", &gv);
        }
        "batch_insert_tree_with_root_hash" => {
            // Verify tree was inserted via InsertTreeWithRootHash batch operation
            expect_tree(&db, &[], b"root", &gv);
            expect_tree(&db, &[b"root"], b"tree_key", &gv);
        }
        _ => panic!("unsupported mode"),
    }
}
