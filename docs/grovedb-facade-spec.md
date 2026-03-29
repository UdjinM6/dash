# GroveDb Facade Spec (C++ Rewrite)

## Goal
Define the current C++ `GroveDb` facade contract for parity with Rust GroveDB `v4.0.0`.

This document is normative for behavior and API shape at the facade layer. Detailed area coverage lives in:
- `docs/rust-cpp-feature-matrix.md`
- `docs/compat-invariants.md`
- `docs/accepted-divergences.md`
- `docs/error-string-parity-policy.md`

## Scope
The facade is not a "minimum CRUD wrapper" anymore. It includes:
- lifecycle: open/wipe/flush/checkpoints
- typed and raw element mutation/read APIs
- transactions (plain + tx overloads)
- subtree orchestration and metadata APIs
- query APIs (raw, optional-keys, value/sum extraction, key-element pairs)
- batch APIs (apply/validate/partial continue + estimate)
- proof APIs (generation + verification family, including version-gated wrappers)
- replication/state-sync helpers
- version-gated wrappers across supported versions

## Data Model
- Values are serialized Grove elements (`element_bytes`) with Rust-compatible encoding semantics.
- Subtrees are tree-typed elements; each subtree maps to a Merk tree persisted via storage context.
- Keys and path segments are byte vectors; ordering is lexicographic byte order.

## Path and Validation Semantics
- `path` is the subtree path from root to target subtree.
- Root path is empty (`{}`).
- Path traversal requires existing tree-typed elements at each segment.
- Missing/non-tree path segments are hard API errors.

## Transaction Contract
- Transaction handles are explicit and stateful.
- Reads/writes with active tx use tx visibility rules covered by parity fixtures.
- Committed/rolled-back/uninitialized handles are rejected where API requires active tx.
- Lifecycle contracts (commit/rollback misuse) are enforced by contract tests.

## Mutation Contract
- Supports generic insert/delete and typed insert helpers (item/tree/sum/count/reference variants).
- "Insert-only"/"replace"/"insert-or-replace"-style semantics are exposed through batch and helper APIs with Rust-aligned option handling.
- Delete operations preserve Rust-aligned absent-key behavior (`success + deleted=false` where applicable).

## Query Contract
- Facade query APIs expose:
  - raw key/value pairs
  - optional-key result sets
  - value/sum extraction APIs
  - key-element-pair APIs
- Query shapes unsupported by a specific method fail explicitly (for example when subqueries are disallowed by that method).
- Offset/limit and direction behavior follows Rust-aligned contracts per API.

## Batch Contract
- `ApplyBatch` supports local-atomic and external-tx execution paths.
- `ValidateBatch` performs dry-run style validation semantics.
- Partial batch pause/resume is supported via `ApplyPartialBatch` and `ContinuePartialApplyBatch`.
- Estimated-operation APIs provide cost-path estimation without mutating storage.
- Disabled-consistency canonicalization behavior follows Rust-backed parity modes.

## Proof Contract
- Facade exposes proof generation (`ProveQuery*`) and verification APIs for path/query proof workflows.
- Version-gated proof APIs are supported and enforce unsupported-version rejection.
- Verification family includes subset/absence/chained-query/parent-tree-info variants where implemented.
- Query proof behavior (including layered/subquery/reference cases) follows parity fixtures and contract tests.

## Checkpoint Contract
- Create/open/delete checkpoint lifecycle is supported.
- Snapshot/isolation and safety semantics are validated by Rust-backed parity modes and C++ contract tests.

## Replication Contract
- State-sync session lifecycle is exposed (start/fetch/apply/complete/commit/empty).
- Global chunk ID encode/decode and version-gated variants are part of facade-level replication support.

## Versioning Contract
- Version-gated wrappers are part of the public facade surface.
- Unsupported-version behavior is explicit and tested.
- Supported-version delegation must match base API behavior.

## Error Contract
- Errors follow policy in `docs/error-string-parity-policy.md`.
- Contract tests must assert deterministic `exact` or stable `contains` behavior.

## Test Evidence (Required)
- C++ facade contract tests:
  - `tests/grovedb_facade_api_contract_tests.cpp`
  - `tests/grovedb_facade_tests.cpp`
  - `tests/grovedb_facade_proof_tests.cpp`
  - `tests/grovedb_facade_cost_tests.cpp`
- Rust-backed facade parity:
  - `tests/rust_grovedb_facade_parity_tests.cpp`
  - `tests/rust_grovedb_proof_parity_tests.cpp`
  - `tests/rust_grovedb_facade_cost_parity_tests.cpp`

## Non-Goals
- This spec does not restate every method signature from `include/grovedb.h`.
- This spec does not replace detailed parity matrices or invariants docs.
