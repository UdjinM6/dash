#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([grovedb-cpp], [1.0.0], [issues@grovedb.io])
AC_CONFIG_SRCDIR([src/grovedb.cpp])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])

# Initialize automake
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])
AM_SILENT_RULES([yes])

# Enable silent build rules if possible
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# C++ compiler checks
AC_PROG_CXX

# Set C++20 flag before checking
case "$CXX" in
    *g++* | *clang++*)
        CXXFLAGS="$CXXFLAGS -std=c++20"
        ;;
    *)
        CXXFLAGS="$CXXFLAGS -std=c++20"
        ;;
esac

AX_CXX_COMPILE_STDCXX_20
AC_PROG_CC

# Archiver check (required for libtool)
AM_PROG_AR

# Libtool for shared library support
LT_PREREQ([2.4])
LT_INIT

# Check for pkg-config
PKG_PROG_PKG_CONFIG

# Check for required headers
AC_CHECK_HEADERS([cstdint cstring vector string])

# Check for required functions
AC_CHECK_FUNCS([malloc free memcpy memset])

# RocksDB detection (required for current public API)
AC_ARG_WITH([rocksdb],
    [AS_HELP_STRING([--with-rocksdb=PATH],
        [use RocksDB at PATH (default: auto-detect)])],
    [
        if test "x$withval" = "xno"; then
            want_rocksdb=no
        elif test "x$withval" = "xyes"; then
            want_rocksdb=yes
        else
            want_rocksdb=yes
            ROCKSDB_DIR="$withval"
        fi
    ],
    [want_rocksdb=auto])

if test "x$want_rocksdb" = "xno"; then
    AC_MSG_ERROR([--without-rocksdb is not supported by the autotools build; RocksDB is currently required])
fi

ROCKSDB_LIBS=""
ROCKSDB_CFLAGS=""
have_rocksdb=no
rocksdb_libdir_flag=""

AC_MSG_CHECKING([for RocksDB])

    saved_CPPFLAGS="$CPPFLAGS"
    saved_LDFLAGS="$LDFLAGS"
    saved_LIBS="$LIBS"

    if test -n "$ROCKSDB_DIR"; then
        ROCKSDB_CFLAGS="-I$ROCKSDB_DIR/include"
        rocksdb_libdir_flag="-L$ROCKSDB_DIR/lib"
        CPPFLAGS="$CPPFLAGS $ROCKSDB_CFLAGS"
        LDFLAGS="$LDFLAGS $rocksdb_libdir_flag"
    fi

    # Try pkg-config first only when no explicit path was provided
    if test -z "$ROCKSDB_DIR" && $PKG_CONFIG --exists rocksdb 2>/dev/null; then
        ROCKSDB_CFLAGS=$($PKG_CONFIG --cflags rocksdb)
        ROCKSDB_LIBS=$($PKG_CONFIG --libs rocksdb)
        have_rocksdb=yes
    else
        # Fallback to manual detection
        AC_LANG_PUSH([C++])
        LIBS="$saved_LIBS $rocksdb_libdir_flag -lrocksdb"
        AC_LINK_IFELSE([
            AC_LANG_PROGRAM([
                #include <rocksdb/db.h>
                #include <rocksdb/options.h>
            ], [
                rocksdb::DB* db;
                rocksdb::Options options;
            ])
        ], [
            ROCKSDB_LIBS="$rocksdb_libdir_flag -lrocksdb"
            have_rocksdb=yes
        ], [
            # Try with -lrocksdb -lz -lbz2 -llz4 -lsnappy -lzstd
            LIBS="$saved_LIBS $rocksdb_libdir_flag -lrocksdb -lz -lbz2 -llz4 -lsnappy -lzstd"
            AC_LINK_IFELSE([
                AC_LANG_PROGRAM([
                    #include <rocksdb/db.h>
                ], [
                    rocksdb::DB* db;
                ])
            ], [
                ROCKSDB_LIBS="$rocksdb_libdir_flag -lrocksdb -lz -lbz2 -llz4 -lsnappy -lzstd"
                have_rocksdb=yes
            ], [
                # Try with Windows system libraries (required for static RocksDB on mingw)
                LIBS="$saved_LIBS $rocksdb_libdir_flag -lrocksdb -lrpcrt4 -lshlwapi"
                AC_LINK_IFELSE([
                    AC_LANG_PROGRAM([
                        #include <rocksdb/db.h>
                    ], [
                        rocksdb::DB* db;
                    ])
                ], [
                    ROCKSDB_LIBS="$rocksdb_libdir_flag -lrocksdb -lrpcrt4 -lshlwapi"
                    have_rocksdb=yes
                ], [have_rocksdb=no])
            ])
        ])
        AC_LANG_POP([C++])
    fi

    CPPFLAGS="$saved_CPPFLAGS"
    LDFLAGS="$saved_LDFLAGS"
    LIBS="$saved_LIBS"

if test "x$have_rocksdb" = "xyes"; then
    AC_MSG_RESULT([yes])
    AC_DEFINE([HAVE_ROCKSDB], [1], [Define if RocksDB is available])
else
    AC_MSG_RESULT([no])
    AC_MSG_ERROR([RocksDB not found; the autotools build currently requires RocksDB])
fi

AM_CONDITIONAL([HAVE_ROCKSDB], [test "x$have_rocksdb" = "xyes"])
AC_SUBST([ROCKSDB_CFLAGS])
AC_SUBST([ROCKSDB_LIBS])

# Enable debug build
AC_ARG_ENABLE([debug],
    [AS_HELP_STRING([--enable-debug],
        [enable debug build @<:@default=no@:>@])],
    [enable_debug=$enableval],
    [enable_debug=no])

if test "x$enable_debug" = "xyes"; then
    CXXFLAGS="$CXXFLAGS -g -O0 -DDEBUG"
    CFLAGS="$CFLAGS -g -O0 -DDEBUG"
else
    CXXFLAGS="$CXXFLAGS -O2"
    CFLAGS="$CFLAGS -O2"
fi

# Enable/disable tests
AC_ARG_ENABLE([tests],
    [AS_HELP_STRING([--disable-tests],
        [disable test building @<:@default=yes@:>@])],
    [enable_tests=$enableval],
    [enable_tests=yes])

# Enable/disable benchmarks
AC_ARG_ENABLE([benchmarks],
    [AS_HELP_STRING([--disable-benchmarks],
        [disable benchmark building @<:@default=yes@:>@])],
    [enable_benchmarks=$enableval],
    [enable_benchmarks=yes])

AM_CONDITIONAL([BUILD_TESTS], [test "x$enable_tests" = "xyes"])
AM_CONDITIONAL([BUILD_BENCHMARKS], [test "x$enable_benchmarks" = "xyes"])

# Output files
AC_CONFIG_FILES([
    Makefile
    src/Makefile
    include/Makefile
    tests/Makefile
    bench/Makefile
    third_party/Makefile
    third_party/blake3/Makefile
])

AC_OUTPUT

# Summary
echo ""
echo "Configuration summary:"
echo "  Prefix:           $prefix"
echo "  C++ Compiler:     $CXX"
echo "  C++ Flags:        $CXXFLAGS"
echo "  C Compiler:       $CC"
echo "  C Flags:          $CFLAGS"
echo "  RocksDB:          $have_rocksdb"
echo "  Tests:            $enable_tests"
echo "  Benchmarks:       $enable_benchmarks"
echo "  Debug:            $enable_debug"
echo ""
