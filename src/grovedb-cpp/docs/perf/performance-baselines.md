# Performance Baselines (GroveDB v4.0.0 + MerkCache)

This file stores the active baseline set for Phase 6 performance checks.

## Active Baseline Run
- Date: 2026-03-14 (security fixes refresh)
- Host: Darwin MacBook-Pro.local arm64 (Darwin Kernel 25.3.0)
- Build type: Release CMake build (`build-release`) + Rust `cargo bench` optimized profile
- Samples: 10
- Warmup: 1s
- Measurement time: 2s (Rust Criterion)
- Corpus minimum iterations per sample (C++): 1000
- Corpus samples (C++): 7 (median reported)
- Corpus minimum sample time (C++): 100ms
- Items: 10000 (scalars/nested), 10 (root leaves)
- Raw outputs:
  - `docs/perf/raw/cpp_insertion_bench_20260314T152502Z.txt`
  - `docs/perf/raw/cpp_insertion_storage_bench_20260314T152604Z.txt`
  - `docs/perf/raw/cpp_corpus_bench_20260314T152639Z.txt`
  - `docs/perf/raw/rust_insertion_bench_20260309T231432Z.txt`

## Derived Data Sources
- Rust insertion medians: `docs/perf/raw/rust_insertion_bench_20260309T231432Z.txt`
- C++ insertion medians (facade): `docs/perf/raw/cpp_insertion_bench_20260314T152502Z.txt`
- Rust corpus medians: `tools/corpus/target/criterion/corpus_proof_*/new/estimates.json`
- C++ corpus medians: `docs/perf/raw/cpp_corpus_bench_20260314T152639Z.txt`

## Deltas
- Corpus proof deltas: `docs/perf/corpus_proof_deltas.md`
- Insertion deltas: `docs/perf/insertion_deltas.md`

## Changes from Previous Baseline (2026-03-10 insert cache reuse + tx batch apply refresh)
- Refreshed C++ insertion baseline from `cpp_insertion_bench_20260314T152502Z.txt`.
- Refreshed C++ storage microbenchmark diagnostic baseline from `cpp_insertion_storage_bench_20260314T152604Z.txt`.
- Refreshed C++ corpus baseline from `cpp_corpus_bench_20260314T152639Z.txt`.
- Reused Rust insertion medians from `rust_insertion_bench_20260309T231432Z.txt`.
- Insertion parity remains below threshold for all six cases; highest current ratio is `0.88x` (deeply nested no-tx).
- Corpus proof parity remains within policy; max current ratio is `0.73x` (path_query_simple).

## Notes
- Keep raw runs in `docs/perf/raw/`.
- Keep this file aligned with the latest baseline artifact used by both delta reports.
- `GROVEDB_BENCH_INSERT_ONLY` is useful for isolated-case measurement/repro and debugging; full-run and isolated-run deltas can differ by environment.
- Storage microbenchmark output (`bench_insertion_storage`) is diagnostic and is not used for Rust-vs-C++ GroveDB insertion parity deltas.
