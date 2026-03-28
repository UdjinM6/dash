# Accepted Divergences

This file tracks temporary parity divergences explicitly approved for short-term progress.
Every entry must include an expiry and owner.
Last reviewed: 2026-03-04

Status legend:
- `active`: temporary divergence currently accepted
- `expired`: divergence past expiry date (must be resolved)
- `closed`: divergence resolved and removed from active scope

## Entries

| id | area | status | reason | scope | owner | created | expiry | tracking_gap_id |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| visualizer-grovedbg-scope | visualizer | closed | Rust `start_visualizer` is feature-gated (`grovedbg`) and the C++ rewrite keeps this hook explicitly out of scope during parity closure, with deterministic unsupported contract text and tracked expiry | `GroveDb::StartVisualizer` returns unsupported until visualizer parity is scheduled | cpp-rewrite | 2026-02-11 | 2026-02-20 | visualizer-hook-parity-decision |

## Current State
- No active accepted divergences.
- CI enforcement: `bash tools/check_accepted_divergence_expiry.sh`
