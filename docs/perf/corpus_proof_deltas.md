## Corpus Proof Verification Deltas (C++ vs Rust)
- Source: `docs/perf/raw/cpp_corpus_bench_20260314T152639Z.txt` + `tools/corpus/target/criterion/corpus_proof_*/new/estimates.json`
- Metric: ns/op (C++ corpus verifier) vs Rust `corpus_proof_*` Criterion medians
- Cases: 43
- Mean cpp/rust: 0.63
- Median cpp/rust: 0.65
- Min cpp/rust: 0.50
- Max cpp/rust: 0.73

case | cpp_ns_op | rust_ns_op | cpp/rust
---|---:|---:|---:
single_key_item | 1798.28 | 3169.955 | 0.57
batch_single_key_item | 1809.75 | 3152.940 | 0.57
multi_key_item | 2194.15 | 3383.857 | 0.65
absent_key | 2118.29 | 3043.640 | 0.70
batch_absent_key | 2113.90 | 3043.691 | 0.69
sum_tree_single_key | 1816.41 | 3320.702 | 0.55
batch_sum_tree_single_key | 1805.73 | 3321.758 | 0.54
big_sum_tree_single_key | 1837.58 | 3356.002 | 0.55
count_tree_single_key | 1802.15 | 3294.627 | 0.55
count_sum_tree_single_key | 1812.68 | 3407.056 | 0.53
provable_count_sum_tree_single_key | 1814.41 | 3411.944 | 0.53
sum_tree_item_with_sum_single_key | 1820.45 | 3398.486 | 0.54
sum_tree_item_with_sum_range | 3452.64 | 4996.071 | 0.69
batch_sum_tree_item_with_sum_range | 3448.61 | 5002.819 | 0.69
count_sum_tree_item_with_sum_range | 3475.54 | 5167.626 | 0.67
batch_count_sum_tree_item_with_sum_range | 3468.26 | 5158.795 | 0.67
big_sum_tree_item_with_sum_range | 3480.05 | 5200.580 | 0.67
provable_count_sum_tree_item_with_sum_range | 3507.39 | 5236.278 | 0.67
sum_tree_item_with_sum_nested_range | 5643.92 | 11034.593 | 0.51
batch_sum_tree_item_with_sum_nested_range | 5633.40 | 11019.756 | 0.51
provable_count_single_key | 2221.79 | 3496.723 | 0.64
provable_count_range_query | 2922.71 | 4593.892 | 0.64
range_query | 3457.26 | 4953.959 | 0.70
batch_range_query | 3459.26 | 4947.355 | 0.70
range_after_query | 3177.75 | 4508.811 | 0.70
range_absent | 2127.84 | 3325.896 | 0.64
range_query_empty | 2130.30 | 3332.272 | 0.64
path_query_simple | 4452.21 | 6101.133 | 0.73
path_query_range_to | 2637.62 | 3789.766 | 0.70
path_query_range_after | 3236.51 | 4728.201 | 0.68
path_query_range_to_inclusive | 2961.49 | 4421.433 | 0.67
path_query_range_after_to | 3186.10 | 4396.768 | 0.72
path_query_range_after_to_inclusive | 3456.17 | 5024.293 | 0.69
nested_path_single_key | 3766.02 | 7472.445 | 0.50
range_inclusive_query | 3148.61 | 4866.255 | 0.65
nested_path_range_query | 5650.62 | 9195.554 | 0.61
provable_count_nested_range_query | 5283.23 | 9304.111 | 0.57
delete_tree_absent | 619.39 | 908.038 | 0.68
batch_delete_tree_absent | 618.64 | 907.722 | 0.68
delete_pc_tree_absent | 1613.56 | 2915.750 | 0.55
reference_single_key | 2191.89 | 3412.503 | 0.64
reference_chain_max_hop | 2383.87 | 3549.877 | 0.67
batch_reference_chain_max_hop | 2380.02 | 3555.610 | 0.67
