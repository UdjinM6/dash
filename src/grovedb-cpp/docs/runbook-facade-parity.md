# Facade Parity and Perf Runbook

## Prerequisites
- C++ build dependencies installed (including RocksDB).
- Rust toolchain available for Rust-backed parity and corpus benches.
- Build directories:
  - `build` (tests/parity)
  - `build-release` (benchmarks)
- Repo-local Rust insertion bench available via `tools/insertion`.

## 1) Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cpu_count="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
jobs="$(( cpu_count > 1 ? cpu_count - 1 : 1 ))"
cmake --build build -j"${jobs}"
cmake --build build-release -j"${jobs}"
```

## 2) Core Test Gates
```bash
cd build
ctest --output-on-failure -j"${jobs}"
```

## 3) Rust-backed Parity Gates
`tools/run_parity_ctest.sh` configures writable Cargo cache defaults under `/tmp` when needed.
```bash
bash tools/check_rust_parity_commit_pin.sh
bash tools/run_parity_ctest.sh all
```

## 4) Benchmark Refresh (Locked Parameters)
Parameters are defined in `docs/perf/methodology-lock.md`.

### C++ facade insertion benchmark (primary)
```bash
bash tools/run_cpp_insertion_bench.sh 10000 10 1 1 build-release
```

### C++ storage insertion benchmark (diagnostic only)
```bash
bash tools/run_cpp_insertion_storage_bench.sh 10000 10 1 1 build-release
```

### C++ corpus proof benchmark
```bash
bash tools/run_cpp_corpus_bench.sh 1000 corpus/corpus.json build-release 7 100
```

### Rust corpus proof benchmark (for corpus deltas)
```bash
CARGO_TARGET_DIR=tools/corpus/target \
cargo bench --manifest-path tools/corpus/Cargo.toml \
  --bench corpus_proof_benchmark -- --warm-up-time 1 --measurement-time 2 --sample-size 10 --noplot
```

### Rust insertion benchmark (repo-local)
```bash
bash tools/run_rust_insertion_bench.sh 1 2 10
```

## 5) Update Perf Docs
After runs, refresh:
- `docs/perf/insertion_deltas.md`
- `docs/perf/corpus_proof_deltas.md`
- `docs/perf/performance-baselines.md`
- `docs/perf/regression-thresholds.json` (only if policy intent changed)

Keep only current raw artifacts in `docs/perf/raw/`.

## 6) Policy Checks
```bash
bash tools/check_perf_regression_thresholds.sh
bash tools/check_accepted_divergence_expiry.sh
bash tools/check_error_string_policy.sh
bash tools/check_facade_parity_mode_coverage.sh
bash tools/run_fixture_regen_check.sh
```
