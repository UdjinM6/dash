#!/usr/bin/env bash
set -euo pipefail

items=${1:-10000}
samples=${2:-10}
warmup_samples=${3:-1}
reset_per_sample=${4:-1}
build_dir=${5:-build-release}

cmake --build "$build_dir" -j7 --target bench_insertion_storage

stamp="$(date -u +"%Y%m%dT%H%M%SZ")"
output="docs/perf/raw/cpp_insertion_storage_bench_${stamp}.txt"

"$build_dir/bench_insertion_storage" "$items" "$samples" "$warmup_samples" "$reset_per_sample" | tee "$output"
