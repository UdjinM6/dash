# src/Makefile.am - Build the GroveDB C++ library

# Library to build
lib_LTLIBRARIES = libgrovedb_cpp.la

# Library sources (C++ files)
libgrovedb_cpp_la_SOURCES = \
    binary.cpp \
    chunk.cpp \
    chunk_depth.cpp \
    chunk_producer.cpp \
    chunk_restore.cpp \
    corpus.cpp \
    element.cpp \
    grovedb.cpp \
    hash.cpp \
    hex.cpp \
    insert_profile.cpp \
    merk.cpp \
    merk_cache.cpp \
    merk_node.cpp \
    merk_storage.cpp \
    proof.cpp \
    query.cpp \
    replication.cpp \
    storage_flags.cpp

# Conditional RocksDB source
if HAVE_ROCKSDB
libgrovedb_cpp_la_SOURCES += rocksdb_wrapper.cpp
endif

# Library compiler flags
libgrovedb_cpp_la_CXXFLAGS = \
    -std=c++20 \
    -Wall \
    -Wextra \
    -Werror \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/third_party/blake3

# Conditional RocksDB flags
if HAVE_ROCKSDB
libgrovedb_cpp_la_CXXFLAGS += -DHAVE_ROCKSDB
libgrovedb_cpp_la_CPPFLAGS = $(ROCKSDB_CFLAGS)
endif

# Library dependencies
libgrovedb_cpp_la_LIBADD = \
    $(top_builddir)/third_party/blake3/libblake3.la

# Conditional RocksDB libraries
if HAVE_ROCKSDB
libgrovedb_cpp_la_LIBADD += $(ROCKSDB_LIBS)
endif

# Library version info (current:revision:age)
libgrovedb_cpp_la_LDFLAGS = -version-info 1:0:0

# Harness program (main.cpp)
if BUILD_TESTS
check_PROGRAMS = harness

harness_SOURCES = main.cpp
harness_CXXFLAGS = \
    -std=c++20 \
    -Wall \
    -Wextra \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/third_party/blake3
harness_LDADD = libgrovedb_cpp.la
endif
