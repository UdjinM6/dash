# C++ Test Suite

47 tests across 6 categories covering unit logic, API contracts, Rust parity, storage internals, cost accounting, and proof/replication.

## Categories

| Category | Count | Role |
|----------|------:|------|
| Core unit tests | 9 | Verify internal logic of each component |
| API contract tests | 8 | Guard public API surfaces against regressions |
| Parity tests | 21 | Ensure C++ output matches Rust byte-for-byte |
| Storage tests | 2 | Storage flags and Merk-level storage internals |
| Cost / shape tests | 2 | Deterministic cost accounting and tree shape |
| Proof / replication | 5 | Proof round-trips, chunk depth, and state sync |

## Core Unit Tests (9)

| File | Description |
|------|-------------|
| `element_tests` | Element serialization, type variants |
| `merk_tests` | Merk AVL insert, delete, balance |
| `storage_tests` | RocksDB wrapper basics |
| `chunk_util_tests` | Chunk encoding / decoding helpers |
| `chunk_restore_tests` | Full-tree restoration from chunks |
| `chunk_producer_tests` | Chunk generation from live trees |
| `raw_iterator_tests` | Low-level storage iterator |
| `corpus_tests` | Corpus-driven deterministic replay |
| `grovedb_facade_tests` | GroveDB facade insert, get, delete |

## API Contract Tests (8)

| File | Description |
|------|-------------|
| `element_api_contract_tests` | Element public API surface |
| `storage_api_contract_tests` | Storage read/write contracts |
| `storage_tx_contract_tests` | Transactional storage contracts |
| `merk_api_contract_tests` | Merk public API surface |
| `query_api_contract_tests` | Query builder and execution |
| `proof_api_contract_tests` | Proof generation and verification |
| `version_api_contract_tests` | Version negotiation API |
| `grovedb_facade_api_contract_tests` | GroveDB facade API surface |

## Parity Tests (22)

These compare C++ output against Rust reference data to ensure byte-level compatibility.

| File |
|------|
| `rust_storage_parity_tests` |
| `rust_storage_batch_parity_tests` |
| `rust_storage_reverse_parity_tests` |
| `rust_storage_tx_parity_tests` |
| `rust_storage_tx_read_parity_tests` |
| `rust_storage_tx_rollback_parity_tests` |
| `rust_storage_tx_conflict_parity_tests` |
| `rust_storage_export_byte_parity_tests` |
| `rust_storage_cost_shape_parity_tests` |
| `rust_merk_storage_parity_tests` |
| `rust_merk_storage_read_parity_tests` |
| `rust_merk_feature_storage_parity_tests` |
| `rust_merk_chunk_parity_tests` |
| `rust_merk_proof_parity_tests` |
| `rust_grovedb_facade_parity_tests` |
| `rust_grovedb_facade_cost_parity_tests` |
| `rust_grovedb_tx_conflict_parity_tests` |
| `rust_grovedb_proof_parity_tests` |
| `rust_grovedb_differential_fuzz_parity_tests` |
| `rust_replication_parity_tests` |
| `prefix_parity_tests` |
| `version_matrix_parity_tests` |

## Storage Tests (2)

| File | Description |
|------|-------------|
| `merk_storage_tests` | Merk-level storage read/write paths |
| `storage_flags_tests` | Epoch-based storage flag encoding |

## Cost / Shape Tests (2)

| File | Description |
|------|-------------|
| `merk_cost_shape_tests` | Deterministic cost values for Merk ops |
| `grovedb_facade_cost_tests` | Cost accounting through the facade layer |

## Proof / Replication Tests (5)

| File | Description |
|------|-------------|
| `chunk_depth_tests` | Chunk depth calculation and limits |
| `chunk_proof_tests` | Chunk-level proof generation |
| `grovedb_facade_proof_tests` | End-to-end proof through facade |
| `grovedb_proof_byte_roundtrip_tests` | Proof serialize/deserialize round-trip |
| `replication_tests` | State sync / replication protocol |

## Fixture Generation

Parity tests generate reference data at runtime — there are no checked-in fixture directories.

**How it works:**

1. Tests check for `GROVEDB_RUN_RUST_PARITY=1` in the environment and skip otherwise
2. Each test creates a fresh temp directory via `MakeTempDir()`
3. The test invokes a Rust writer tool (`cargo run --manifest-path tools/rust-storage-tools/Cargo.toml --bin <writer>`) that produces reference data into that temp directory
4. The C++ test reads the output and compares byte-for-byte against its own implementation

**Rust writer tools** live in `tools/rust-storage-tools/src/bin/` (18 binaries, one per parity test group).

Rust parity commit pin is tracked in `RUST_PARITY_COMMIT`; verify lock alignment with:

```bash
bash tools/check_rust_parity_commit_pin.sh
```

**CI** runs parity tests via `tools/run_parity_ctest.sh`, which supports test groups: `tx`, `semantic`, `byte`, `cost`, `contract`, `all`.

Differential replay helper:

```bash
bash tools/run_differential_fuzz.sh
```

In sandboxed environments, set a writable Cargo cache before enabling Rust parity tests, for example:

```bash
export CARGO_HOME=/tmp/grovedb-cpp-cargo-home
export CARGO_TARGET_DIR=/tmp/grovedb-cpp-cargo-target
```

`tools/run_parity_ctest.sh` defaults these to `/tmp/grovedb-cpp-cargo-home` and
`/tmp/grovedb-cpp-cargo-target` when unset.

## Running Tests

```bash
# Run all tests (CMake)
cd build
ctest --output-on-failure

# Run a single test by name
ctest -R element_tests --output-on-failure

# Run a category (e.g. all parity tests)
ctest -R parity --output-on-failure

# Verbose output
ctest -V
```

Autotools:

```bash
make check
```
