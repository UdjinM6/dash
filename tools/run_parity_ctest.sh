#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "build directory not found: ${BUILD_DIR}" >&2
  exit 1
fi

group="${1:-all}"

if [[ "${GROVEDB_SKIP_RUST_PIN_CHECK:-0}" != "1" ]]; then
  bash "${ROOT_DIR}/tools/check_rust_parity_commit_pin.sh"
fi

if [[ -z "${CARGO_HOME:-}" ]]; then
  export CARGO_HOME="/tmp/grovedb-cpp-cargo-home"
fi
if [[ -z "${CARGO_TARGET_DIR:-}" ]]; then
  export CARGO_TARGET_DIR="/tmp/grovedb-cpp-cargo-target"
fi
mkdir -p "${CARGO_HOME}" "${CARGO_TARGET_DIR}"

if [[ -n "${GROVEDB_CTEST_JOBS:-}" ]]; then
  ctest_jobs="${GROVEDB_CTEST_JOBS}"
else
  cpu_count="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  ctest_jobs="$(( cpu_count > 1 ? cpu_count - 1 : 1 ))"
fi

run_group() {
  local name="$1"
  local regex="$2"
  echo "[parity] running group: ${name}"
  (
    cd "${BUILD_DIR}"
    GROVEDB_RUN_RUST_PARITY=1 ctest -j "${ctest_jobs}" -R "${regex}" --output-on-failure
  )
}

case "${group}" in
  tx)
    run_group "tx" \
      "rust_grovedb_facade_parity_(misc|facade_mutation|facade_reference|facade_reads|tx_core|tx_visibility|tx_conflict_order|tx_checkpoint_a|tx_checkpoint_b|batch_apply|batch_validate|batch_insert_delete|batch_misc)_tests|rust_grovedb_tx_conflict_parity_tests|grovedb_facade_api_contract_tests|storage_tx_contract_tests|rust_storage_tx_conflict_parity_tests"
    ;;
  semantic)
    run_group "semantic" \
      "rust_storage_parity_tests|rust_storage_reverse_parity_tests|rust_storage_batch_parity_tests|rust_storage_tx_parity_tests|rust_storage_tx_conflict_parity_tests|rust_storage_tx_rollback_parity_tests|rust_storage_tx_read_parity_tests|rust_merk_storage_parity_tests|rust_merk_storage_read_parity_tests|rust_merk_feature_storage_parity_tests|rust_merk_proof_parity_tests|rust_merk_chunk_parity_tests|rust_grovedb_proof_parity_tests|rust_grovedb_differential_fuzz_parity_tests|prefix_parity_tests"
    ;;
  byte)
    run_group "byte" \
      "rust_storage_export_byte_parity_tests|rust_merk_proof_parity_tests|rust_merk_chunk_parity_tests|grovedb_proof_byte_roundtrip_tests"
    ;;
  cost)
    run_group "cost" \
      "rust_storage_cost_shape_parity_tests|merk_cost_shape_tests"
    ;;
  contract)
    run_group "contract" \
      "storage_tx_contract_tests|storage_api_contract_tests|proof_api_contract_tests|query_api_contract_tests|version_api_contract_tests|version_matrix_parity_tests|merk_api_contract_tests|element_api_contract_tests|storage_tests"
    ;;
  all)
    run_group "tx" \
      "rust_grovedb_facade_parity_(misc|facade_mutation|facade_reference|facade_reads|tx_core|tx_visibility|tx_conflict_order|tx_checkpoint_a|tx_checkpoint_b|batch_apply|batch_validate|batch_insert_delete|batch_misc)_tests|rust_grovedb_tx_conflict_parity_tests|grovedb_facade_api_contract_tests|storage_tx_contract_tests|rust_storage_tx_conflict_parity_tests"
    run_group "semantic" \
      "rust_storage_parity_tests|rust_storage_reverse_parity_tests|rust_storage_batch_parity_tests|rust_storage_tx_parity_tests|rust_storage_tx_conflict_parity_tests|rust_storage_tx_rollback_parity_tests|rust_storage_tx_read_parity_tests|rust_merk_storage_parity_tests|rust_merk_storage_read_parity_tests|rust_merk_feature_storage_parity_tests|rust_merk_proof_parity_tests|rust_merk_chunk_parity_tests|rust_grovedb_proof_parity_tests|rust_grovedb_differential_fuzz_parity_tests|prefix_parity_tests"
    run_group "byte" \
      "rust_storage_export_byte_parity_tests|rust_merk_proof_parity_tests|rust_merk_chunk_parity_tests|grovedb_proof_byte_roundtrip_tests"
    run_group "cost" \
      "rust_storage_cost_shape_parity_tests|merk_cost_shape_tests"
    run_group "contract" \
      "storage_tx_contract_tests|storage_api_contract_tests|proof_api_contract_tests|query_api_contract_tests|version_api_contract_tests|version_matrix_parity_tests|merk_api_contract_tests|element_api_contract_tests|storage_tests"
    ;;
  *)
    echo "unknown group: ${group}" >&2
    echo "usage: $0 [tx|semantic|byte|cost|contract|all]" >&2
    exit 2
    ;;
esac
