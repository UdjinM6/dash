## Insertion Benchmark Deltas (C++ vs Rust)

### Latest Run (2026-03-14, Security Fixes Refresh - Methodology-Lock Params)
- Source: `docs/perf/raw/cpp_insertion_bench_20260314T152502Z.txt` vs `docs/perf/raw/rust_insertion_bench_20260309T231432Z.txt`
- Metric: ns/op per inserted item (Rust normalized per item)
- Note: C++ bench uses `GroveDb` facade insertion path with 10000 items (10 for root leaves); Rust uses GroveDB insertion Criterion benches.
- Note: Rust ns/op is derived from benchmark median `time` divided by item count.

case | cpp_ns_op | rust_ns_op | cpp/rust
---|---:|---:|---:
deeply nested scalars insertion with transaction | 122372.5 | 154910.000 | 0.79
deeply nested scalars insertion with transaction | 125594.9 | 154910.000 | 0.81
deeply nested scalars insertion without transaction | 146012.3 | 166840.000 | 0.88
root leaves insertion with transaction | 36081.3 | 81226.000 | 0.44
root leaves insertion without transaction | 36314.6 | 89328.000 | 0.41
scalars insertion with transaction | 70115.3 | 99939.000 | 0.70
scalars insertion without transaction | 85109.7 | 101730.000 | 0.84

### Delta Change (2026-03-10 Snapshot vs 2026-03-14 Security Fixes Refresh)
- Results are refreshed using the locked C++ insertion parameters (`10000 10 1 1`) from `docs/perf/methodology-lock.md`
- All insertion cases remain under the current `1.1` threshold policy

case | cpp/rust (prev) | cpp/rust (new) | change
---|---:|---:|---:
deeply nested scalars insertion with transaction | 0.79 | 0.81 | +2.6%
deeply nested scalars insertion without transaction | 0.86 | 0.88 | +2.2%
root leaves insertion with transaction | 0.48 | 0.44 | -6.5%
root leaves insertion without transaction | 0.38 | 0.41 | +7.4%
scalars insertion with transaction | 0.67 | 0.70 | +4.4%
scalars insertion without transaction | 0.82 | 0.84 | +2.6%

### Summary
- Relative to Rust: C++ remains at or below Rust in all six insertion targets (`0.88x` max).
- Largest regression in this refresh is root leaves insertion without transaction, rising by `+7.4%`.
- Largest improvement in this refresh is root leaves insertion with transaction, improving by `-6.5%`.
- Threshold impact: no insertion case currently exceeds the `1.1` regression threshold.
