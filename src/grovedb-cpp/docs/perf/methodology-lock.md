# C++/Rust Benchmark Methodology Lock

This file defines the shared benchmark parameters for Rust and C++ parity runs.

## Scope
- GroveDB facade insertion workload (primary parity metric)
- Storage insertion microbenchmark (diagnostic-only)
- Proof workload (corpus/path-proof verification)

## Locked Parameters
- Rust benchmark crate: `grovedb`
- Rust feature set: default benchmark setup in `grovedb/benches/*`
- C++ binaries: `bench_insertion` (facade), `bench_insertion_storage` (microbench), `bench_corpus`
- Insertion item count: `10000`
- Sample count: `10`
- Warmup samples/time:
  - Rust: `--warm-up-time 1 --measurement-time 2`
  - C++: `warmup_samples=1`
- Plot output: disabled for Rust (`--noplot`)
- C++ insertion reset mode: `reset_per_sample=1` (fresh DB per sample)
- Corpus minimum iterations per sample (C++): `1000`
- Corpus samples (C++): `7` (median reported)
- Corpus minimum sample time (C++): `100ms`

## Canonical Runners
- `tools/run_cpp_insertion_bench.sh`
- `tools/run_cpp_corpus_bench.sh`
- `tools/run_rust_insertion_bench.sh` (repo-local Rust insertion bench via `tools/insertion`)
- Rust corpus proof bench via
  `cargo bench --manifest-path tools/corpus/Cargo.toml --bench corpus_proof_benchmark`
  with `CARGO_TARGET_DIR=tools/corpus/target` and locked params.

## Clarification
- `bench_insertion` now measures C++ `GroveDb` facade inserts (not direct `RocksDbWrapper::Put`).
- `bench_insertion_storage` is retained only for storage-level diagnostics and should not be used for Rust-vs-C++ GroveDB parity claims.
