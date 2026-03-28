# Rewrite Acceptance Criteria (GroveDB v4.0.0)

These criteria define objective cutover readiness for `grovedb-cpp` against Rust GroveDB `v4.0.0`.

## 1) Build and Test Gates
- CMake build succeeds for `build` and `build-release`.
- Core test gate passes:
  - `cd build && ctest --output-on-failure`
- Rust-backed parity gate passes:
  - `bash tools/run_parity_ctest.sh all`

## 2) Functional Parity
- Proof/query verification behavior matches Rust for covered fixtures and parity suites.
- Storage and transaction behavior matches Rust for covered contract/parity modes.
- Reference resolution semantics (including hop limits) match Rust for covered modes.
- Version-gated APIs reject unsupported versions and delegate correctly for supported versions.

## 3) Cost and Policy Gates
- Cost-shape parity tests pass for covered storage/merk/facade paths.
- Policy scripts pass:
  - `bash tools/check_error_string_policy.sh`
  - `bash tools/check_accepted_divergence_expiry.sh`
  - `bash tools/check_facade_parity_mode_coverage.sh`
  - `bash tools/check_rust_parity_commit_pin.sh`

## 4) Performance Gate
- Benchmarks and deltas are refreshed using locked methodology in `docs/perf/methodology-lock.md`.
- Threshold policy passes:
  - `bash tools/check_perf_regression_thresholds.sh`
- Baseline/delta docs are updated and internally consistent:
  - `docs/perf/performance-baselines.md`
  - `docs/perf/insertion_deltas.md`
  - `docs/perf/corpus_proof_deltas.md`
  - `docs/perf/regression-thresholds.json`

## 5) Documentation Gate
- Living contracts/specs are present and current:
  - `docs/compat-invariants.md`
  - `docs/accepted-divergences.md`
  - `docs/error-string-parity-policy.md`
  - `docs/grovedb-facade-spec.md`
  - `docs/rust-cpp-feature-matrix.md`
