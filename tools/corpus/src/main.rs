use std::fs;
use std::path::PathBuf;

use grovedb::{Element, GroveDb, PathQuery, Query};
use grovedb::reference_path::ReferencePathType;
use grovedb::operations::delete::DeleteOptions;
use grovedb_costs::storage_cost::removal::StorageRemovedBytes;
use grovedb_costs::OperationCost;
use grovedb_version::version::GroveVersion;
use serde::Serialize;
use tempfile::TempDir;

#[derive(Serialize)]
struct Corpus {
    version: String,
    cases: Vec<CorpusCase>,
}

#[derive(Serialize)]
struct CorpusCase {
    name: String,
    path: Vec<String>,
    keys: Vec<String>,
    element_bytes_list: Vec<String>,
    item_bytes_list: Vec<String>,
    expect_present: bool,
    proof: String,
    subtree_key: String,
    root_hash: String,
    cost: CostSummary,
}

#[derive(Serialize)]
struct OutputCorpus {
    version: String,
    cases: Vec<OutputCorpusCase>,
}

#[derive(Serialize)]
struct OutputCorpusCase {
    name: String,
    path: Vec<String>,
    keys: Vec<String>,
    element_bytes_list: Vec<String>,
    item_bytes_list: Vec<String>,
    query_descriptor: QueryDescriptor,
    expect_present: bool,
    proof: String,
    subtree_key: String,
    root_hash: String,
    cost: CostSummary,
}

#[derive(Serialize)]
struct QueryDescriptor {
    kind: String,
    key: String,
    keys: Vec<String>,
    start_key: String,
    end_key: String,
    has_start_key: bool,
    has_end_key: bool,
    has_limit: bool,
    has_offset: bool,
    limit: u16,
    offset: u16,
}

#[derive(Serialize)]
struct CostSummary {
    seek_count: u32,
    storage_loaded_bytes: u64,
    hash_node_calls: u32,
    storage_added_bytes: u32,
    storage_replaced_bytes: u32,
    storage_removed: String,
}

impl OutputCorpusCase {
    fn from_case(case: CorpusCase) -> Self {
        let query_descriptor = descriptor_for_case(&case);
        OutputCorpusCase {
            name: case.name,
            path: case.path,
            keys: case.keys,
            element_bytes_list: case.element_bytes_list,
            item_bytes_list: case.item_bytes_list,
            query_descriptor,
            expect_present: case.expect_present,
            proof: case.proof,
            subtree_key: case.subtree_key,
            root_hash: case.root_hash,
            cost: case.cost,
        }
    }
}

fn encode_hex(bytes: &[u8]) -> String {
    hex::encode(bytes)
}

fn query_descriptor(
    kind: &str,
    key: Option<String>,
    keys: Vec<String>,
    start_key: Option<&[u8]>,
    end_key: Option<&[u8]>,
) -> QueryDescriptor {
    QueryDescriptor {
        kind: kind.to_string(),
        key: key.unwrap_or_default(),
        keys,
        start_key: start_key.map(encode_hex).unwrap_or_default(),
        end_key: end_key.map(encode_hex).unwrap_or_default(),
        has_start_key: start_key.is_some(),
        has_end_key: end_key.is_some(),
        has_limit: false,
        has_offset: false,
        limit: 0,
        offset: 0,
    }
}

fn descriptor_for_case(case: &CorpusCase) -> QueryDescriptor {
    match case.name.as_str() {
        "range_query"
        | "batch_range_query"
        | "sum_tree_item_with_sum_range"
        | "batch_sum_tree_item_with_sum_range"
        | "count_sum_tree_item_with_sum_range"
        | "batch_count_sum_tree_item_with_sum_range"
        | "big_sum_tree_item_with_sum_range"
        | "provable_count_sum_tree_item_with_sum_range"
        | "sum_tree_item_with_sum_nested_range"
        | "batch_sum_tree_item_with_sum_nested_range"
        | "nested_path_range_query" => query_descriptor(
            "range",
            None,
            Vec::new(),
            Some(b"b"),
            Some(b"d"),
        ),
        "range_absent" | "range_query_empty" => query_descriptor(
            "range",
            None,
            Vec::new(),
            Some(b"b"),
            Some(b"c"),
        ),
        "range_inclusive_query"
        | "provable_count_range_query"
        | "provable_count_nested_range_query" => query_descriptor(
            "range_inclusive",
            None,
            Vec::new(),
            Some(b"b"),
            Some(b"c"),
        ),
        "range_after_query" | "path_query_range_after_to" => query_descriptor(
            "range_after_to",
            None,
            Vec::new(),
            Some(b"b"),
            Some(b"d"),
        ),
        "path_query_range_to" => {
            query_descriptor("range_to", None, Vec::new(), None, Some(b"b"))
        }
        "path_query_range_after" => {
            query_descriptor("range_after", None, Vec::new(), Some(b"a"), None)
        }
        "path_query_range_to_inclusive" => {
            query_descriptor("range_to_inclusive", None, Vec::new(), None, Some(b"b"))
        }
        "path_query_range_after_to_inclusive" => query_descriptor(
            "range_after_to_inclusive",
            None,
            Vec::new(),
            Some(b"b"),
            Some(b"d"),
        ),
        "path_query_simple" => query_descriptor("range_full", None, Vec::new(), None, None),
        _ if case.keys.len() == 1 => query_descriptor(
            "single_key",
            Some(case.keys[0].clone()),
            Vec::new(),
            None,
            None,
        ),
        _ if case.name.contains("range") || case.name.starts_with("path_query_") => panic!(
            "descriptor_for_case: range/path_query case '{}' must be explicitly mapped",
            case.name
        ),
        _ if case.keys.len() > 1 => query_descriptor("key_set", None, case.keys.clone(), None, None),
        _ => panic!(
            "descriptor_for_case: unsupported/unmapped case '{}'; add explicit descriptor mapping",
            case.name
        ),
    }
}

