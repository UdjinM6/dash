# tests/Makefile.am - Build and run test suite

# Test programs (alphabetically sorted)
check_PROGRAMS = \
    chunk_depth_tests \
    chunk_producer_tests \
    chunk_proof_tests \
    chunk_restore_tests \
    chunk_util_tests \
    corpus_tests \
    element_api_contract_tests \
    element_tests \
    grovedb_facade_api_contract_tests \
    grovedb_facade_cost_tests \
    grovedb_facade_proof_tests \
    grovedb_facade_tests \
    grovedb_proof_byte_roundtrip_tests \
    merk_api_contract_tests \
    merk_cost_shape_tests \
    merk_tests \
    prefix_parity_tests \
    proof_api_contract_tests \
    query_api_contract_tests \
    replication_tests \
    rust_grovedb_proof_parity_tests \
    rust_grovedb_tx_conflict_parity_tests \
    rust_merk_chunk_parity_tests \
    rust_merk_feature_storage_parity_tests \
    rust_merk_proof_parity_tests \
    rust_merk_storage_parity_tests \
    rust_merk_storage_read_parity_tests \
    rust_replication_parity_tests \
    rust_storage_batch_parity_tests \
    rust_storage_cost_shape_parity_tests \
    rust_storage_parity_tests \
    rust_storage_reverse_parity_tests \
    rust_storage_tx_conflict_parity_tests \
    rust_storage_tx_parity_tests \
    rust_storage_tx_read_parity_tests \
    rust_storage_tx_rollback_parity_tests \
    storage_flags_tests \
    version_api_contract_tests \
    version_matrix_parity_tests

# Conditional RocksDB tests
if HAVE_ROCKSDB
check_PROGRAMS += \
    merk_storage_tests \
    raw_iterator_tests \
    rust_grovedb_facade_cost_parity_tests \
    rust_grovedb_differential_fuzz_parity_tests \
    rust_grovedb_facade_parity_tests \
    rust_storage_export_byte_parity_tests \
    storage_api_contract_tests \
    storage_tests \
    storage_tx_contract_tests
endif

# TESTS for automake test suite
TESTS = $(check_PROGRAMS)

# Common preprocessor flags for all tests
AM_CPPFLAGS =

AM_CXXFLAGS = \
    -std=c++20 \
    -Wall \
    -Wextra \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/third_party/blake3

# Common libraries for all tests
common_ldadd = \
    $(top_builddir)/src/libgrovedb_cpp.la \
    $(top_builddir)/third_party/blake3/libblake3.la \
    -lpthread

# Conditional RocksDB flags
if HAVE_ROCKSDB
AM_CPPFLAGS += $(ROCKSDB_CFLAGS)
AM_CXXFLAGS += -DHAVE_ROCKSDB
common_ldadd += $(ROCKSDB_LIBS)
endif

# Test program definitions
chunk_depth_tests_SOURCES = chunk_depth_tests.cpp
chunk_depth_tests_LDADD = $(common_ldadd)

chunk_producer_tests_SOURCES = chunk_producer_tests.cpp
chunk_producer_tests_LDADD = $(common_ldadd)

chunk_proof_tests_SOURCES = chunk_proof_tests.cpp
chunk_proof_tests_LDADD = $(common_ldadd)

chunk_restore_tests_SOURCES = chunk_restore_tests.cpp
chunk_restore_tests_LDADD = $(common_ldadd)

chunk_util_tests_SOURCES = chunk_util_tests.cpp
chunk_util_tests_LDADD = $(common_ldadd)

corpus_tests_SOURCES = corpus_tests.cpp
corpus_tests_LDADD = $(common_ldadd)

element_api_contract_tests_SOURCES = element_api_contract_tests.cpp
element_api_contract_tests_LDADD = $(common_ldadd)

element_tests_SOURCES = element_tests.cpp
element_tests_LDADD = $(common_ldadd)

grovedb_facade_api_contract_tests_SOURCES = grovedb_facade_api_contract_tests.cpp
grovedb_facade_api_contract_tests_LDADD = $(common_ldadd)

grovedb_facade_cost_tests_SOURCES = grovedb_facade_cost_tests.cpp
grovedb_facade_cost_tests_LDADD = $(common_ldadd)

grovedb_facade_proof_tests_SOURCES = grovedb_facade_proof_tests.cpp
grovedb_facade_proof_tests_LDADD = $(common_ldadd)

grovedb_facade_tests_SOURCES = grovedb_facade_tests.cpp
grovedb_facade_tests_LDADD = $(common_ldadd)

grovedb_proof_byte_roundtrip_tests_SOURCES = grovedb_proof_byte_roundtrip_tests.cpp
grovedb_proof_byte_roundtrip_tests_LDADD = $(common_ldadd)

merk_api_contract_tests_SOURCES = merk_api_contract_tests.cpp
merk_api_contract_tests_LDADD = $(common_ldadd)

merk_cost_shape_tests_SOURCES = merk_cost_shape_tests.cpp
merk_cost_shape_tests_LDADD = $(common_ldadd)

merk_tests_SOURCES = merk_tests.cpp
merk_tests_LDADD = $(common_ldadd)

prefix_parity_tests_SOURCES = prefix_parity_tests.cpp
prefix_parity_tests_LDADD = $(common_ldadd)

