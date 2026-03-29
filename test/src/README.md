# DBSQL Test Suite

Modern C-based testing infrastructure for comprehensive code coverage of the DBSQL codebase.

## Features

- **TAP (Test Anything Protocol) output**: Standard test output format
- **Multiple build configurations**: DEBUG, COVERAGE, DIAGNOSTIC, UMEM, RELEASE
- **Code coverage measurement**: Target >95% function and branch coverage
- **Sanitizer integration**: AddressSanitizer, LeakSanitizer, UndefinedBehaviorSanitizer
- **Memory debugging**: libumem integration with multiple configurations
- **Performance benchmarking**: Built-in timing for performance-critical tests

## Quick Start

```bash
# Build and run tests
cd test/src
make test

# Generate coverage report
make BUILD_TYPE=COVERAGE coverage

# Run with AddressSanitizer
make asan-test

# Run with libumem memory debugging
make umem-test

# Check coverage meets 95% target
make coverage-check
```

## Test Organization

### Test Files

- **test_api.c**: Core API tests (open, close, exec, prepare, step, finalize)
- **test_vdbe.c**: VDBE (Virtual Database Engine) tests
- **test_memory.c**: Memory management and leak detection tests
- **test_concurrency.c**: Thread safety and concurrent access tests
- **test_sql_parser.c**: SQL parsing and code generation tests
- **test_performance.c**: Performance benchmarks

### Test Framework

- **test_framework.h**: Test macros and utilities
- **test_framework.c**: Test framework implementation

## Build Configurations

### DEBUG (Default)
- Debug symbols (-g3)
- Optimization for debugging (-Og)
- All sanitizers enabled
- Stack protection and fortification

```bash
make test
# or explicitly
make BUILD_TYPE=DEBUG test
```

### COVERAGE
- No optimization (-O0)
- Coverage instrumentation (--coverage)
- Used for code coverage analysis

```bash
make BUILD_TYPE=COVERAGE coverage
```

### DIAGNOSTIC
- Maximum warnings enabled
- All sanitizers
- Strict type checking and conversions

```bash
make BUILD_TYPE=DIAGNOSTIC test
```

### UMEM
- libumem memory allocator integration
- Multiple UMEM configurations tested
- Detects memory corruption, leaks, double-frees

```bash
make umem-test
```

### RELEASE
- Full optimization (-O2)
- No debugging symbols
- NDEBUG defined

```bash
make BUILD_TYPE=RELEASE test
```

## Sanitizer Options

### AddressSanitizer (ASan)
Detects:
- Buffer overflows (stack and heap)
- Use-after-free
- Use-after-return
- Double-free
- Memory leaks

Enabled by default in DEBUG and DIAGNOSTIC builds.

Environment variables:
```bash
ASAN_OPTIONS="detect_leaks=1:check_initialization_order=1:strict_init_order=1"
LSAN_OPTIONS="verbosity=1:log_threads=1"
```

### UndefinedBehaviorSanitizer (UBSan)
Detects:
- NULL pointer dereferences
- Signed integer overflow
- Pointer overflow
- Invalid shifts
- Out-of-bounds array accesses

Enabled by default in DEBUG and DIAGNOSTIC builds.

## libumem Configuration

libumem provides advanced memory debugging with three modes:

### Default Mode
Basic memory tracking, moderate overhead:
```bash
LD_PRELOAD=/path/to/libumem.so ./test_api
```

### Debug Mode
Detailed logging of allocations:
```bash
LD_PRELOAD=/path/to/libumem.so \
UMEM_DEBUG=default \
UMEM_LOGGING=transaction \
./test_api
```

### Audit Mode
Aggressive checks, highest overhead:
```bash
LD_PRELOAD=/path/to/libumem.so \
UMEM_DEBUG=audit \
./test_api
```

## Code Coverage

### Generating Reports

```bash
# Generate coverage report
make BUILD_TYPE=COVERAGE coverage

# View HTML report
firefox coverage_html/index.html

# Check coverage target
make coverage-check  # Fails if <95% coverage
```

### Coverage Target

- **Function coverage**: ≥95%
- **Line coverage**: ≥95%
- **Branch coverage**: ≥95%

### Adding Tests for Coverage Gaps

1. Generate coverage report: `make coverage`
2. Open `coverage_html/index.html` to identify uncovered code
3. Add tests to exercise uncovered functions/branches
4. Re-run coverage analysis
5. Repeat until target met

## Writing New Tests

### Test Structure

