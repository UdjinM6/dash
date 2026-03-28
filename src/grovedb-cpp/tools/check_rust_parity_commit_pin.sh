#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIN_FILE="${ROOT_DIR}/RUST_PARITY_COMMIT"
LOCK_FILE="${ROOT_DIR}/tools/rust-storage-tools/Cargo.lock"

if [[ ! -f "${PIN_FILE}" ]]; then
  echo "missing rust parity pin file: ${PIN_FILE}" >&2
  exit 1
fi
if [[ ! -f "${LOCK_FILE}" ]]; then
  echo "missing rust tools lockfile: ${LOCK_FILE}" >&2
  exit 1
fi

repo="$(grep -E '^REPO=' "${PIN_FILE}" | head -n1 | cut -d= -f2-)"
tag="$(grep -E '^TAG=' "${PIN_FILE}" | head -n1 | cut -d= -f2-)"
commit="$(grep -E '^COMMIT=' "${PIN_FILE}" | head -n1 | cut -d= -f2-)"

if [[ -z "${repo}" || -z "${tag}" || -z "${commit}" ]]; then
  echo "invalid ${PIN_FILE}: expected REPO=, TAG=, COMMIT=" >&2
  exit 1
fi

if [[ ! "${commit}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "invalid COMMIT value in ${PIN_FILE}: '${commit}'" >&2
  exit 1
fi

expected_source="git+${repo}?tag=${tag}#${commit}"
if ! grep -Fq "${expected_source}" "${LOCK_FILE}"; then
  echo "rust parity pin mismatch: expected lockfile source '${expected_source}'" >&2
  echo "hint: update ${PIN_FILE} or refresh ${LOCK_FILE} to match" >&2
  exit 1
fi

echo "rust parity commit pin check passed (${tag} @ ${commit})"
