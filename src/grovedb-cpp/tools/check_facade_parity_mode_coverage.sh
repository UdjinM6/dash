#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
writer_file="${repo_root}/tools/rust-storage-tools/src/bin/rust_grovedb_facade_writer.rs"
reader_file="${repo_root}/tools/rust-storage-tools/src/bin/rust_grovedb_facade_reader.rs"
cpp_file="${repo_root}/tests/rust_grovedb_facade_parity_tests.cpp"

tmp_writer="$(mktemp)"
tmp_reader="$(mktemp)"
tmp_cpp="$(mktemp)"
cleanup() {
  rm -f "${tmp_writer}" "${tmp_reader}" "${tmp_cpp}"
}
trap cleanup EXIT

grep -E '^[[:space:]]+"[a-z0-9_]+"[[:space:]]*=>' "${writer_file}" \
  | sed -E 's/^[[:space:]]*"([a-z0-9_]+)".*/\1/' \
  | sort -u > "${tmp_writer}"

grep -E '^[[:space:]]+"[a-z0-9_]+"[[:space:]]*=>' "${reader_file}" \
  | sed -E 's/^[[:space:]]*"([a-z0-9_]+)".*/\1/' \
  | sort -u > "${tmp_reader}"

awk '
  /const std::vector<std::string> modes = \{/ { in_modes=1; next }
  in_modes {
    while (match($0, /"([a-z0-9_]+)"/)) {
      print substr($0, RSTART + 1, RLENGTH - 2)
      $0 = substr($0, RSTART + RLENGTH)
    }
    if ($0 ~ /\};/) {
      in_modes=0
    }
  }
' "${cpp_file}" | sort -u > "${tmp_cpp}"

writer_missing_in_reader="$(comm -23 "${tmp_writer}" "${tmp_reader}")"
reader_extra_vs_writer="$(comm -13 "${tmp_writer}" "${tmp_reader}")"
missing_in_cpp="$(comm -23 "${tmp_writer}" "${tmp_cpp}")"
extra_in_cpp="$(comm -13 "${tmp_writer}" "${tmp_cpp}")"

if [[ -n "${writer_missing_in_reader}" || -n "${reader_extra_vs_writer}" || \
      -n "${missing_in_cpp}" || -n "${extra_in_cpp}" ]]; then
  if [[ -n "${writer_missing_in_reader}" ]]; then
    echo "facade parity mode coverage mismatch: Rust writer modes missing in Rust reader:"
    echo "${writer_missing_in_reader}"
  fi
  if [[ -n "${reader_extra_vs_writer}" ]]; then
    echo "facade parity mode coverage mismatch: extra Rust reader modes not in Rust writer:"
    echo "${reader_extra_vs_writer}"
  fi
  if [[ -n "${missing_in_cpp}" ]]; then
    echo "facade parity mode coverage mismatch: missing in C++ mode list:"
    echo "${missing_in_cpp}"
  fi
  if [[ -n "${extra_in_cpp}" ]]; then
    echo "facade parity mode coverage mismatch: extra in C++ mode list:"
    echo "${extra_in_cpp}"
  fi
  exit 1
fi

echo "facade parity mode coverage check passed"
