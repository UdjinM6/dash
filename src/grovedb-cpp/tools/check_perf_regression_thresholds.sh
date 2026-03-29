#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THRESHOLDS="${ROOT_DIR}/docs/perf/regression-thresholds.json"
INSERTION="${ROOT_DIR}/docs/perf/insertion_deltas.md"
CORPUS="${ROOT_DIR}/docs/perf/corpus_proof_deltas.md"

if [[ "${PERF_REGRESSION_WAIVE:-0}" == "1" ]]; then
  echo "perf regression threshold check waived (PERF_REGRESSION_WAIVE=1)"
  exit 0
fi

if [[ ! -f "${THRESHOLDS}" || ! -f "${INSERTION}" || ! -f "${CORPUS}" ]]; then
  echo "missing perf threshold inputs" >&2
  exit 1
fi

ins_threshold="$(grep -o '"insertion_max_cpp_over_rust_ratio":[[:space:]]*[0-9.]*' "${THRESHOLDS}" | head -n1 | awk -F: '{print $2}' | xargs)"
cor_threshold="$(grep -o '"corpus_max_cpp_over_rust_ratio":[[:space:]]*[0-9.]*' "${THRESHOLDS}" | head -n1 | awk -F: '{print $2}' | xargs)"

if [[ -z "${ins_threshold}" || -z "${cor_threshold}" ]]; then
  echo "failed to parse thresholds from ${THRESHOLDS}" >&2
  exit 1
fi

check_table() {
  local file="$1"
  local threshold="$2"
  local label="$3"
  local bad=0
  while IFS= read -r line; do
    ratio="$(echo "${line}" | awk -F'|' '{print $4}' | xargs)"
    case_name="$(echo "${line}" | awk -F'|' '{print $1}' | xargs)"
    if [[ -z "${ratio}" || "${ratio}" == "cpp/rust" || "${ratio}" == "---:" ]]; then
      continue
    fi
    if [[ ! "${ratio}" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
      continue
    fi
    case_threshold="${threshold}"
    override="$(
      grep -E "\"${case_name//\//\\/}\"[[:space:]]*:[[:space:]]*[0-9.]+" "${THRESHOLDS}" \
      | head -n1 \
      | awk -F: '{print $2}' \
      | tr -d ' ,'
    )"
    if [[ -n "${override}" ]]; then
      case_threshold="${override}"
    fi
    if ! awk -v r="${ratio}" -v t="${case_threshold}" 'BEGIN {exit !(r <= t)}'; then
      echo "${label} threshold exceeded: case='${case_name}' ratio=${ratio} threshold=${case_threshold}" >&2
      bad=1
    fi
  done < <(grep '|' "${file}")
  return "${bad}"
}

check_table "${INSERTION}" "${ins_threshold}" "insertion" || exit 1
check_table "${CORPUS}" "${cor_threshold}" "corpus" || exit 1

echo "perf regression thresholds passed (insertion<=${ins_threshold}, corpus<=${cor_threshold})"
