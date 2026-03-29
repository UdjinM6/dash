use std::env;
use std::fs;
use std::path::Path;
use std::collections::HashSet;

use grovedb::{
    batch::{BatchApplyOptions, QualifiedGroveDbOp},
    Element, Error, GroveDb, PathQuery, TransactionArg,
};
use grovedb_merk::proofs::Query;
use grovedb_merk::proofs::query::query_item::QueryItem;
use grovedb_version::version::GroveVersion;

struct Lcg(u64);

impl Lcg {
    fn new(seed: u64) -> Self {
        Self(seed)
    }

    fn next_u32(&mut self) -> u32 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (self.0 >> 32) as u32
    }

    fn range(&mut self, upper: u32) -> u32 {
        if upper == 0 {
            0
        } else {
            self.next_u32() % upper
        }
    }
}

fn to_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(HEX[(b >> 4) as usize] as char);
        out.push(HEX[(b & 0x0f) as usize] as char);
    }
    out
}

fn main() {
    let out_dir = env::args().nth(1).expect("output directory required");
    let seed: u64 = env::args()
        .nth(2)
        .unwrap_or_else(|| "12345".to_string())
        .parse()
        .expect("seed must be u64");
    let batches: usize = env::args()
        .nth(3)
        .unwrap_or_else(|| "40".to_string())
        .parse()
        .expect("batches must be usize");
    let queries: usize = env::args()
        .nth(4)
        .unwrap_or_else(|| "20".to_string())
        .parse()
        .expect("queries must be usize");

    let out_path = Path::new(&out_dir);
    fs::create_dir_all(out_path).expect("create output dir");
    let db_path = out_path.join("rust_db");
    let _ = fs::remove_dir_all(&db_path);
    let db = GroveDb::open(&db_path).expect("open db");
    let grove_version = GroveVersion::latest();

    db.insert(
        &[] as &[&[u8]],
        b"root",
        Element::new_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let mut rng = Lcg::new(seed);
    let mut ops_out = String::new();
    ops_out.push_str(&format!("SEED {}\n", seed));
    let mut roots_out = String::new();
    let mut states_out = String::new();

    for _ in 0..batches {
        let disable_consistency = false;
        let mut opts = BatchApplyOptions::default();
        opts.disable_operation_consistency_check = disable_consistency;
        let use_tx = false;
        let ops_in_batch = 1;
        let mut ops = Vec::with_capacity(ops_in_batch);
        let mut used_keys: HashSet<String> = HashSet::new();
        ops_out.push_str(&format!(
            "BATCH {} {}\n",
            if disable_consistency { 1 } else { 0 },
            if use_tx { 1 } else { 0 }
        ));

        for _ in 0..ops_in_batch {
            let kind = if use_tx { 0 } else { rng.range(2) };
            let key = loop {
                let candidate = format!("k{:02}", rng.range(12));
                if used_keys.insert(candidate.clone()) {
                    break candidate;
                }
            };
            let key_bytes = key.as_bytes().to_vec();
            match kind {
                0 => {
                    let value = format!("v{}_{}", rng.range(1000), rng.range(1000));
                    ops.push(QualifiedGroveDbOp::insert_or_replace_op(
                        vec![b"root".to_vec()],
                        key_bytes.clone(),
                        Element::new_item(value.as_bytes().to_vec()),
                    ));
                    ops_out.push_str(&format!("OP PUT_ITEM root {} {}\n", key, value));
                }
                1 => {
                    ops.push(QualifiedGroveDbOp::delete_op(
                        vec![b"root".to_vec()],
                        key_bytes.clone(),
                    ));
                    ops_out.push_str(&format!("OP DELETE root {}\n", key));
                }
                _ => unreachable!(),
            }
        }

        if use_tx {
            let tx = db.start_transaction();
            db.apply_batch(ops, Some(opts), Some(&tx), &grove_version)
                .unwrap()
                .expect("generated tx batch should be valid and succeed");
            ops_out.push_str("END\n");
            let root_hash = db
                .root_hash(Some(&tx), &grove_version)
                .unwrap()
                .expect("tx root hash after batch");
            roots_out.push_str(&format!("ROOT {}\n", to_hex(&root_hash)));

            states_out.push_str("STATE");
            for idx in 0..12 {
                let key = format!("k{:02}", idx);
                let got = db
                    .get(
                        &[b"root".as_slice()],
                        key.as_bytes(),
                        Some(&tx),
                        &grove_version,
                    )
                    .unwrap();
                let encoded = match got {
                    Ok(Element::Item(value, _)) => to_hex(&value),
                    Ok(Element::Tree(..)) => "T".to_string(),
                    Ok(Element::SumTree(..)) => "ST".to_string(),
                    Ok(Element::BigSumTree(..)) => "BST".to_string(),
                    Ok(Element::CountTree(..)) => "CT".to_string(),
                    Ok(Element::CountSumTree(..)) => "CST".to_string(),
                    Ok(Element::ProvableCountTree(..)) => "PCT".to_string(),
                    Ok(Element::ProvableCountSumTree(..)) => "PCST".to_string(),
                    Ok(Element::Reference(..)) => "REF".to_string(),
                    Ok(Element::SumItem(..)) => "SI".to_string(),
                    Ok(Element::ItemWithSumItem(..)) => "IWSI".to_string(),
                    Err(_) => "-".to_string(),
                };
                states_out.push_str(&format!(" {}={}", key, encoded));
            }
            states_out.push('\n');
            db.commit_transaction(tx)
                .unwrap()
                .expect("commit differential fuzz tx batch");
            continue;
        }

        db.apply_batch(ops, Some(opts), None, &grove_version)
            .unwrap()
            .expect("generated batch should be valid and succeed");
        ops_out.push_str("END\n");
        let root_hash = db
            .root_hash(TransactionArg::None, &grove_version)
            .unwrap()
            .expect("root hash after batch");
        roots_out.push_str(&format!("ROOT {}\n", to_hex(&root_hash)));

        states_out.push_str("STATE");
        for idx in 0..12 {
            let key = format!("k{:02}", idx);
            let got = db
                .get(
                    &[b"root".as_slice()],
                    key.as_bytes(),
                    None,
                    &grove_version,
                )
                .unwrap();
            let encoded = match got {
                Ok(Element::Item(value, _)) => to_hex(&value),
                Ok(Element::Tree(..)) => "T".to_string(),
                Ok(Element::SumTree(..)) => "ST".to_string(),
                Ok(Element::BigSumTree(..)) => "BST".to_string(),
                Ok(Element::CountTree(..)) => "CT".to_string(),
                Ok(Element::CountSumTree(..)) => "CST".to_string(),
                Ok(Element::ProvableCountTree(..)) => "PCT".to_string(),
                Ok(Element::ProvableCountSumTree(..)) => "PCST".to_string(),
                Ok(Element::Reference(..)) => "REF".to_string(),
                Ok(Element::SumItem(..)) => "SI".to_string(),
                Ok(Element::ItemWithSumItem(..)) => "IWSI".to_string(),
                Err(_) => "-".to_string(),
            };
            states_out.push_str(&format!(" {}={}", key, encoded));
        }
        states_out.push('\n');
    }

    let mut query_out = String::new();
    for _ in 0..queries {
        let key = format!("k{:02}", rng.range(12));
        if rng.range(2) == 0 {
            let path_query =
                PathQuery::new_single_key(vec![b"root".to_vec()], key.as_bytes().to_vec());
            let proof = db
                .prove_query(&path_query, None, &grove_version)
                .unwrap()
                .expect("key proof");
            query_out.push_str(&format!("Q KEY root {} {}\n", key, to_hex(&proof)));
            continue;
        }

        let tx = db.start_transaction();
        let stage_put = rng.range(2) == 0;
        let staged_value = format!("qv{}_{}", rng.range(1000), rng.range(1000));
        if stage_put {
            db.insert(
                &[b"root".as_slice()],
                key.as_bytes(),
                Element::new_item(staged_value.as_bytes().to_vec()),
                None,
                Some(&tx),
                &grove_version,
            )
            .unwrap()
            .expect("stage tx put");
        } else {
            let delete_result = db
                .delete(
                &[b"root".as_slice()],
                key.as_bytes(),
                None,
                Some(&tx),
                &grove_version,
            )
            .unwrap();
            match delete_result {
                Ok(()) => {}
                Err(Error::PathKeyNotFound(_)) => {}
                Err(e) => panic!("stage tx delete: {e:?}"),
            }
        }
        let got = db
            .get(
                &[b"root".as_slice()],
                key.as_bytes(),
                Some(&tx),
                &grove_version,
            )
            .unwrap();
        let expected_state = match got {
            Ok(Element::Item(value, _)) => to_hex(&value),
            Err(_) => "-".to_string(),
            Ok(_) => panic!("unexpected non-item value in differential tx query mode"),
        };
        if stage_put {
            query_out.push_str(&format!(
                "Q KEY_TX root {} PUT_ITEM {} {}\n",
                key,
                to_hex(staged_value.as_bytes()),
                expected_state
            ));
        } else {
            query_out.push_str(&format!("Q KEY_TX root {} DELETE - {}\n", key, expected_state));
        }
        db.rollback_transaction(&tx).unwrap();
    }

    db.insert(
        &[b"root".as_slice()],
        b"skey",
        Element::new_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert skey tree");
    db.insert(
        &[b"root".as_slice(), b"skey".as_slice()],
        b"branch",
        Element::new_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert skey/branch tree");
    db.insert(
        &[b"root".as_slice(), b"skey".as_slice(), b"branch".as_slice()],
        b"xa",
        Element::new_item(b"sub_a".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert skey/branch/xa");
    db.insert(
        &[b"root".as_slice(), b"skey".as_slice(), b"branch".as_slice()],
        b"xb",
        Element::new_item(b"sub_b".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert skey/branch/xb");

    let mut subquery_path_query = Query::new();
    subquery_path_query.insert_key(b"skey".to_vec());
    subquery_path_query.set_subquery_path(vec![b"branch".to_vec()]);
    subquery_path_query.set_subquery(Query::new_range_full());
    let subquery_path_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec()], subquery_path_query);
    let subquery_path_proof = db
        .prove_query(&subquery_path_path_query, None, &grove_version)
        .unwrap()
        .expect("subquery_path proof");
    query_out.push_str(&format!("Q SUBQ_PATH root {}\n", to_hex(&subquery_path_proof)));

    db.insert(
        &[b"root".as_slice()],
        b"ckey",
        Element::new_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ckey tree");
    db.insert(
        &[b"root".as_slice(), b"ckey".as_slice()],
        b"ca",
        Element::new_item(b"cond_a".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ckey/ca");
    db.insert(
        &[b"root".as_slice(), b"ckey".as_slice()],
        b"cb",
        Element::new_item(b"cond_b".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ckey/cb");
    db.insert(
        &[b"root".as_slice()],
        b"ck2",
        Element::new_item(b"cond_leaf".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert ck2 item");

    let mut conditional_query = Query::new();
    conditional_query.insert_key(b"ckey".to_vec());
    conditional_query.insert_key(b"ck2".to_vec());
    conditional_query.set_subquery(Query::new_range_full());
    conditional_query.add_conditional_subquery(QueryItem::Key(b"ck2".to_vec()), None, None);
    let conditional_path_query = PathQuery::new_unsized(vec![b"root".to_vec()], conditional_query);
    let conditional_proof = db
        .prove_query(&conditional_path_query, None, &grove_version)
        .unwrap()
        .expect("conditional_subquery proof");
    query_out.push_str(&format!("Q COND_SUBQ root {}\n", to_hex(&conditional_proof)));

    db.insert(
        &[b"root".as_slice()],
        b"aggt",
        Element::new_sum_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert aggt sum tree");
    db.insert(
        &[b"root".as_slice(), b"aggt".as_slice()],
        b"s1",
        Element::new_sum_item(3),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert aggt/s1");
    db.insert(
        &[b"root".as_slice(), b"aggt".as_slice()],
        b"s2",
        Element::new_sum_item(4),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert aggt/s2");
    let agg_path_query =
        PathQuery::new_single_key(vec![b"root".to_vec(), b"aggt".to_vec()], b"s1".to_vec());
    let agg_proof = db
        .prove_query(&agg_path_query, None, &grove_version)
        .unwrap()
        .expect("aggregate key proof");
    query_out.push_str(&format!("Q AGG_KEY root/aggt s1 {}\n", to_hex(&agg_proof)));
    let agg_range_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec(), b"aggt".to_vec()], Query::new_range_full());
    let agg_range_proof = db
        .prove_query(&agg_range_path_query, None, &grove_version)
        .unwrap()
        .expect("aggregate range proof");
    query_out.push_str(&format!(
        "Q AGG_SUM_RANGE root/aggt {}\n",
        to_hex(&agg_range_proof)
    ));

    db.insert(
        &[b"root".as_slice()],
        b"aggp",
        Element::new_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert aggp tree");
    db.insert(
        &[b"root".as_slice(), b"aggp".as_slice()],
        b"inner",
        Element::new_sum_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert aggp/inner sum tree");
    db.insert(
        &[b"root".as_slice(), b"aggp".as_slice(), b"inner".as_slice()],
        b"n1",
        Element::new_sum_item(5),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert aggp/inner/n1");
    let nested_agg_query = PathQuery::new_single_key(
        vec![b"root".to_vec(), b"aggp".to_vec(), b"inner".to_vec()],
        b"n1".to_vec(),
    );
    let nested_agg_proof = db
        .prove_query(&nested_agg_query, None, &grove_version)
        .unwrap()
        .expect("nested aggregate key proof");
    query_out.push_str(&format!(
        "Q AGG_NESTED_KEY root/aggp/inner n1 {}\n",
        to_hex(&nested_agg_proof)
    ));

    db.insert(
        &[b"root".as_slice()],
        b"aggr",
        Element::new_sum_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert aggr sum tree");
    let mut agg_spec_parts: Vec<String> = Vec::new();
    for idx in 0..4 {
        let key = format!("g{}", idx);
        let sum = (rng.range(201) as i64) - 100;
        db.insert(
            &[b"root".as_slice(), b"aggr".as_slice()],
            key.as_bytes(),
            Element::new_sum_item(sum),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert randomized aggr sum item");
        agg_spec_parts.push(format!("{}={}", key, sum));
    }
    let agg_target = format!("g{}", rng.range(4));
    let agg_gen_query = PathQuery::new_single_key(
        vec![b"root".to_vec(), b"aggr".to_vec()],
        agg_target.as_bytes().to_vec(),
    );
    let agg_gen_proof = db
        .prove_query(&agg_gen_query, None, &grove_version)
        .unwrap()
        .expect("aggregate generated key proof");
    query_out.push_str(&format!(
        "Q AGG_KEY_GEN root/aggr {} {} {}\n",
        agg_target,
        to_hex(&agg_gen_proof),
        agg_spec_parts.join(",")
    ));

    db.insert(
        &[b"root".as_slice()],
        b"countt",
        Element::new_count_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert countt tree");
    db.insert(
        &[b"root".as_slice(), b"countt".as_slice()],
        b"c1",
        Element::new_item(b"cv1".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert countt/c1");
    db.insert(
        &[b"root".as_slice(), b"countt".as_slice()],
        b"c2",
        Element::new_item(b"cv2".to_vec()),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert countt/c2");
    let count_target = if rng.range(2) == 0 { "c1" } else { "c2" };
    let count_query = PathQuery::new_single_key(
        vec![b"root".to_vec(), b"countt".to_vec()],
        count_target.as_bytes().to_vec(),
    );
    let count_proof = db
        .prove_query(&count_query, None, &grove_version)
        .unwrap()
        .expect("count-tree key proof");
    query_out.push_str(&format!(
        "Q AGG_COUNT_KEY root/countt {} {}\n",
        count_target,
        to_hex(&count_proof)
    ));

    db.insert(
        &[b"root".as_slice()],
        b"countgr",
        Element::new_count_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert countgr tree");
    let mut count_spec_parts: Vec<String> = Vec::new();
    for idx in 0..3 {
        let key = format!("r{}", idx);
        let value = format!("cv{}_{}", rng.range(1000), rng.range(1000));
        db.insert(
            &[b"root".as_slice(), b"countgr".as_slice()],
            key.as_bytes(),
            Element::new_item(value.as_bytes().to_vec()),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert randomized countgr item");
        count_spec_parts.push(format!("{}={}", key, to_hex(value.as_bytes())));
    }
    let count_gen_target = format!("r{}", rng.range(3));
    let count_gen_query = PathQuery::new_single_key(
        vec![b"root".to_vec(), b"countgr".to_vec()],
        count_gen_target.as_bytes().to_vec(),
    );
    let count_gen_proof = db
        .prove_query(&count_gen_query, None, &grove_version)
        .unwrap()
        .expect("countgr generated key proof");
    query_out.push_str(&format!(
        "Q AGG_COUNT_GEN root/countgr {} {} {}\n",
        count_gen_target,
        to_hex(&count_gen_proof),
        count_spec_parts.join(",")
    ));
    let mut count_range_query = Query::new();
    count_range_query.insert_range(b"r0".to_vec()..b"r9".to_vec());
    let count_range_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec(), b"countgr".to_vec()], count_range_query);
    let count_range_proof = db
        .prove_query(&count_range_path_query, None, &grove_version)
        .unwrap()
        .expect("countgr generated range proof");
    query_out.push_str(&format!(
        "Q AGG_COUNT_RANGE_GEN root/countgr {} {}\n",
        to_hex(&count_range_proof),
        count_spec_parts.join(",")
    ));

    db.insert(
        &[b"root".as_slice()],
        b"bigt",
        Element::new_big_sum_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert bigt tree");
    db.insert(
        &[b"root".as_slice(), b"bigt".as_slice()],
        b"b1",
        Element::new_sum_item(9),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert bigt/b1");
    db.insert(
        &[b"root".as_slice(), b"bigt".as_slice()],
        b"b2",
        Element::new_sum_item(12),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert bigt/b2");
    let big_target = if rng.range(2) == 0 { "b1" } else { "b2" };
    let big_query = PathQuery::new_single_key(
        vec![b"root".to_vec(), b"bigt".to_vec()],
        big_target.as_bytes().to_vec(),
    );
    let big_proof = db
        .prove_query(&big_query, None, &grove_version)
        .unwrap()
        .expect("big-sum-tree key proof");
    query_out.push_str(&format!(
        "Q AGG_BIG_KEY root/bigt {} {}\n",
        big_target,
        to_hex(&big_proof)
    ));

    db.insert(
        &[b"root".as_slice()],
        b"biggr",
        Element::new_big_sum_tree(None),
        None,
        TransactionArg::None,
        &grove_version,
    )
    .unwrap()
    .expect("insert biggr tree");
    let mut big_spec_parts: Vec<String> = Vec::new();
    for idx in 0..3 {
        let key = format!("q{}", idx);
        let sum = rng.range(21) as i64;
        db.insert(
            &[b"root".as_slice(), b"biggr".as_slice()],
            key.as_bytes(),
            Element::new_sum_item(sum),
            None,
            TransactionArg::None,
            &grove_version,
        )
        .unwrap()
        .expect("insert randomized biggr sum item");
        big_spec_parts.push(format!("{}={}", key, sum));
    }
    let big_gen_target = format!("q{}", rng.range(3));
    let big_gen_query = PathQuery::new_single_key(
        vec![b"root".to_vec(), b"biggr".to_vec()],
        big_gen_target.as_bytes().to_vec(),
    );
    let big_gen_proof = db
        .prove_query(&big_gen_query, None, &grove_version)
        .unwrap()
        .expect("biggr generated key proof");
    query_out.push_str(&format!(
        "Q AGG_BIG_GEN root/biggr {} {} {}\n",
        big_gen_target,
        to_hex(&big_gen_proof),
        big_spec_parts.join(",")
    ));
    let mut big_range_query = Query::new();
    big_range_query.insert_range(b"q0".to_vec()..b"q9".to_vec());
    let big_range_path_query =
        PathQuery::new_unsized(vec![b"root".to_vec(), b"biggr".to_vec()], big_range_query);
    let big_range_proof = db
        .prove_query(&big_range_path_query, None, &grove_version)
        .unwrap()
        .expect("biggr generated range proof");
    query_out.push_str(&format!(
        "Q AGG_BIG_RANGE_GEN root/biggr {} {}\n",
        to_hex(&big_range_proof),
        big_spec_parts.join(",")
    ));

    let mut malformed = conditional_proof.clone();
    if !malformed.is_empty() {
        malformed.pop();
    }
    query_out.push_str(&format!("Q BAD_PROOF root {}\n", to_hex(&malformed)));

    fs::write(out_path.join("ops.txt"), ops_out).expect("write ops");
    fs::write(out_path.join("roots.txt"), roots_out).expect("write roots");
    fs::write(out_path.join("states.txt"), states_out).expect("write states");
    fs::write(out_path.join("queries.txt"), query_out).expect("write queries");
}
