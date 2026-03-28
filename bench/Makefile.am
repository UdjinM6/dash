# bench/Makefile.am - Build benchmark programs

# Benchmark programs (not installed)
noinst_PROGRAMS = \
    bench_corpus \
    bench_insertion \
    bench_insertion_storage

# Common preprocessor flags
AM_CPPFLAGS =

AM_CXXFLAGS = \
    -std=c++20 \
    -Wall \
    -Wextra \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/third_party/blake3

# Common libraries
common_ldadd = \
    $(top_builddir)/src/libgrovedb_cpp.la \
    $(top_builddir)/third_party/blake3/libblake3.la

# Conditional RocksDB flags
if HAVE_ROCKSDB
AM_CPPFLAGS += $(ROCKSDB_CFLAGS)
AM_CXXFLAGS += -DHAVE_ROCKSDB
common_ldadd += $(ROCKSDB_LIBS)
endif

# Benchmark program definitions
bench_corpus_SOURCES = corpus.cpp
bench_corpus_LDADD = $(common_ldadd)

bench_insertion_SOURCES = insertion.cpp
bench_insertion_LDADD = $(common_ldadd)

bench_insertion_storage_SOURCES = insertion_storage.cpp
bench_insertion_storage_LDADD = $(common_ldadd)
