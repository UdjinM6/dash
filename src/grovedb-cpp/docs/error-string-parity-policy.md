# Error String Parity Policy

This policy defines how C++ parity tests assert error strings.
Last reviewed: 2026-03-04

## Modes
- `exact`: use when the error is deterministic and produced directly by C++ rewrite code.
- `contains`: use when the message includes unstable/dynamic fragments or Rust-side wording that can vary slightly.
- `code-based`: reserved for future typed error codes (not yet adopted).

## Test Rules
- Contract tests must not assert on empty-string behavior (for example `error.empty()` as success criteria).
- Contract tests must assert either:
  - exact match: `error == "..."` / `error != "..."`
  - contains match: `error.find("...") != std::string::npos` pattern
- Unsupported-version checks should use `exact` mode.
- Rust-fixture shape-dependent checks may use `contains` mode when wording variability is expected.
- New tests should default to `exact` unless there is a documented reason to use `contains`.

## Enforcement
- `tools/check_error_string_policy.sh`

The script fails on forbidden empty-string assertion patterns in contract tests.
