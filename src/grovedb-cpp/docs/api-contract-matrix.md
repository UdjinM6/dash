# C++ API Contract Matrix

This matrix tracks contract-level coverage for public headers in `include/`.
Last validated: 2026-03-04

Coverage status:
- `covered`: direct contract/negative test coverage exists
- `indirect`: covered through higher-level integration/parity suites where no dedicated header-level contract target exists

| Header | Primary API Surface | Coverage | Contract Tests |
| --- | --- | --- | --- |
| `include/element.h` | Element encode/decode/reference helpers | covered | `tests/element_api_contract_tests.cpp` |
| `include/query.h` | Query/PathQuery builders and query item contracts | covered | `tests/query_api_contract_tests.cpp` |
| `include/proof.h` | Proof decode/verify/execute/rewrite API contracts | covered | `tests/proof_api_contract_tests.cpp`, `tests/version_api_contract_tests.cpp` |
| `include/grove_version.h` | Version parse/order/support contracts | covered | `tests/version_api_contract_tests.cpp` |
| `include/rocksdb_wrapper.h` | Storage open/read/write/scan/tx contracts | covered | `tests/storage_api_contract_tests.cpp`, `tests/storage_tx_contract_tests.cpp` |
| `include/merk.h` | Merk public API null/invalid/contract checks | covered | `tests/merk_api_contract_tests.cpp` |
| `include/chunk.h` | Chunk decode/execute contracts | covered | `tests/chunk_util_tests.cpp`, `tests/chunk_proof_tests.cpp` |
| `include/chunk_producer.h` | Chunk producer/multichunk contract checks | covered | `tests/chunk_producer_tests.cpp` |
| `include/chunk_depth.h` | Chunk depth API contracts | covered | `tests/chunk_depth_tests.cpp` |
| `include/chunk_restore.h` | Chunk restore API contracts | covered | `tests/chunk_restore_tests.cpp` |
| `include/storage_flags.h` | Storage flag encode/decode contracts | covered | `tests/storage_flags_tests.cpp` |
| `include/merk_storage.h` | Persist/load/open precondition contracts | indirect | `tests/merk_storage_tests.cpp`, `tests/rust_merk_storage_*` |
| `include/corpus.h` | Corpus loader/decoder contracts | indirect | `tests/corpus_tests.cpp` |
| `include/binary.h` | Binary codec primitives | indirect | encode/decode coverage via element/proof/chunk/storage suites |
| `include/hash.h` | Hash helper primitives | indirect | coverage via proof/merk/storage suites |
| `include/hex.h` | Hex codec helpers | indirect | coverage via corpus/proof suites |
| `include/merk_node.h` | Node encode/decode helpers | indirect | coverage via merk/proof/storage suites |
| `include/operation_cost.h` | Cost structs/reset behavior | indirect | `tests/merk_cost_shape_tests.cpp`, `tests/rust_storage_cost_shape_parity_tests.cpp` |
| `include/merk_costs.h` | Cost helpers/constants | indirect | coverage via storage/merk cost suites |
| `include/cost_utils.h` | Cost utility helpers | indirect | coverage via storage/merk cost suites |
| `include/value_defined_cost.h` | Value-defined cost types | indirect | coverage via merk/storage cost suites |

## Notes
- Contract coverage is enforced by `tools/run_parity_ctest.sh contract`.
- Error-string assertion policy is defined in `docs/error-string-parity-policy.md` and enforced by `tools/check_error_string_policy.sh`.
- Version-tagged contract execution matrix is covered by `tests/version_matrix_parity_tests.cpp`.
- All public API surfaces have full test coverage (direct or indirect).
