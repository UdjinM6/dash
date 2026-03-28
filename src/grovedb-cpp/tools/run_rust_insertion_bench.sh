#!/usr/bin/env bash
set -euo pipefail

warm_up_time=${1:-1}
measurement_time=${2:-2}
sample_size=${3:-10}
manifest_path=${4:-tools/insertion/Cargo.toml}
target_dir=${5:-tools/insertion/target}

if [[ ! -f "${manifest_path}" ]]; then
  echo "Insertion benchmark manifest not found: ${manifest_path}" >&2
  exit 1
fi

if [[ -z "${CARGO_HOME:-}" ]]; then
  export CARGO_HOME="/tmp/grovedb-cpp-cargo-home"
fi
mkdir -p "${CARGO_HOME}"

stamp="$(date -u +"%Y%m%dT%H%M%SZ")"
output="docs/perf/raw/rust_insertion_bench_${stamp}.txt"

CARGO_TARGET_DIR="${target_dir}" \
cargo bench --manifest-path "${manifest_path}" \
  --bench insertion_benchmark \
  -- --warm-up-time "${warm_up_time}" \
     --measurement-time "${measurement_time}" \
     --sample-size "${sample_size}" \
     --noplot | tee "${output}"

echo "wrote ${output}"
