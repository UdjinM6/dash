# Canonical Corpus and Regression Gate (GroveDB v4.0.0)

## Canonical Corpus Source
- Canonical corpus file: `corpus/corpus.json`.
- The corpus is generated from Rust fixtures via `tools/corpus` and consumed by C++ corpus tests/benchmarks.
- Corpus changes are explicit and reviewable (no implicit regeneration during normal test runs).

## Regression Gate
- `tests/corpus_tests.cpp` must pass against the canonical corpus.
- Rust-backed proof parity suites using corpus-derived fixtures must pass.
- Any unexpected proof byte/result mismatch vs canonical fixtures is a hard failure.
- Performance policy derived from corpus proof deltas must pass:
  - `bash tools/check_perf_regression_thresholds.sh`

## Update Workflow
- If corpus behavior intentionally changes:
  1. Regenerate/update corpus and related fixtures.
  2. Re-run corpus tests/parity/benchmarks.
  3. Refresh perf deltas/baselines in `docs/perf/*`.
  4. Document rationale in the commit message and affected docs.

## Parity Pin
- Rust parity target is pinned in `RUST_PARITY_COMMIT`.
- CI enforces parity-pin consistency via `tools/check_rust_parity_commit_pin.sh`.

## Maintenance
- Keep the corpus minimal but representative.
- Add cases only for new edge cases or regressions.