```c
#include "test_framework.h"
#include "../../src/inc/dbsql.h"

TEST(my_new_test) {
    // Setup
    DBSQL *db = NULL;
    int rc = dbsql_open(":memory:", &db);
    ASSERT_EQ(DBSQL_SUCCESS, rc);

    // Test code
    rc = dbsql_exec(db, "SELECT 1", NULL, NULL, NULL);
    ASSERT_EQ(DBSQL_SUCCESS, rc);

    // Cleanup
    dbsql_close(db);
}

int main(int argc, char *argv[]) {
    TEST_INIT("My Test Suite");
    test_run_all();
    TEST_FINISH();
}
```

### Test Macros

- `TEST(name)`: Define a test function
- `ASSERT_EQ(expected, actual)`: Assert equality
- `ASSERT_NE(expected, actual)`: Assert inequality
- `ASSERT_TRUE(condition)`: Assert condition is true
- `ASSERT_FALSE(condition)`: Assert condition is false
- `ASSERT_NULL(pointer)`: Assert pointer is NULL
- `ASSERT_NOT_NULL(pointer)`: Assert pointer is not NULL
- `ASSERT_STR_EQ(expected, actual)`: Assert string equality
- `TEST_SKIP(reason)`: Skip test with reason
- `TEST_TODO(reason)`: Mark test as TODO
- `BENCHMARK_START(name)`: Start performance benchmark
- `BENCHMARK_END(name)`: End performance benchmark

### Adding Tests to Build

1. Create new test file: `test_myfeature.c`
2. Add to `TEST_SRCS` in Makefile:
   ```makefile
   TEST_SRCS := test_api.c test_vdbe.c test_myfeature.c
   ```
3. Build and run: `make test`

## Continuous Integration

### Automated Testing

Run all test configurations in CI:

```bash
#!/bin/bash
set -e

# Debug build with sanitizers
make clean
make BUILD_TYPE=DEBUG test

# Coverage analysis
make clean
make BUILD_TYPE=COVERAGE coverage-check

# Diagnostic build (maximum warnings)
make clean
make BUILD_TYPE=DIAGNOSTIC test

# libumem testing
if [ -f ../libumem/libumem.so ]; then
    make clean
    make umem-test
fi

# Release build
make clean
make BUILD_TYPE=RELEASE test

echo "All CI tests passed!"
```

## Debugging Test Failures

### Running Individual Tests

```bash
# Build test
make test_api

# Run with debugger
gdb ./test_api

# Run with verbose sanitizer output
ASAN_OPTIONS="verbosity=1" ./test_api
```

### Analyzing Memory Issues

```bash
# Run with valgrind
valgrind --leak-check=full --show-leak-kinds=all ./test_api

# Run with libumem audit mode
LD_PRELOAD=/path/to/libumem.so \
UMEM_DEBUG=audit \
./test_api
```

### Coverage Analysis

```bash
# Generate coverage and identify gaps
make coverage
grep "0.0%" coverage_html/index.html

# View uncovered source
less coverage_html/path/to/source.c.gcov.html
```

## Performance Benchmarking

Tests include built-in benchmarking for performance-critical operations:

```c
TEST(benchmark_example) {
    BENCHMARK_START(my_operation);

    // Code to benchmark
    for (int i = 0; i < 1000; i++) {
        dbsql_exec(db, "SELECT 1", NULL, NULL, NULL);
    }

    BENCHMARK_END(my_operation);
}
```

Output includes timing: `# BENCHMARK my_operation: 1.234567 seconds`

## Troubleshooting

### Common Issues

**Issue**: Tests fail with "library not found"
**Solution**: Set `LD_LIBRARY_PATH`:
```bash
export LD_LIBRARY_PATH=../../build_unix.old:$LD_LIBRARY_PATH
```

**Issue**: Sanitizer reports false positives
**Solution**: Add suppressions file or disable specific checks:
```bash
ASAN_OPTIONS="suppressions=asan.supp"
```

**Issue**: Coverage data not generated
**Solution**: Ensure `BUILD_TYPE=COVERAGE` and run tests before generating report

**Issue**: libumem not found
**Solution**: Build libumem first:
```bash
cd ../../libumem
make
```

## Contributing

When adding new features to DBSQL:

1. Write tests first (TDD approach)
2. Ensure all tests pass: `make test`
3. Check coverage: `make coverage-check`
4. Run with sanitizers: `make asan-test`
5. Test with libumem: `make umem-test`
6. Add tests for edge cases and error conditions
7. Document any new test utilities in this README

## References

- [TAP Protocol](https://testanything.org/)
- [AddressSanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizer)
- [libumem](https://github.com/gburd/libumem)
- [lcov Documentation](http://ltp.sourceforge.net/coverage/lcov.php)
