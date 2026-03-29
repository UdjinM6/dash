# Insertion Benchmark (Repo-local Rust)

Runs Rust GroveDB insertion benchmarks from this repository without requiring a sibling Rust workspace checkout.

## Command

From repo root:

```bash
bash tools/run_rust_insertion_bench.sh
```

Optional parameters:

```bash
bash tools/run_rust_insertion_bench.sh <warmup_s> <measurement_s> <sample_size>
```

Defaults are aligned with `docs/perf/methodology-lock.md`:
- warmup: `1`
- measurement: `2`
- sample size: `10`

Raw output is written to `docs/perf/raw/rust_insertion_bench_<timestamp>.txt`.
