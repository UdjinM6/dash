#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

files=(
  "${ROOT_DIR}/tests/element_api_contract_tests.cpp"
  "${ROOT_DIR}/tests/merk_api_contract_tests.cpp"
  "${ROOT_DIR}/tests/proof_api_contract_tests.cpp"
  "${ROOT_DIR}/tests/query_api_contract_tests.cpp"
  "${ROOT_DIR}/tests/storage_api_contract_tests.cpp"
  "${ROOT_DIR}/tests/storage_tx_contract_tests.cpp"
  "${ROOT_DIR}/tests/version_api_contract_tests.cpp"
)

bad=0
for file in "${files[@]}"; do
  if [[ ! -f "${file}" ]]; then
    echo "missing contract test file: ${file}" >&2
    bad=1
    continue
  fi
  if rg -n 'error\.empty\(\)|error\s*==\s*""|error\s*!=\s*""' "${file}" >/tmp/error_policy_hits.txt 2>&1; then
    echo "error-string policy violation in ${file}:" >&2
    cat /tmp/error_policy_hits.txt >&2
    bad=1
  fi
done

if [[ "${bad}" -ne 0 ]]; then
  exit 1
fi

echo "error-string policy check passed"
