#!/bin/bash
# Build BerkeleyDB 5.3.21 from submodule source
#
# This script configures and builds BerkeleyDB if not already built.
# The library will be placed in deps/libdb/build_unix/.libs/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIBDB_DIR="$SCRIPT_DIR/libdb"
BUILD_DIR="$LIBDB_DIR/build_unix"

echo "Building BerkeleyDB 5.3.21..."

# Check if submodule is initialized
if [ ! -d "$LIBDB_DIR/dist" ]; then
    echo "Error: BerkeleyDB submodule not initialized"
    echo "Run: git submodule update --init --recursive"
    exit 1
fi

# Navigate to build directory
cd "$BUILD_DIR"

# Configure if not already done
if [ ! -f Makefile ]; then
    echo "Configuring BerkeleyDB..."
    ../dist/configure \
        --enable-shared \
        --disable-static \
        --enable-cxx=no \
        --enable-java=no \
        --enable-tcl=no
fi

# Build
echo "Compiling BerkeleyDB..."
make -j"$(nproc 2>/dev/null || echo 4)"

# Verify build
if [ -f .libs/libdb-5.3.so ]; then
    SIZE=$(du -h .libs/libdb-5.3.so | cut -f1)
    echo "✓ BerkeleyDB built successfully: .libs/libdb-5.3.so ($SIZE)"
else
    echo "✗ Build failed: library not found"
    exit 1
fi
