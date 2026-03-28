#!/usr/bin/env bash
set -euo pipefail

iters=${1:-1000}
corpus=${2:-corpus/corpus.json}
build_dir=${3:-build-release}
samples=${4:-7}
min_sample_ms=${5:-100}

cmake --build "$build_dir" -j7

stamp="$(date -u +"%Y%m%dT%H%M%SZ")"
output="docs/perf/raw/cpp_corpus_bench_${stamp}.txt"

"$build_dir/bench_corpus" "$corpus" "$iters" "$samples" "$min_sample_ms" | tee "$output"
