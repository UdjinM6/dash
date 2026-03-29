# third_party/blake3/Makefile.am - Build blake3 library

# Blake3 library
lib_LTLIBRARIES = libblake3.la

# Blake3 sources (C files)
libblake3_la_SOURCES = \
    blake3.c \
    blake3_dispatch.c \
    blake3_portable.c

# Blake3 compiler flags (C, not C++)
libblake3_la_CFLAGS = \
    -Wall \
    -Wextra \
    -DBLAKE3_USE_NEON=0 \
    -DBLAKE3_NO_SSE2 \
    -DBLAKE3_NO_SSE41 \
    -DBLAKE3_NO_AVX2 \
    -DBLAKE3_NO_AVX512

# Include directory for blake3
libblake3_la_CPPFLAGS = -I$(top_srcdir)/third_party/blake3

# Install blake3 headers
include_HEADERS = \
    blake3.h \
    blake3_impl.h
