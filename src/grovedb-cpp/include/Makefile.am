# include/Makefile.am - Install public headers

# Public headers to install
nobase_include_HEADERS = \
    binary.h \
    chunk.h \
    chunk_depth.h \
    chunk_producer.h \
    chunk_restore.h \
    corpus.h \
    cost_utils.h \
    element.h \
    grove_version.h \
    grovedb.h \
    hash.h \
    hex.h \
    merk.h \
    merk_cache.h \
    merk_costs.h \
    merk_node.h \
    merk_storage.h \
    operation_cost.h \
    proof.h \
    query.h \
    replication.h \
    storage_flags.h \
    value_defined_cost.h

# Conditional RocksDB header
if HAVE_ROCKSDB
nobase_include_HEADERS += rocksdb_wrapper.h
endif
