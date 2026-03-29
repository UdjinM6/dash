# Corpus Builder

Generates a golden corpus from the Rust implementation for deterministic regeneration checks.

## Build

From repo root:

```bash
cargo run --manifest-path tools/corpus/Cargo.toml
```

## Output

Writes `corpus/corpus.json` by default. Pass a custom path as the first argument.

## Corpus Proof Benchmark

Runs Rust proof verification medians consumed by C++/Rust corpus delta reporting:

```bash
CARGO_TARGET_DIR=tools/corpus/target \
cargo bench --manifest-path tools/corpus/Cargo.toml \
  --bench corpus_proof_benchmark -- --warm-up-time 1 --measurement-time 2 --noplot
```

Criterion outputs are written under `tools/corpus/target/criterion/corpus_proof_*/new/estimates.json`.
