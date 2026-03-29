#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "build directory not found: ${BUILD_DIR}" >&2
  exit 1
fi

if [[ -z "${CARGO_HOME:-}" ]]; then
  export CARGO_HOME="/tmp/grovedb-cpp-cargo-home"
fi
if [[ -z "${CARGO_TARGET_DIR:-}" ]]; then
  export CARGO_TARGET_DIR="/tmp/grovedb-cpp-cargo-target"
fi
mkdir -p "${CARGO_HOME}" "${CARGO_TARGET_DIR}"

bash "${ROOT_DIR}/tools/check_rust_parity_commit_pin.sh"
(
  cd "${BUILD_DIR}"
  GROVEDB_RUN_RUST_PARITY=1 ctest -R rust_grovedb_differential_fuzz_parity_tests --output-on-failure
)