fn encode_varint_u64(mut value: u64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let byte = (value & 0x7f) as u8;
        value >>= 7;
        if value == 0 {
            out.push(byte);
            return out;
        }
        out.push(byte | 0x80);
    }
}

fn zigzag_i64(value: i64) -> u64 {
    ((value as u64) << 1) ^ ((value >> 63) as u64)
}

fn encode_varint_i64(value: i64) -> Vec<u8> {
    encode_varint_u64(zigzag_i64(value))
}

fn summarize_cost(cost: &OperationCost) -> CostSummary {
    let removed = match &cost.storage_cost.removed_bytes {
        StorageRemovedBytes::NoStorageRemoval => "none".to_string(),
        StorageRemovedBytes::BasicStorageRemoval(bytes) => format!("basic:{bytes}"),
        StorageRemovedBytes::SectionedStorageRemoval(_) => "sectioned".to_string(),
    };

    CostSummary {
        seek_count: cost.seek_count,
        storage_loaded_bytes: cost.storage_loaded_bytes,
        hash_node_calls: cost.hash_node_calls,
        storage_added_bytes: cost.storage_cost.added_bytes,
        storage_replaced_bytes: cost.storage_cost.replaced_bytes,
        storage_removed: removed,
    }
}

fn subtree_key_from_proof(proof: &[u8]) -> Vec<u8> {
    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof, config)
            .expect("decode proof")
            .0;
    match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    }
}

