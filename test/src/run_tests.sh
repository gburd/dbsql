#!/bin/bash
# DBSQL Test Runner
# Compiles and runs all test programs

set -e

BUILD_DIR="../../build_unix.old"
SRC_DIR="../../src"
LIB_DIR="$BUILD_DIR/.libs"
LIBDBSQL="$LIB_DIR/libdbsql-0.4.so"
BDB_LIB="/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib"

CFLAGS="-std=c99 -I$BUILD_DIR -I$SRC_DIR/inc -Wall -Wextra"
LDFLAGS="-L$LIB_DIR -ldbsql-0.4 -lm -lpthread -L$BDB_LIB -ldb-4.8"
LDFLAGS="$LDFLAGS -Wl,-rpath,$LIB_DIR"

echo "=== DBSQL Test Suite ==="
echo

# Check library exists
if [ ! -f "$LIBDBSQL" ]; then
    echo "ERROR: Library not found: $LIBDBSQL"
    echo "Please build DBSQL first:"
    echo "  cd ../../build_unix.old && make"
    exit 1
fi

echo "Library: $LIBDBSQL"
echo "Compiler: $(gcc --version | head -1)"
echo

# List of test programs
TESTS="test_debug test_api test_vdbe test_memory"

# Compile each test
for test in $TESTS; do
    echo "Compiling $test..."
    if [ "$test" = "test_debug" ]; then
        gcc $CFLAGS $LDFLAGS -o $test ${test}.c
    else
        gcc $CFLAGS $LDFLAGS -o $test ${test}.c test_framework.c
    fi
done

echo
echo "=== Running Tests ==="
echo

# Clean up any leftover database files from previous runs
echo "Cleaning up old database files..."
rm -f *.db *.db_* __db.* 2>/dev/null || true
echo

# Run each test
for test in $TESTS; do
    echo "--- Running $test ---"
    ./$test || echo "FAILED: $test"
    # Clean up database files after each test
    rm -f *.db *.db_* __db.* 2>/dev/null || true
    echo
done

echo "=== Test Suite Complete ==="
