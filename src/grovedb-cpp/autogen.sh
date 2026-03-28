#!/bin/sh
# autogen.sh - Bootstrap the autotools build system

set -e

echo "Running autotools bootstrap..."

# Create required directories
mkdir -p build-aux
mkdir -p m4

# Run libtoolize first (required for LT_INIT)
# macOS uses glibtoolize, Linux uses libtoolize
if command -v glibtoolize >/dev/null 2>&1; then
    LIBTOOLIZE=glibtoolize
elif command -v libtoolize >/dev/null 2>&1; then
    LIBTOOLIZE=libtoolize
else
    echo "Error: libtoolize not found. Please install libtool."
    exit 1
fi

echo "Running $LIBTOOLIZE..."
$LIBTOOLIZE --copy --force

# Run autotools in correct order
echo "Running aclocal..."
aclocal -I m4

echo "Running autoheader..."
autoheader

echo "Running autoconf..."
autoconf

echo "Running automake..."
automake --add-missing --copy

echo ""
echo "Bootstrap complete!"
echo "Now run: ./configure && make && make check"