fn build_item_case(name: &str, items: Vec<(&[u8], &[u8])>, query_key: &[u8]) -> CorpusCase {
    assert!(!items.is_empty(), "items must not be empty");
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"root";

    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    db.insert(
        parent_path.as_slice(),
        items[0].0,
        Element::new_item(items[0].1.to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");
    for (key, value) in items.iter().skip(1) {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item((*value).to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let path = vec![root_key.to_vec()];
    let query = PathQuery::new_single_key(path.clone(), query_key.to_vec());

    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    let mut element_bytes = None;
    for result in results {
        if result.path == path && result.key == query_key {
            element_bytes = Some(result.value);
            break;
        }
    }

    let element_bytes = element_bytes.expect("element bytes");
    let element = Element::deserialize(&element_bytes, grove_version)
        .expect("deserialize element");
    let item_bytes = match element {
        Element::Item(value, _) => value,
        _ => panic!("expected item element"),
    };

    CorpusCase {
        name: name.to_string(),
        path: vec![encode_hex(&path[0])],
        keys: vec![encode_hex(query_key)],
        element_bytes_list: vec![encode_hex(&element_bytes)],
        item_bytes_list: vec![encode_hex(&item_bytes)],
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_reference_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"ref_root";
    let target_root_key = b"target_root";
    let target_key = b"target";
    let ref_key = b"ref";

    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    db.insert(
        empty_path.as_slice(),
        target_root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target root tree");

    let target_path = vec![target_root_key.to_vec()];
    db.insert(
        target_path.as_slice(),
        target_key,
        Element::new_item(b"value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");

    let parent_path = vec![root_key.to_vec()];

    let reference = Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
        target_root_key.to_vec(),
        target_key.to_vec(),
    ]));
    db.insert(
        parent_path.as_slice(),
        ref_key,
        reference,
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert reference item");

    let path = vec![root_key.to_vec()];
    let query = PathQuery::new_single_key(path.clone(), ref_key.to_vec());

    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut element_bytes = None;
    let mut result_key = None;
    for result in results {
        if result.path == path {
            element_bytes = Some(result.value);
            result_key = Some(result.key);
            break;
        }
    }
    let element_bytes = element_bytes.expect("element bytes");
    let result_key = result_key.expect("result key");
    let element = Element::deserialize(&element_bytes, grove_version)
        .expect("deserialize element");
    let item_bytes = match element {
        Element::Item(value, _) => value,
        _ => panic!("expected item element"),
    };

    CorpusCase {
        name: "reference_single_key".to_string(),
        path: vec![encode_hex(&path[0])],
        keys: vec![encode_hex(&result_key)],
        element_bytes_list: vec![encode_hex(&element_bytes)],
        item_bytes_list: vec![encode_hex(&item_bytes)],
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_reference_chain_max_hop_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let ref_root_key = b"ref_root";
    let mid_root_key = b"mid_root";
    let target_root_key = b"target_root";
    let target_key = b"target";
    let mid_key = b"mid";
    let ref_key = b"ref";

    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        ref_root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref root tree");

    db.insert(
        empty_path.as_slice(),
        mid_root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert mid root tree");

    db.insert(
        empty_path.as_slice(),
        target_root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target root tree");

    let target_path = vec![target_root_key.to_vec()];
    db.insert(
        target_path.as_slice(),
        target_key,
        Element::new_item(b"value".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert target item");

    let mid_path = vec![mid_root_key.to_vec()];
    let mid_reference = Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
        target_root_key.to_vec(),
        target_key.to_vec(),
    ]));
    db.insert(
        mid_path.as_slice(),
        mid_key,
        mid_reference,
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert mid reference");

    let ref_path = vec![ref_root_key.to_vec()];
    let ref_reference = Element::new_reference_with_hops(
        ReferencePathType::AbsolutePathReference(vec![
            mid_root_key.to_vec(),
            mid_key.to_vec(),
        ]),
        Some(2),
    );
    db.insert(
        ref_path.as_slice(),
        ref_key,
        ref_reference,
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert ref reference");

    let path = vec![ref_root_key.to_vec()];
    let query = PathQuery::new_single_key(path.clone(), ref_key.to_vec());

    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut element_bytes = None;
    let mut result_key = None;
    for result in results {
        if result.path == path {
            element_bytes = Some(result.value);
            result_key = Some(result.key);
            break;
        }
    }
    let element_bytes = element_bytes.expect("element bytes");
    let result_key = result_key.expect("result key");
    let element = Element::deserialize(&element_bytes, grove_version)
        .expect("deserialize element");
    let item_bytes = match element {
        Element::Item(value, _) => value,
        _ => panic!("expected item element"),
    };

    CorpusCase {
        name: "reference_chain_max_hop".to_string(),
        path: vec![encode_hex(&path[0])],
        keys: vec![encode_hex(&result_key)],
        element_bytes_list: vec![encode_hex(&element_bytes)],
        item_bytes_list: vec![encode_hex(&item_bytes)],
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_batch_reference_chain_max_hop_case() -> CorpusCase {
    let mut base = build_reference_chain_max_hop_case();
    base.name = "batch_reference_chain_max_hop".to_string();
    base
}

fn build_single_tree_case(
    name: &str,
    root_key: &[u8],
    item_key: &[u8],
    tree_element: Element,
    item_element: Element,
    item_bytes: Vec<u8>,
) -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        tree_element,
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert tree");

    let parent_path = vec![root_key.to_vec()];
    db.insert(
        parent_path.as_slice(),
        item_key,
        item_element,
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");

    let path = vec![root_key.to_vec()];
    let query = PathQuery::new_single_key(path.clone(), item_key.to_vec());

    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut element_bytes = None;
    for result in results {
        if result.path == path && result.key == item_key {
            element_bytes = Some(result.value);
            break;
        }
    }
    let element_bytes = element_bytes.expect("element bytes");

    CorpusCase {
        name: name.to_string(),
        path: vec![encode_hex(&path[0])],
        keys: vec![encode_hex(item_key)],
        element_bytes_list: vec![encode_hex(&element_bytes)],
        item_bytes_list: vec![encode_hex(&item_bytes)],
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_single_key_case() -> CorpusCase {
    build_item_case("single_key_item", vec![(b"item", b"value")], b"item")
}

fn build_batch_single_key_case() -> CorpusCase {
    build_item_case("batch_single_key_item", vec![(b"item", b"value")], b"item")
}

fn build_multi_key_case() -> CorpusCase {
    build_item_case(
        "multi_key_item",
        vec![(b"a", b"va"), (b"b", b"vb"), (b"c", b"vc")],
        b"b",
    )
}

fn build_absent_key_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"root";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    db.insert(
        parent_path.as_slice(),
        b"a",
        Element::new_item(b"va".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");
    db.insert(
        parent_path.as_slice(),
        b"c",
        Element::new_item(b"vc".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");

    let query_key = b"b";
    let path = vec![root_key.to_vec()];
    let query = PathQuery::new_single_key(path.clone(), query_key.to_vec());

    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    if results.iter().any(|r| r.path == path && r.key == query_key) {
        panic!("expected missing key result");
    }

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    CorpusCase {
        name: "absent_key".to_string(),
        path: vec![encode_hex(&path[0])],
        keys: vec![encode_hex(query_key)],
        element_bytes_list: Vec::new(),
        item_bytes_list: Vec::new(),
        expect_present: false,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_sum_tree_case() -> CorpusCase {
    build_single_tree_case(
        "sum_tree_single_key",
        b"sumtree",
        b"sumkey",
        Element::new_sum_tree(None),
        Element::new_sum_item(7),
        encode_varint_i64(7),
    )
}

fn build_batch_sum_tree_case() -> CorpusCase {
    build_single_tree_case(
        "batch_sum_tree_single_key",
        b"sumtree",
        b"sumkey",
        Element::new_sum_tree(None),
        Element::new_sum_item(7),
        encode_varint_i64(7),
    )
}

fn build_batch_absent_key_case() -> CorpusCase {
    let base = build_absent_key_case();
    CorpusCase {
        name: "batch_absent_key".to_string(),
        ..base
    }
}

fn build_big_sum_tree_case() -> CorpusCase {
    build_single_tree_case(
        "big_sum_tree_single_key",
        b"bigsumtree",
        b"sumkey",
        Element::new_big_sum_tree(None),
        Element::new_sum_item(-12),
        encode_varint_i64(-12),
    )
}

fn build_count_tree_case() -> CorpusCase {
    build_single_tree_case(
        "count_tree_single_key",
        b"counttree",
        b"countkey",
        Element::new_count_tree(None),
        Element::new_item(b"va".to_vec()),
        b"va".to_vec(),
    )
}

fn build_count_sum_tree_case() -> CorpusCase {
    build_single_tree_case(
        "count_sum_tree_single_key",
        b"countsumtree",
        b"sumkey",
        Element::new_count_sum_tree(None),
        Element::new_sum_item(11),
        encode_varint_i64(11),
    )
}

fn build_provable_count_sum_tree_case() -> CorpusCase {
    build_single_tree_case(
        "provable_count_sum_tree_single_key",
        b"provcountsum",
        b"sumkey",
        Element::new_provable_count_sum_tree(None),
        Element::new_sum_item(4),
        encode_varint_i64(4),
    )
}

fn build_item_with_sum_case() -> CorpusCase {
    build_single_tree_case(
        "sum_tree_item_with_sum_single_key",
        b"sumwith",
        b"sumitem",
        Element::new_sum_tree(None),
        Element::new_item_with_sum_item(b"payload".to_vec(), 6),
        b"payload".to_vec(),
    )
}

fn build_item_with_sum_range_case(
    name: &str,
    root_key: &[u8],
    tree_element: Element,
) -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        tree_element,
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref(), 5),
        (b"b".as_ref(), b"vb".as_ref(), 6),
        (b"c".as_ref(), b"vc".as_ref(), 7),
        (b"d".as_ref(), b"vd".as_ref(), 8),
    ];
    for (key, value, sum) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item_with_sum_item((*value).to_vec(), *sum),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range(b"b".to_vec()..b"d".to_vec());
    let path = vec![root_key.to_vec()];
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::ItemWithSumItem(value, _, _) => value,
            _ => panic!("expected item-with-sum element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range results");
    }

    CorpusCase {
        name: name.to_string(),
        path: vec![encode_hex(&path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_sum_tree_item_with_sum_range_case() -> CorpusCase {
    build_item_with_sum_range_case(
        "sum_tree_item_with_sum_range",
        b"sumrange",
        Element::new_sum_tree(None),
    )
}

fn build_batch_sum_tree_item_with_sum_range_case() -> CorpusCase {
    build_item_with_sum_range_case(
        "batch_sum_tree_item_with_sum_range",
        b"sumrange",
        Element::new_sum_tree(None),
    )
}

fn build_count_sum_tree_item_with_sum_range_case() -> CorpusCase {
    build_item_with_sum_range_case(
        "count_sum_tree_item_with_sum_range",
        b"countsumrange",
        Element::new_count_sum_tree(None),
    )
}

fn build_batch_count_sum_tree_item_with_sum_range_case() -> CorpusCase {
    build_item_with_sum_range_case(
        "batch_count_sum_tree_item_with_sum_range",
        b"countsumrange",
        Element::new_count_sum_tree(None),
    )
}

fn build_big_sum_tree_item_with_sum_range_case() -> CorpusCase {
    build_item_with_sum_range_case(
        "big_sum_tree_item_with_sum_range",
        b"bigsumsumrange",
        Element::new_big_sum_tree(None),
    )
}

fn build_provable_count_sum_tree_item_with_sum_range_case() -> CorpusCase {
    build_item_with_sum_range_case(
        "provable_count_sum_tree_item_with_sum_range",
        b"provcountsumrange",
        Element::new_provable_count_sum_tree(None),
    )
}

fn build_sum_tree_item_with_sum_nested_range_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"sum_nested_root";
    let inner_key = b"sum_nested_inner";
    let leaf_key = b"sum_nested_leaf";

    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");
    db.insert(
        &[root_key.to_vec()],
        inner_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert inner tree");
    db.insert(
        &[root_key.to_vec(), inner_key.to_vec()],
        leaf_key,
        Element::new_sum_tree(None),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert leaf sum tree");

    let parent_path = vec![root_key.to_vec(), inner_key.to_vec(), leaf_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref(), 5),
        (b"b".as_ref(), b"vb".as_ref(), 6),
        (b"c".as_ref(), b"vc".as_ref(), 7),
        (b"d".as_ref(), b"vd".as_ref(), 8),
    ];
    for (key, value, sum) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item_with_sum_item((*value).to_vec(), *sum),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range(b"b".to_vec()..b"d".to_vec());
    let path = parent_path.clone();
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::ItemWithSumItem(value, _, _) => value,
            _ => panic!("expected item-with-sum element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected nested range results");
    }

    CorpusCase {
        name: "sum_tree_item_with_sum_nested_range".to_string(),
        path: vec![
            encode_hex(&root_key.to_vec()),
            encode_hex(&inner_key.to_vec()),
            encode_hex(&leaf_key.to_vec()),
        ],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_batch_sum_tree_item_with_sum_nested_range_case() -> CorpusCase {
    let mut base = build_sum_tree_item_with_sum_nested_range_case();
    base.name = "batch_sum_tree_item_with_sum_nested_range".to_string();
    base
}

fn build_provable_count_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"counts";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_provable_count_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert provable count tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
    ];
    for (key, value) in items {
        db.insert(
            parent_path.as_slice(),
            key,
            Element::new_item(value.to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let query_key = b"b";
    let path = vec![root_key.to_vec()];
    let query = PathQuery::new_single_key(path.clone(), query_key.to_vec());

    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    let mut element_bytes = None;
    for result in results {
        if result.path == path && result.key == query_key {
            element_bytes = Some(result.value);
            break;
        }
    }
    let element_bytes = element_bytes.expect("element bytes");
    let element = Element::deserialize(&element_bytes, grove_version)
        .expect("deserialize element");
    let item_bytes = match element {
        Element::Item(value, _) => value,
        _ => panic!("expected item element"),
    };

    CorpusCase {
        name: "provable_count_single_key".to_string(),
        path: vec![encode_hex(&path[0])],
        keys: vec![encode_hex(query_key)],
        element_bytes_list: vec![encode_hex(&element_bytes)],
        item_bytes_list: vec![encode_hex(&item_bytes)],
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_provable_count_range_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"counts_range";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_provable_count_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert provable count tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
    ];
    for (key, value) in items {
        db.insert(
            parent_path.as_slice(),
            key,
            Element::new_item(value.to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_inclusive(b"b".to_vec()..=b"c".to_vec());
    let path = vec![root_key.to_vec()];
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range results");
    }

    CorpusCase {
        name: "provable_count_range_query".to_string(),
        path: vec![encode_hex(&path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_range_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"range";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items {
        db.insert(
            parent_path.as_slice(),
            key,
            Element::new_item(value.to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range(b"b".to_vec()..b"d".to_vec());
    let path = vec![root_key.to_vec()];
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range results");
    }

    CorpusCase {
        name: "range_query".to_string(),
        path: vec![encode_hex(&path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_batch_range_case() -> CorpusCase {
    let mut base = build_range_case();
    base.name = "batch_range_query".to_string();
    base
}

fn build_range_after_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"range_after";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items {
        db.insert(
            parent_path.as_slice(),
            key,
            Element::new_item(value.to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_after_to(b"b".to_vec()..b"d".to_vec());
    let path = vec![root_key.to_vec()];
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.len() != 1 || keys[0] != encode_hex(b"c") {
        panic!("expected range after results");
    }

    CorpusCase {
        name: "range_after_query".to_string(),
        path: vec![encode_hex(&path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_range_absent_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"range_absent";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    db.insert(
        parent_path.as_slice(),
        b"a",
        Element::new_item(b"va".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");
    db.insert(
        parent_path.as_slice(),
        b"c",
        Element::new_item(b"vc".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");

    let mut query = Query::new();
    query.insert_range(b"b".to_vec()..b"c".to_vec());
    let path = vec![root_key.to_vec()];
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    if !results.is_empty() {
        panic!("expected empty range results");
    }

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    CorpusCase {
        name: "range_absent".to_string(),
        path: vec![encode_hex(&path[0])],
        keys: vec![encode_hex(b"b")],
        element_bytes_list: Vec::new(),
        item_bytes_list: Vec::new(),
        expect_present: false,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_range_empty_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"range_empty";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    db.insert(
        parent_path.as_slice(),
        b"a",
        Element::new_item(b"va".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");
    db.insert(
        parent_path.as_slice(),
        b"c",
        Element::new_item(b"vc".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");

    let mut query = Query::new();
    query.insert_range(b"b".to_vec()..b"c".to_vec());
    let path = vec![root_key.to_vec()];
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    if !results.is_empty() {
        panic!("expected empty range results");
    }

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.len() != 1 {
                panic!("expected exactly one lower layer, got {}", lower_layers.len());
            }
            let (key, _layer) = lower_layers
                .pop_first()
                .expect("missing lower layer entry");
            key
        }
    };

    CorpusCase {
        name: "range_query_empty".to_string(),
        path: vec![encode_hex(&path[0])],
        keys: Vec::new(),
        element_bytes_list: Vec::new(),
        item_bytes_list: Vec::new(),
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_path_query_simple_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"pq_simple";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item((*value).to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let query = Query::new_range_full();
    let path_query =
        PathQuery::new(parent_path.clone(), grovedb::SizedQuery::new(query, None, None));

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != parent_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected limit/offset results");
    }

    CorpusCase {
        name: "path_query_simple".to_string(),
        path: vec![encode_hex(&parent_path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_path_query_range_to_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"pq_to";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
    ];
    for (key, value) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item((*value).to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_to(..b"b".to_vec());
    let path_query = PathQuery::new_unsized(parent_path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != parent_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range to results");
    }

    CorpusCase {
        name: "path_query_range_to".to_string(),
        path: vec![encode_hex(&parent_path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_path_query_range_after_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"pq_after";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
    ];
    for (key, value) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item((*value).to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_after(b"a".to_vec()..);
    let path_query = PathQuery::new_unsized(parent_path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != parent_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range after results");
    }

    CorpusCase {
        name: "path_query_range_after".to_string(),
        path: vec![encode_hex(&parent_path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_path_query_range_to_inclusive_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"pq_toi";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
    ];
    for (key, value) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item((*value).to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_to_inclusive(..=b"b".to_vec());
    let path_query = PathQuery::new_unsized(parent_path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != parent_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range to inclusive results");
    }

    CorpusCase {
        name: "path_query_range_to_inclusive".to_string(),
        path: vec![encode_hex(&parent_path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_path_query_range_after_to_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"pq_aft";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item((*value).to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_after_to(b"b".to_vec()..b"d".to_vec());
    let path_query = PathQuery::new_unsized(parent_path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != parent_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range after to results");
    }

    CorpusCase {
        name: "path_query_range_after_to".to_string(),
        path: vec![encode_hex(&parent_path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_path_query_range_after_to_inclusive_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"pq_afti";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items.iter() {
        db.insert(
            parent_path.as_slice(),
            *key,
            Element::new_item((*value).to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_after_to_inclusive(b"b".to_vec()..=b"d".to_vec());
    let path_query = PathQuery::new_unsized(parent_path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let subtree_key = subtree_key_from_proof(&proof);

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != parent_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range after to inclusive results");
    }

    CorpusCase {
        name: "path_query_range_after_to_inclusive".to_string(),
        path: vec![encode_hex(&parent_path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_nested_path_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"root";
    let inner_key = b"inner";
    let leaf_key = b"leaf";
    let item_key = b"item";
    let item_value = b"value";
    let empty_path: Vec<Vec<u8>> = Vec::new();

    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let root_path = vec![root_key.to_vec()];
    db.insert(
        root_path.as_slice(),
        inner_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert inner tree");

    let inner_path = vec![root_key.to_vec(), inner_key.to_vec()];
    db.insert(
        inner_path.as_slice(),
        leaf_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert leaf tree");

    let leaf_path = vec![root_key.to_vec(), inner_key.to_vec(), leaf_key.to_vec()];
    db.insert(
        leaf_path.as_slice(),
        item_key,
        Element::new_item(item_value.to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert item");

    let query = PathQuery::new_single_key(leaf_path.clone(), item_key.to_vec());

    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.len() != 1 {
                panic!("expected exactly one lower layer, got {}", lower_layers.len());
            }
            let (key, _layer) = lower_layers
                .pop_first()
                .expect("missing lower layer entry");
            key
        }
    };

    let mut element_bytes = None;
    for result in results {
        if result.path == leaf_path && result.key == item_key {
            element_bytes = Some(result.value);
            break;
        }
    }
    let element_bytes = element_bytes.expect("element bytes");
    let element = Element::deserialize(&element_bytes, grove_version)
        .expect("deserialize element");
    let item_bytes = match element {
        Element::Item(value, _) => value,
        _ => panic!("expected item element"),
    };

    CorpusCase {
        name: "nested_path_single_key".to_string(),
        path: vec![
            encode_hex(&root_key.to_vec()),
            encode_hex(&inner_key.to_vec()),
            encode_hex(&leaf_key.to_vec()),
        ],
        keys: vec![encode_hex(item_key)],
        element_bytes_list: vec![encode_hex(&element_bytes)],
        item_bytes_list: vec![encode_hex(&item_bytes)],
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_range_inclusive_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"range_inclusive";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let parent_path = vec![root_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items {
        db.insert(
            parent_path.as_slice(),
            key,
            Element::new_item(value.to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_inclusive(b"b".to_vec()..=b"c".to_vec());
    let path = vec![root_key.to_vec()];
    let path_query = PathQuery::new_unsized(path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.len() != 1 {
                panic!("expected exactly one lower layer, got {}", lower_layers.len());
            }
            let (key, _layer) = lower_layers
                .pop_first()
                .expect("missing lower layer entry");
            key
        }
    };

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected range inclusive results");
    }

    CorpusCase {
        name: "range_inclusive_query".to_string(),
        path: vec![encode_hex(&path[0])],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_nested_range_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"root";
    let inner_key = b"inner";
    let leaf_key = b"leaf";
    let empty_path: Vec<Vec<u8>> = Vec::new();

    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let root_path = vec![root_key.to_vec()];
    db.insert(
        root_path.as_slice(),
        inner_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert inner tree");

    let inner_path = vec![root_key.to_vec(), inner_key.to_vec()];
    db.insert(
        inner_path.as_slice(),
        leaf_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert leaf tree");

    let leaf_path = vec![root_key.to_vec(), inner_key.to_vec(), leaf_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items {
        db.insert(
            leaf_path.as_slice(),
            key,
            Element::new_item(value.to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range(b"b".to_vec()..b"d".to_vec());
    let path_query = PathQuery::new_unsized(leaf_path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.len() != 1 {
                panic!("expected exactly one lower layer, got {}", lower_layers.len());
            }
            let (key, _layer) = lower_layers
                .pop_first()
                .expect("missing lower layer entry");
            key
        }
    };

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != leaf_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected nested range results");
    }

    CorpusCase {
        name: "nested_path_range_query".to_string(),
        path: vec![
            encode_hex(&root_key.to_vec()),
            encode_hex(&inner_key.to_vec()),
            encode_hex(&leaf_key.to_vec()),
        ],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_delete_tree_absent_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"delete_root";
    let inner_key = b"delete_inner";
    let leaf_key = b"delete_leaf";
    let empty_path: Vec<Vec<u8>> = Vec::new();

    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let root_path = vec![root_key.to_vec()];
    db.insert(
        root_path.as_slice(),
        inner_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert inner tree");

    let inner_path = vec![root_key.to_vec(), inner_key.to_vec()];
    db.insert(
        inner_path.as_slice(),
        leaf_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert leaf tree");

    let leaf_path = vec![root_key.to_vec(), inner_key.to_vec(), leaf_key.to_vec()];
    db.insert(
        leaf_path.as_slice(),
        b"a",
        Element::new_item(b"va".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert leaf item");

    let delete_options = DeleteOptions {
        allow_deleting_non_empty_trees: true,
        deleting_non_empty_trees_returns_error: false,
        base_root_storage_is_free: true,
        validate_tree_at_path_exists: false,
    };
    db.delete(
        root_path.as_slice(),
        inner_key,
        Some(delete_options),
        None,
        grove_version,
    )
    .unwrap()
    .expect("delete tree");

    let query = PathQuery::new_single_key(empty_path.clone(), inner_key.to_vec());
    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    if results
        .iter()
        .any(|r| r.path == empty_path && r.key == inner_key)
    {
        panic!("expected missing inner tree result");
    }

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    CorpusCase {
        name: "delete_tree_absent".to_string(),
        path: Vec::new(),
        keys: vec![encode_hex(inner_key)],
        element_bytes_list: Vec::new(),
        item_bytes_list: Vec::new(),
        expect_present: false,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_batch_delete_tree_absent_case() -> CorpusCase {
    let mut base = build_delete_tree_absent_case();
    base.name = "batch_delete_tree_absent".to_string();
    base
}

fn build_delete_pc_tree_absent_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let outer_key = b"delete_pc_outer";
    let root_key = b"delete_pc_root";
    let other_key = b"delete_pc_other";
    let empty_path: Vec<Vec<u8>> = Vec::new();
    db.insert(
        empty_path.as_slice(),
        outer_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert pc outer tree");
    let outer_path = vec![outer_key.to_vec()];
    db.insert(
        outer_path.as_slice(),
        root_key,
        Element::empty_provable_count_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert pc root tree");
    db.insert(
        outer_path.as_slice(),
        other_key,
        Element::new_item(b"keep".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert pc other item");

    let root_path = vec![outer_key.to_vec(), root_key.to_vec()];
    db.insert(
        root_path.as_slice(),
        b"a",
        Element::new_item(b"va".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert pc item");

    let delete_options = DeleteOptions {
        allow_deleting_non_empty_trees: true,
        deleting_non_empty_trees_returns_error: false,
        base_root_storage_is_free: true,
        validate_tree_at_path_exists: false,
    };
    db.delete(
        outer_path.as_slice(),
        root_key,
        Some(delete_options),
        None,
        grove_version,
    )
    .unwrap()
    .expect("delete pc tree");

    let query = PathQuery::new_single_key(outer_path.clone(), root_key.to_vec());
    let proof_with_cost = db.prove_query(&query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
            .expect("verify proof");

    if results.iter().any(|r| r.path == outer_path && r.key == root_key) {
        panic!("expected missing pc root result");
    }

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.is_empty() {
                Vec::new()
            } else {
                if lower_layers.len() != 1 {
                    panic!("expected exactly one lower layer, got {}", lower_layers.len());
                }
                let (key, _layer) = lower_layers
                    .pop_first()
                    .expect("missing lower layer entry");
                key
            }
        }
    };

    CorpusCase {
        name: "delete_pc_tree_absent".to_string(),
        path: vec![encode_hex(&outer_key.to_vec())],
        keys: vec![encode_hex(root_key)],
        element_bytes_list: Vec::new(),
        item_bytes_list: Vec::new(),
        expect_present: false,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn build_provable_count_nested_range_case() -> CorpusCase {
    let grove_version = GroveVersion::latest();
    let temp_dir = TempDir::new().expect("tempdir");
    let db = GroveDb::open(temp_dir.path()).expect("open grovedb");

    let root_key = b"root_pc";
    let inner_key = b"inner";
    let leaf_key = b"leaf_pc";
    let empty_path: Vec<Vec<u8>> = Vec::new();

    db.insert(
        empty_path.as_slice(),
        root_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert root tree");

    let root_path = vec![root_key.to_vec()];
    db.insert(
        root_path.as_slice(),
        inner_key,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert inner tree");

    let inner_path = vec![root_key.to_vec(), inner_key.to_vec()];
    db.insert(
        inner_path.as_slice(),
        leaf_key,
        Element::empty_provable_count_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("insert provable count leaf");

    let leaf_path = vec![root_key.to_vec(), inner_key.to_vec(), leaf_key.to_vec()];
    let items = vec![
        (b"a".as_ref(), b"va".as_ref()),
        (b"b".as_ref(), b"vb".as_ref()),
        (b"c".as_ref(), b"vc".as_ref()),
        (b"d".as_ref(), b"vd".as_ref()),
    ];
    for (key, value) in items {
        db.insert(
            leaf_path.as_slice(),
            key,
            Element::new_item(value.to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item");
    }

    let mut query = Query::new();
    query.insert_range_inclusive(b"b".to_vec()..=b"c".to_vec());
    let path_query = PathQuery::new_unsized(leaf_path.clone(), query);

    let proof_with_cost = db.prove_query(&path_query, None, grove_version);
    let cost = proof_with_cost.cost.clone();
    let proof = proof_with_cost.value.expect("proof result");

    let (root_hash, results) =
        GroveDb::verify_query_raw(proof.as_slice(), &path_query, grove_version)
            .expect("verify proof");

    let config = bincode::config::standard()
        .with_big_endian()
        .with_no_limit();
    let grovedb_proof: grovedb::operations::proof::GroveDBProof =
        bincode::decode_from_slice(proof.as_slice(), config)
            .expect("decode proof")
            .0;
    let subtree_key = match grovedb_proof {
        grovedb::operations::proof::GroveDBProof::V0(proof_v0) => {
            let root_layer = proof_v0.root_layer;
            let mut lower_layers = root_layer.lower_layers;
            if lower_layers.len() != 1 {
                panic!("expected exactly one lower layer, got {}", lower_layers.len());
            }
            let (key, _layer) = lower_layers
                .pop_first()
                .expect("missing lower layer entry");
            key
        }
    };

    let mut keys = Vec::new();
    let mut element_bytes_list = Vec::new();
    let mut item_bytes_list = Vec::new();
    for result in results {
        if result.path != leaf_path {
            continue;
        }
        let element = Element::deserialize(&result.value, grove_version)
            .expect("deserialize element");
        let item_bytes = match element {
            Element::Item(value, _) => value,
            _ => panic!("expected item element"),
        };
        keys.push(encode_hex(&result.key));
        element_bytes_list.push(encode_hex(&result.value));
        item_bytes_list.push(encode_hex(&item_bytes));
    }
    if keys.is_empty() {
        panic!("expected nested provable count results");
    }

    CorpusCase {
        name: "provable_count_nested_range_query".to_string(),
        path: vec![
            encode_hex(&root_key.to_vec()),
            encode_hex(&inner_key.to_vec()),
            encode_hex(&leaf_key.to_vec()),
        ],
        keys,
        element_bytes_list,
        item_bytes_list,
        expect_present: true,
        proof: encode_hex(&proof),
        subtree_key: encode_hex(&subtree_key),
        root_hash: encode_hex(&root_hash),
        cost: summarize_cost(&cost),
    }
}

fn main() {
    let output_path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("cpp/corpus/corpus.json"));

    let corpus = Corpus {
        version: "v1".to_string(),
        cases: vec![
            build_single_key_case(),
            build_batch_single_key_case(),
            build_multi_key_case(),
            build_absent_key_case(),
            build_batch_absent_key_case(),
            build_sum_tree_case(),
            build_batch_sum_tree_case(),
            build_big_sum_tree_case(),
            build_count_tree_case(),
            build_count_sum_tree_case(),
            build_provable_count_sum_tree_case(),
            build_item_with_sum_case(),
            build_sum_tree_item_with_sum_range_case(),
            build_batch_sum_tree_item_with_sum_range_case(),
            build_count_sum_tree_item_with_sum_range_case(),
            build_batch_count_sum_tree_item_with_sum_range_case(),
            build_big_sum_tree_item_with_sum_range_case(),
            build_provable_count_sum_tree_item_with_sum_range_case(),
            build_sum_tree_item_with_sum_nested_range_case(),
            build_batch_sum_tree_item_with_sum_nested_range_case(),
            build_provable_count_case(),
            build_provable_count_range_case(),
            build_range_case(),
            build_batch_range_case(),
        build_range_after_case(),
        build_range_absent_case(),
        build_range_empty_case(),
        build_path_query_simple_case(),
        build_path_query_range_to_case(),
        build_path_query_range_after_case(),
        build_path_query_range_to_inclusive_case(),
        build_path_query_range_after_to_case(),
        build_path_query_range_after_to_inclusive_case(),
        build_nested_path_case(),
            build_range_inclusive_case(),
            build_nested_range_case(),
            build_provable_count_nested_range_case(),
            build_delete_tree_absent_case(),
            build_batch_delete_tree_absent_case(),
            build_delete_pc_tree_absent_case(),
            build_reference_case(),
            build_reference_chain_max_hop_case(),
            build_batch_reference_chain_max_hop_case(),
        ],
    };

    let output = OutputCorpus {
        version: corpus.version,
        cases: corpus
            .cases
            .into_iter()
            .map(OutputCorpusCase::from_case)
            .collect(),
    };

    let json = serde_json::to_string_pretty(&output).expect("serialize corpus");
    fs::write(&output_path, json).expect("write corpus");

    println!("wrote corpus to {}", output_path.display());
}
