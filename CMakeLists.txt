cmake_minimum_required(VERSION 3.18)
project(rewrite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

add_library(blake3 STATIC
    third_party/blake3/blake3.c
    third_party/blake3/blake3_dispatch.c
    third_party/blake3/blake3_portable.c
)

set_target_properties(blake3 PROPERTIES
    LINKER_LANGUAGE C
)

target_include_directories(blake3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/blake3
)

target_compile_definitions(blake3 PRIVATE
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
    BLAKE3_USE_NEON=0
)

add_library(grovedb_cpp
    src/binary.cpp
    src/chunk.cpp
    src/chunk_depth.cpp
    src/chunk_producer.cpp
    src/chunk_restore.cpp
    src/corpus.cpp
    src/element.cpp
    src/grovedb.cpp
    src/hex.cpp
    src/proof.cpp
    src/hash.cpp
    src/insert_profile.cpp
    src/merk.cpp
    src/merk_node.cpp
    src/merk_storage.cpp
    src/merk_cache.cpp
    src/query.cpp
    src/replication.cpp
    src/rocksdb_wrapper.cpp
    src/storage_flags.cpp
)

target_include_directories(grovedb_cpp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(grovedb_cpp PRIVATE -Wall -Wextra -Werror)
if (NOT MSVC)
    target_compile_options(grovedb_cpp PRIVATE
        -fstack-protector-strong
        -Wformat-security
    )
    target_compile_definitions(grovedb_cpp PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>
    )
endif()
target_link_libraries(grovedb_cpp PRIVATE blake3)

option(GROVEDB_CPP_ENABLE_ROCKSDB "Enable RocksDB backend if available" ON)
set(GROVEDB_CPP_WITH_ROCKSDB 0)
if (GROVEDB_CPP_ENABLE_ROCKSDB)
    find_library(ROCKSDB_LIB rocksdb)
    find_path(ROCKSDB_INCLUDE rocksdb/db.h)
    if (ROCKSDB_LIB AND ROCKSDB_INCLUDE)
        set(GROVEDB_CPP_WITH_ROCKSDB 1)
        target_include_directories(grovedb_cpp PRIVATE ${ROCKSDB_INCLUDE})
        target_link_libraries(grovedb_cpp PRIVATE ${ROCKSDB_LIB})
        target_compile_definitions(grovedb_cpp PRIVATE HAVE_ROCKSDB)
    else()
        message(STATUS "RocksDB not found; building without RocksDB backend")
    endif()
endif()

# === Benchmarks ===
add_executable(bench_corpus
    bench/corpus.cpp
)
target_link_libraries(bench_corpus PRIVATE grovedb_cpp)

add_executable(bench_insertion
    bench/insertion.cpp
)
target_link_libraries(bench_insertion PRIVATE grovedb_cpp)

add_executable(bench_insertion_storage
    bench/insertion_storage.cpp
)
target_link_libraries(bench_insertion_storage PRIVATE grovedb_cpp)

# === Tests (alphabetically sorted) ===
add_executable(binary_api_contract_tests
    tests/binary_api_contract_tests.cpp
)
target_link_libraries(binary_api_contract_tests PRIVATE grovedb_cpp)
add_test(NAME binary_api_contract_tests COMMAND binary_api_contract_tests)

add_executable(chunk_depth_tests
    tests/chunk_depth_tests.cpp
)
target_link_libraries(chunk_depth_tests PRIVATE grovedb_cpp)
add_test(NAME chunk_depth_tests COMMAND chunk_depth_tests)

add_executable(chunk_producer_tests
    tests/chunk_producer_tests.cpp
)
target_link_libraries(chunk_producer_tests PRIVATE grovedb_cpp)
add_test(NAME chunk_producer_tests COMMAND chunk_producer_tests)

add_executable(chunk_proof_tests
    tests/chunk_proof_tests.cpp
)
target_link_libraries(chunk_proof_tests PRIVATE grovedb_cpp)
add_test(NAME chunk_proof_tests COMMAND chunk_proof_tests)

add_executable(chunk_util_tests
    tests/chunk_util_tests.cpp
)
target_link_libraries(chunk_util_tests PRIVATE grovedb_cpp)
add_test(NAME chunk_util_tests COMMAND chunk_util_tests)

add_executable(corpus_tests
    tests/corpus_tests.cpp
)
target_link_libraries(corpus_tests PRIVATE grovedb_cpp)
add_test(NAME corpus_tests COMMAND corpus_tests ${CMAKE_CURRENT_SOURCE_DIR}/corpus/corpus.json)

add_executable(element_api_contract_tests
    tests/element_api_contract_tests.cpp
)
target_link_libraries(element_api_contract_tests PRIVATE grovedb_cpp)
add_test(NAME element_api_contract_tests COMMAND element_api_contract_tests)

add_executable(element_tests
    tests/element_tests.cpp
)
target_link_libraries(element_tests PRIVATE grovedb_cpp)
add_test(NAME element_tests COMMAND element_tests)

add_executable(grovedb_proof_byte_roundtrip_tests
    tests/grovedb_proof_byte_roundtrip_tests.cpp
)
target_link_libraries(grovedb_proof_byte_roundtrip_tests PRIVATE grovedb_cpp)
add_test(NAME grovedb_proof_byte_roundtrip_tests COMMAND grovedb_proof_byte_roundtrip_tests)

add_executable(harness
    src/main.cpp
)
target_link_libraries(harness PRIVATE grovedb_cpp)

add_executable(merk_api_contract_tests
    tests/merk_api_contract_tests.cpp
)
target_link_libraries(merk_api_contract_tests PRIVATE grovedb_cpp)
add_test(NAME merk_api_contract_tests COMMAND merk_api_contract_tests)

add_executable(merk_cost_shape_tests
    tests/merk_cost_shape_tests.cpp
)
target_link_libraries(merk_cost_shape_tests PRIVATE grovedb_cpp)
add_test(NAME merk_cost_shape_tests COMMAND merk_cost_shape_tests)

add_executable(merk_tests
    tests/merk_tests.cpp
)
target_link_libraries(merk_tests PRIVATE grovedb_cpp)
add_test(NAME merk_tests COMMAND merk_tests)

add_executable(prefix_parity_tests
    tests/prefix_parity_tests.cpp
)
target_link_libraries(prefix_parity_tests PRIVATE grovedb_cpp)
add_test(NAME prefix_parity_tests COMMAND prefix_parity_tests)

add_executable(proof_api_contract_tests
    tests/proof_api_contract_tests.cpp
)
target_link_libraries(proof_api_contract_tests PRIVATE grovedb_cpp)
add_test(NAME proof_api_contract_tests COMMAND proof_api_contract_tests)

add_executable(query_api_contract_tests
    tests/query_api_contract_tests.cpp
)
target_link_libraries(query_api_contract_tests PRIVATE grovedb_cpp)
add_test(NAME query_api_contract_tests COMMAND query_api_contract_tests)

add_executable(replication_tests
    tests/replication_tests.cpp
)
target_link_libraries(replication_tests PRIVATE grovedb_cpp)
add_test(NAME replication_tests COMMAND replication_tests)

add_executable(rust_grovedb_proof_parity_tests
    tests/rust_grovedb_proof_parity_tests.cpp
)
target_link_libraries(rust_grovedb_proof_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_grovedb_proof_parity_tests COMMAND rust_grovedb_proof_parity_tests)

add_executable(rust_grovedb_tx_conflict_parity_tests
    tests/rust_grovedb_tx_conflict_parity_tests.cpp
)
target_link_libraries(rust_grovedb_tx_conflict_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_grovedb_tx_conflict_parity_tests COMMAND rust_grovedb_tx_conflict_parity_tests)

add_executable(rust_merk_chunk_parity_tests
    tests/rust_merk_chunk_parity_tests.cpp
)
target_link_libraries(rust_merk_chunk_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_merk_chunk_parity_tests COMMAND rust_merk_chunk_parity_tests)

add_executable(rust_merk_feature_storage_parity_tests
    tests/rust_merk_feature_storage_parity_tests.cpp
)
target_link_libraries(rust_merk_feature_storage_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_merk_feature_storage_parity_tests COMMAND rust_merk_feature_storage_parity_tests)

add_executable(rust_merk_proof_parity_tests
    tests/rust_merk_proof_parity_tests.cpp
)
target_link_libraries(rust_merk_proof_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_merk_proof_parity_tests COMMAND rust_merk_proof_parity_tests)

add_executable(rust_merk_storage_parity_tests
    tests/rust_merk_storage_parity_tests.cpp
)
target_link_libraries(rust_merk_storage_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_merk_storage_parity_tests COMMAND rust_merk_storage_parity_tests)

add_executable(rust_merk_storage_read_parity_tests
    tests/rust_merk_storage_read_parity_tests.cpp
)
target_link_libraries(rust_merk_storage_read_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_merk_storage_read_parity_tests COMMAND rust_merk_storage_read_parity_tests)

add_executable(rust_replication_parity_tests
    tests/rust_replication_parity_tests.cpp
)
target_link_libraries(rust_replication_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_replication_parity_tests COMMAND rust_replication_parity_tests)

add_executable(rust_storage_batch_parity_tests
    tests/rust_storage_batch_parity_tests.cpp
)
target_link_libraries(rust_storage_batch_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_batch_parity_tests COMMAND rust_storage_batch_parity_tests)

add_executable(rust_storage_cost_shape_parity_tests
    tests/rust_storage_cost_shape_parity_tests.cpp
)
target_link_libraries(rust_storage_cost_shape_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_cost_shape_parity_tests COMMAND rust_storage_cost_shape_parity_tests)

add_executable(rust_storage_parity_tests
    tests/rust_storage_parity_tests.cpp
)
target_link_libraries(rust_storage_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_parity_tests COMMAND rust_storage_parity_tests)

add_executable(rust_storage_reverse_parity_tests
    tests/rust_storage_reverse_parity_tests.cpp
)
target_link_libraries(rust_storage_reverse_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_reverse_parity_tests COMMAND rust_storage_reverse_parity_tests)

add_executable(rust_storage_tx_conflict_parity_tests
    tests/rust_storage_tx_conflict_parity_tests.cpp
)
target_link_libraries(rust_storage_tx_conflict_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_tx_conflict_parity_tests COMMAND rust_storage_tx_conflict_parity_tests)

add_executable(rust_storage_tx_parity_tests
    tests/rust_storage_tx_parity_tests.cpp
)
target_link_libraries(rust_storage_tx_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_tx_parity_tests COMMAND rust_storage_tx_parity_tests)

add_executable(rust_storage_tx_read_parity_tests
    tests/rust_storage_tx_read_parity_tests.cpp
)
target_link_libraries(rust_storage_tx_read_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_tx_read_parity_tests COMMAND rust_storage_tx_read_parity_tests)

add_executable(rust_storage_tx_rollback_parity_tests
    tests/rust_storage_tx_rollback_parity_tests.cpp
)
target_link_libraries(rust_storage_tx_rollback_parity_tests PRIVATE grovedb_cpp)
add_test(NAME rust_storage_tx_rollback_parity_tests COMMAND rust_storage_tx_rollback_parity_tests)

add_executable(storage_flags_tests
    tests/storage_flags_tests.cpp
)
target_link_libraries(storage_flags_tests PRIVATE grovedb_cpp)
add_test(NAME storage_flags_tests COMMAND storage_flags_tests)

add_executable(version_matrix_parity_tests
    tests/version_matrix_parity_tests.cpp
)
target_link_libraries(version_matrix_parity_tests PRIVATE grovedb_cpp)
add_test(NAME version_matrix_parity_tests COMMAND version_matrix_parity_tests)

# === RocksDB-dependent tests ===
if (GROVEDB_CPP_WITH_ROCKSDB)
    add_executable(chunk_restore_tests
        tests/chunk_restore_tests.cpp
    )
    target_link_libraries(chunk_restore_tests PRIVATE grovedb_cpp)
    add_test(NAME chunk_restore_tests COMMAND chunk_restore_tests)

    add_executable(grovedb_facade_api_contract_tests
        tests/grovedb_facade_api_contract_tests.cpp
    )
    target_link_libraries(grovedb_facade_api_contract_tests PRIVATE grovedb_cpp)
    add_test(NAME grovedb_facade_api_contract_tests COMMAND grovedb_facade_api_contract_tests)

    add_executable(grovedb_facade_cost_tests
        tests/grovedb_facade_cost_tests.cpp
    )
    target_link_libraries(grovedb_facade_cost_tests PRIVATE grovedb_cpp)
    add_test(NAME grovedb_facade_cost_tests COMMAND grovedb_facade_cost_tests)

    add_executable(grovedb_facade_proof_tests
        tests/grovedb_facade_proof_tests.cpp
    )
    target_link_libraries(grovedb_facade_proof_tests PRIVATE grovedb_cpp)
    add_test(NAME grovedb_facade_proof_tests COMMAND grovedb_facade_proof_tests)

    add_executable(grovedb_facade_tests
        tests/grovedb_facade_tests.cpp
    )
    target_link_libraries(grovedb_facade_tests PRIVATE grovedb_cpp)
    add_test(NAME grovedb_facade_tests COMMAND grovedb_facade_tests)

    add_executable(version_api_contract_tests
        tests/version_api_contract_tests.cpp
    )
    target_link_libraries(version_api_contract_tests PRIVATE grovedb_cpp)
    add_test(NAME version_api_contract_tests COMMAND version_api_contract_tests)

    add_executable(merk_storage_tests
        tests/merk_storage_tests.cpp
    )
    target_link_libraries(merk_storage_tests PRIVATE grovedb_cpp)
    add_test(NAME merk_storage_tests COMMAND merk_storage_tests)

    add_executable(raw_iterator_tests
        tests/raw_iterator_tests.cpp
    )
    target_link_libraries(raw_iterator_tests PRIVATE grovedb_cpp)
    add_test(NAME raw_iterator_tests COMMAND raw_iterator_tests)

    add_executable(rust_grovedb_facade_cost_parity_tests
        tests/rust_grovedb_facade_cost_parity_tests.cpp
    )
    target_include_directories(rust_grovedb_facade_cost_parity_tests PRIVATE ${ROCKSDB_INCLUDE})
    target_link_libraries(rust_grovedb_facade_cost_parity_tests PRIVATE grovedb_cpp)
    add_test(NAME rust_grovedb_facade_cost_parity_tests COMMAND rust_grovedb_facade_cost_parity_tests)

    add_executable(rust_grovedb_facade_parity_tests
        tests/rust_grovedb_facade_parity_tests.cpp
    )
    target_include_directories(rust_grovedb_facade_parity_tests PRIVATE ${ROCKSDB_INCLUDE})
    target_link_libraries(rust_grovedb_facade_parity_tests PRIVATE grovedb_cpp)
    add_test(NAME rust_grovedb_facade_parity_misc_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=simple,nested
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_facade_mutation_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_PREFIX=facade_insert_,facade_flush,facade_root_key,facade_delete_if_empty_tree,facade_clear_subtree
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_facade_reference_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_PREFIX=facade_follow_reference,facade_find_subtrees
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_facade_reads_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_PREFIX=facade_get_,facade_has_,facade_query_
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_tx_core_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=tx_commit,tx_rollback,tx_visibility_rollback,tx_mixed_commit,tx_rollback_range,tx_drop_abort,tx_commit_after_rollback_rejected,tx_write_after_rollback_rejected,tx_multi_rollback_reuse,tx_delete_after_rollback,tx_reopen_visibility_after_rollback,tx_delete_visibility,tx_delete_then_reinsert_same_key,tx_insert_then_delete_same_key,tx_delete_missing_noop,tx_double_rollback_noop
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_tx_visibility_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=tx_read_committed_visibility,tx_has_read_committed_visibility,tx_query_range_committed_visibility,tx_iterator_stability_under_commit,tx_read_only_then_writer_commit,tx_reopen_conflict_same_path,tx_conflict_sequence_persistence
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_tx_conflict_order_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=tx_same_key_conflict_reverse_order,tx_shared_subtree_disjoint_conflict_reverse_order,tx_disjoint_subtree_conflict_reverse_order,tx_disjoint_subtree_conflict_forward_order,tx_delete_insert_same_key_forward,tx_delete_insert_same_key_reverse,tx_delete_insert_same_subtree_disjoint_forward,tx_delete_insert_same_subtree_disjoint_reverse,tx_delete_insert_disjoint_subtree_forward,tx_delete_insert_disjoint_subtree_reverse,tx_replace_delete_same_key_forward,tx_replace_delete_same_key_reverse,tx_replace_replace_same_key_forward,tx_replace_replace_same_key_reverse
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_tx_checkpoint_a_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=tx_checkpoint_snapshot_isolation,tx_checkpoint_independent_writes,tx_checkpoint_delete_safety,tx_checkpoint_open_safety,tx_checkpoint_delete_short_path_safety,tx_checkpoint_open_missing_path,tx_checkpoint_reopen_mutate_recheckpoint
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_tx_checkpoint_b_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=tx_checkpoint_reopen_mutate_chain,tx_checkpoint_batch_ops,tx_checkpoint_delete_reopen_sequence,tx_checkpoint_aux_isolation,tx_checkpoint_tx_operations,tx_checkpoint_chain_mutation_isolation,tx_checkpoint_reopen_after_main_delete
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_batch_apply_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=batch_apply_local_atomic,batch_apply_tx_visibility,batch_apply_empty_noop,batch_apply_failure_atomic_noop,batch_apply_tx_failure_atomic_noop,batch_apply_tx_failure_then_reuse,batch_apply_tx_failure_then_rollback,batch_apply_tx_success_then_rollback,batch_apply_tx_delete_then_rollback,batch_apply_tx_delete_missing_noop
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_batch_validate_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=batch_validate_success_noop,batch_validate_failure_noop,batch_validate_no_override_insert,batch_validate_no_override_insert_only,batch_validate_no_override_replace,batch_validate_no_override_tree_insert,batch_validate_strict_noop,batch_validate_no_override_tree_insert_or_replace
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_batch_insert_delete_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=batch_insert_only_semantics,batch_replace_semantics,batch_delete_non_empty_tree_error,batch_delete_non_empty_tree_no_error,batch_disable_consistency_check,batch_insert_tree_semantics,batch_insert_tree_replace,batch_sum_tree_create_and_sum_item,batch_count_tree_create_and_item,batch_provable_count_tree_create_and_item,batch_count_sum_tree_create_and_sum_item,batch_provable_count_sum_tree_create_and_sum_item,batch_delete_missing_noop,batch_delete_tree_op,batch_delete_tree_disable_consistency_check,batch_delete_tree_non_empty_options,batch_insert_tree_below_deleted_path_consistency,batch_insert_tree_with_root_hash,batch_insert_or_replace_semantics,batch_insert_or_replace_with_override_validation
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    add_test(NAME rust_grovedb_facade_parity_batch_misc_tests
        COMMAND ${CMAKE_COMMAND} -E env
            GROVEDB_RUST_PARITY_MODE_LIST=batch_mixed_non_minimal_ops,batch_mixed_non_minimal_ops_with_options,batch_patch_existing,batch_patch_missing,batch_patch_strict_no_override,batch_refresh_reference_trust,batch_pause_height_passthrough,batch_partial_pause_resume,batch_base_root_storage_is_free_passthrough
            $<TARGET_FILE:rust_grovedb_facade_parity_tests>)
    set_tests_properties(
        rust_grovedb_facade_parity_misc_tests
        rust_grovedb_facade_parity_facade_mutation_tests
        rust_grovedb_facade_parity_facade_reference_tests
        rust_grovedb_facade_parity_facade_reads_tests
        rust_grovedb_facade_parity_tx_core_tests
        rust_grovedb_facade_parity_tx_visibility_tests
        rust_grovedb_facade_parity_tx_conflict_order_tests
        rust_grovedb_facade_parity_tx_checkpoint_a_tests
        rust_grovedb_facade_parity_tx_checkpoint_b_tests
        rust_grovedb_facade_parity_batch_apply_tests
        rust_grovedb_facade_parity_batch_validate_tests
        rust_grovedb_facade_parity_batch_insert_delete_tests
        rust_grovedb_facade_parity_batch_misc_tests
        PROPERTIES RUN_SERIAL TRUE)

    add_executable(rust_grovedb_differential_fuzz_parity_tests
        tests/rust_grovedb_differential_fuzz_parity_tests.cpp
    )
    target_include_directories(rust_grovedb_differential_fuzz_parity_tests PRIVATE ${ROCKSDB_INCLUDE})
    target_link_libraries(rust_grovedb_differential_fuzz_parity_tests PRIVATE grovedb_cpp)
    add_test(NAME rust_grovedb_differential_fuzz_parity_tests COMMAND rust_grovedb_differential_fuzz_parity_tests)

    add_executable(rust_storage_export_byte_parity_tests
        tests/rust_storage_export_byte_parity_tests.cpp
    )
    target_include_directories(rust_storage_export_byte_parity_tests PRIVATE ${ROCKSDB_INCLUDE})
    target_link_libraries(rust_storage_export_byte_parity_tests PRIVATE grovedb_cpp)
    add_test(NAME rust_storage_export_byte_parity_tests COMMAND rust_storage_export_byte_parity_tests)

    add_executable(storage_api_contract_tests
        tests/storage_api_contract_tests.cpp
    )
    target_link_libraries(storage_api_contract_tests PRIVATE grovedb_cpp)
    add_test(NAME storage_api_contract_tests COMMAND storage_api_contract_tests)

    add_executable(storage_tests
        tests/storage_tests.cpp
    )
    target_link_libraries(storage_tests PRIVATE grovedb_cpp)
    add_test(NAME storage_tests COMMAND storage_tests)

    add_executable(storage_tx_contract_tests
        tests/storage_tx_contract_tests.cpp
    )
    target_link_libraries(storage_tx_contract_tests PRIVATE grovedb_cpp)
    add_test(NAME storage_tx_contract_tests COMMAND storage_tx_contract_tests)
endif()
