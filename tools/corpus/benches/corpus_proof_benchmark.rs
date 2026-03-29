use std::{fs, hint::black_box, path::PathBuf};

use criterion::{criterion_group, criterion_main, Criterion};
use grovedb::{GroveDb, PathQuery, Query, SizedQuery};
use grovedb_version::version::GroveVersion;
use serde::Deserialize;

#[derive(Deserialize)]
struct Corpus {
    cases: Vec<CorpusCase>,
}

#[derive(Deserialize)]
struct CorpusCase {
    name: String,
    path: Vec<String>,
    query_descriptor: QueryDescriptor,
    proof: String,
}

#[derive(Deserialize)]
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

struct PreparedCase {
    name: String,
    proof: Vec<u8>,
    query: PathQuery,
}

fn decode_hex(hex: &str) -> Vec<u8> {
    hex::decode(hex).unwrap_or_else(|e| panic!("failed to decode hex: {e}"))
}

fn corpus_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
        .join("corpus")
        .join("corpus.json")
}

fn build_query(case: &CorpusCase) -> PathQuery {
    let path = case.path.iter().map(|v| decode_hex(v)).collect::<Vec<_>>();
    let descriptor = &case.query_descriptor;
    match descriptor.kind.as_str() {
        "single_key" => PathQuery::new_single_key(path, decode_hex(&descriptor.key)),
        "key_set" => {
            let mut query = Query::new();
            for key in &descriptor.keys {
                query.insert_key(decode_hex(key));
            }
            PathQuery::new_unsized(path, query)
        }
        kind => {
            let mut query = Query::new();
            let start = if descriptor.has_start_key {
                Some(decode_hex(&descriptor.start_key))
            } else {
                None
            };
            let end = if descriptor.has_end_key {
                Some(decode_hex(&descriptor.end_key))
            } else {
                None
            };
            match kind {
                "range" => {
                    query.insert_range(start.expect("missing start")..end.expect("missing end"));
                }
                "range_inclusive" => {
                    query.insert_range_inclusive(
                        start.expect("missing start")..=end.expect("missing end"),
                    );
                }
                "range_full" => {
                    query = Query::new_range_full();
                }
                "range_from" => {
                    query.insert_range_from(start.expect("missing start")..);
                }
                "range_to" => {
                    query.insert_range_to(..end.expect("missing end"));
                }
                "range_to_inclusive" => {
                    query.insert_range_to_inclusive(..=end.expect("missing end"));
                }
                "range_after" => {
                    query.insert_range_after(start.expect("missing start")..);
                }
                "range_after_to" => {
                    query.insert_range_after_to(start.expect("missing start")..end.expect("missing end"));
                }
                "range_after_to_inclusive" => {
                    query.insert_range_after_to_inclusive(
                        start.expect("missing start")..=end.expect("missing end"),
                    );
                }
                _ => panic!("unsupported query descriptor kind: {kind}"),
            }
            if descriptor.has_limit || descriptor.has_offset {
                PathQuery::new(
                    path,
                    SizedQuery::new(
                        query,
                        descriptor.has_limit.then_some(descriptor.limit),
                        descriptor.has_offset.then_some(descriptor.offset),
                    ),
                )
            } else {
                PathQuery::new_unsized(path, query)
            }
        }
    }
}

fn load_cases() -> Vec<PreparedCase> {
    let payload = fs::read_to_string(corpus_path()).expect("failed to read corpus.json");
    let corpus: Corpus = serde_json::from_str(&payload).expect("failed to parse corpus.json");
    corpus
        .cases
        .iter()
        .map(|case| PreparedCase {
            name: case.name.clone(),
            proof: decode_hex(&case.proof),
            query: build_query(case),
        })
        .collect()
}

fn corpus_proof_benchmark(c: &mut Criterion) {
    let cases = load_cases();
    let grove_version = GroveVersion::latest();
    for case in &cases {
        let bench_name = format!("corpus_proof_{}", case.name);
        let proof = case.proof.clone();
        let query = case.query.clone();
        c.bench_function(&bench_name, move |b| {
            b.iter(|| {
                let verified = GroveDb::verify_query_raw(proof.as_slice(), &query, grove_version)
                    .is_ok();
                assert!(verified, "proof verification failed");
                black_box(verified);
            });
        });
    }
}

criterion_group!(benches, corpus_proof_benchmark);
criterion_main!(benches);
