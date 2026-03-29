#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS_GEN_MANIFEST="${ROOT_DIR}/tools/corpus/Cargo.toml"
CANONICAL_CORPUS="${ROOT_DIR}/corpus/corpus.json"

tmp_one="$(mktemp /tmp/grovedb_cpp_corpus_1_XXXXXX.json)"
tmp_two="$(mktemp /tmp/grovedb_cpp_corpus_2_XXXXXX.json)"
cleanup() {
  rm -f "${tmp_one}" "${tmp_two}"
}
trap cleanup EXIT

echo "[parity] generating corpus sample #1"
(
  cd "${ROOT_DIR}"
  cargo run --manifest-path "${CORPUS_GEN_MANIFEST}" -- "${tmp_one}" >/tmp/grovedb_cpp_corpus_gen_1.log 2>&1
)

echo "[parity] generating corpus sample #2"
(
  cd "${ROOT_DIR}"
  cargo run --manifest-path "${CORPUS_GEN_MANIFEST}" -- "${tmp_two}" >/tmp/grovedb_cpp_corpus_gen_2.log 2>&1
)

if ! cmp -s "${tmp_one}" "${tmp_two}"; then
  echo "determinism check failed: two independent corpus generations differ" >&2
  diff -u "${tmp_one}" "${tmp_two}" || true
  exit 1
fi

if ! cmp -s "${tmp_one}" "${CANONICAL_CORPUS}"; then
  echo "canonical corpus drift detected: generated corpus differs from ${CANONICAL_CORPUS}" >&2
  diff -u "${CANONICAL_CORPUS}" "${tmp_one}" || true
  exit 1
fi

echo "[parity] corpus regeneration determinism check passed"