proof_api_contract_tests_SOURCES = proof_api_contract_tests.cpp
proof_api_contract_tests_LDADD = $(common_ldadd)

query_api_contract_tests_SOURCES = query_api_contract_tests.cpp
query_api_contract_tests_LDADD = $(common_ldadd)

replication_tests_SOURCES = replication_tests.cpp
replication_tests_LDADD = $(common_ldadd)

rust_grovedb_proof_parity_tests_SOURCES = rust_grovedb_proof_parity_tests.cpp
rust_grovedb_proof_parity_tests_LDADD = $(common_ldadd)

rust_grovedb_tx_conflict_parity_tests_SOURCES = rust_grovedb_tx_conflict_parity_tests.cpp
rust_grovedb_tx_conflict_parity_tests_LDADD = $(common_ldadd)

rust_merk_chunk_parity_tests_SOURCES = rust_merk_chunk_parity_tests.cpp
rust_merk_chunk_parity_tests_LDADD = $(common_ldadd)

rust_merk_feature_storage_parity_tests_SOURCES = rust_merk_feature_storage_parity_tests.cpp
rust_merk_feature_storage_parity_tests_LDADD = $(common_ldadd)

rust_merk_proof_parity_tests_SOURCES = rust_merk_proof_parity_tests.cpp
rust_merk_proof_parity_tests_LDADD = $(common_ldadd)

rust_merk_storage_parity_tests_SOURCES = rust_merk_storage_parity_tests.cpp
rust_merk_storage_parity_tests_LDADD = $(common_ldadd)

rust_merk_storage_read_parity_tests_SOURCES = rust_merk_storage_read_parity_tests.cpp
rust_merk_storage_read_parity_tests_LDADD = $(common_ldadd)

rust_replication_parity_tests_SOURCES = rust_replication_parity_tests.cpp
rust_replication_parity_tests_LDADD = $(common_ldadd)

rust_storage_batch_parity_tests_SOURCES = rust_storage_batch_parity_tests.cpp
rust_storage_batch_parity_tests_LDADD = $(common_ldadd)

rust_storage_cost_shape_parity_tests_SOURCES = rust_storage_cost_shape_parity_tests.cpp
rust_storage_cost_shape_parity_tests_LDADD = $(common_ldadd)

rust_storage_parity_tests_SOURCES = rust_storage_parity_tests.cpp
rust_storage_parity_tests_LDADD = $(common_ldadd)

rust_storage_reverse_parity_tests_SOURCES = rust_storage_reverse_parity_tests.cpp
rust_storage_reverse_parity_tests_LDADD = $(common_ldadd)

rust_storage_tx_conflict_parity_tests_SOURCES = rust_storage_tx_conflict_parity_tests.cpp
rust_storage_tx_conflict_parity_tests_LDADD = $(common_ldadd)

rust_storage_tx_parity_tests_SOURCES = rust_storage_tx_parity_tests.cpp
rust_storage_tx_parity_tests_LDADD = $(common_ldadd)

rust_storage_tx_read_parity_tests_SOURCES = rust_storage_tx_read_parity_tests.cpp
rust_storage_tx_read_parity_tests_LDADD = $(common_ldadd)

rust_storage_tx_rollback_parity_tests_SOURCES = rust_storage_tx_rollback_parity_tests.cpp
rust_storage_tx_rollback_parity_tests_LDADD = $(common_ldadd)

storage_flags_tests_SOURCES = storage_flags_tests.cpp
storage_flags_tests_LDADD = $(common_ldadd)

version_api_contract_tests_SOURCES = version_api_contract_tests.cpp
version_api_contract_tests_LDADD = $(common_ldadd)

version_matrix_parity_tests_SOURCES = version_matrix_parity_tests.cpp
version_matrix_parity_tests_LDADD = $(common_ldadd)

# Conditional RocksDB tests
if HAVE_ROCKSDB
merk_storage_tests_SOURCES = merk_storage_tests.cpp
merk_storage_tests_LDADD = $(common_ldadd)

raw_iterator_tests_SOURCES = raw_iterator_tests.cpp
raw_iterator_tests_LDADD = $(common_ldadd)

rust_grovedb_facade_cost_parity_tests_SOURCES = rust_grovedb_facade_cost_parity_tests.cpp
rust_grovedb_facade_cost_parity_tests_LDADD = $(common_ldadd)

rust_grovedb_differential_fuzz_parity_tests_SOURCES = rust_grovedb_differential_fuzz_parity_tests.cpp
rust_grovedb_differential_fuzz_parity_tests_LDADD = $(common_ldadd)

rust_grovedb_facade_parity_tests_SOURCES = rust_grovedb_facade_parity_tests.cpp
rust_grovedb_facade_parity_tests_LDADD = $(common_ldadd)

rust_storage_export_byte_parity_tests_SOURCES = rust_storage_export_byte_parity_tests.cpp
rust_storage_export_byte_parity_tests_LDADD = $(common_ldadd)

storage_api_contract_tests_SOURCES = storage_api_contract_tests.cpp
storage_api_contract_tests_LDADD = $(common_ldadd)

storage_tests_SOURCES = storage_tests.cpp
storage_tests_LDADD = $(common_ldadd)

storage_tx_contract_tests_SOURCES = storage_tx_contract_tests.cpp
storage_tx_contract_tests_LDADD = $(common_ldadd)
endif
