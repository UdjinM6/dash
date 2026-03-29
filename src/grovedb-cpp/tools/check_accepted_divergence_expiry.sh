#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOC="${ROOT_DIR}/docs/accepted-divergences.md"

if [[ ! -f "${DOC}" ]]; then
  echo "missing accepted-divergences doc: ${DOC}" >&2
  exit 1
fi

today_utc="$(date -u +%Y-%m-%d)"
stale=0

while IFS='|' read -r c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 _; do
  id="$(echo "${c2}" | xargs)"
  status="$(echo "${c4}" | xargs)"
  expiry="$(echo "${c9}" | xargs)"

  if [[ -z "${id}" || "${id}" == "id" || "${id}" == "---" || "${id}" == "none" ]]; then
    continue
  fi
  if [[ "${status}" != "active" ]]; then
    continue
  fi
  if [[ ! "${expiry}" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
    echo "invalid expiry for active divergence '${id}': '${expiry}'" >&2
    stale=1
    continue
  fi
  if [[ "${expiry}" < "${today_utc}" ]]; then
    echo "expired active divergence '${id}' (expiry=${expiry}, today=${today_utc})" >&2
    stale=1
  fi
done < <(grep '^|' "${DOC}")

if [[ "${stale}" -ne 0 ]]; then
  exit 1
fi

echo "accepted divergences check passed (${today_utc})"
