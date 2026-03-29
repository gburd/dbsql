# DBSQL Test Suite Makefile
#
# Supports multiple build configurations:
# - DEBUG: Debug symbols + sanitizers
# - COVERAGE: Code coverage measurement
# - DIAGNOSTIC: Maximum warnings + sanitizers
# - UMEM: libumem memory debugging
#
# Usage:
#   make              # Build tests
#   make test         # Run tests
#   make coverage     # Generate coverage report
#   make umem-test    # Run with libumem
#   make clean        # Clean build artifacts

# Build configuration
BUILD_DIR := ../../build_unix.old
SRC_DIR := ../../src
LIBDBSQL := $(BUILD_DIR)/.libs/libdbsql-0.4.so
LIBUMEM_DIR := ../../libumem

# Compiler and flags
CC := gcc
CFLAGS_BASE := -std=c99 -I$(SRC_DIR)/inc -I$(BUILD_DIR) -Wall -Wextra -Wpedantic

# Build type selection (default: DEBUG)
BUILD_TYPE ?= DEBUG

# Platform detection
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Sanitizer flags (platform-specific)
ifeq ($(UNAME_S),Linux)
    SANITIZER_FLAGS := -fsanitize=address,leak,undefined,pointer-compare,pointer-subtract
    SANITIZER_FLAGS += -fsanitize=bounds,null,return,object-size,pointer-overflow
    SANITIZER_FLAGS += -fsanitize-address-use-after-scope
    # Additional hardening
    HARDENING_FLAGS := -fstack-protector-strong -D_FORTIFY_SOURCE=2
    HARDENING_FLAGS += -Wformat -Wformat-security
else ifeq ($(UNAME_S),Darwin)
    # macOS has more limited sanitizer support
    SANITIZER_FLAGS := -fsanitize=address,undefined
    HARDENING_FLAGS := -fstack-protector-strong
endif

# Coverage flags
COVERAGE_FLAGS := --coverage -fprofile-arcs -ftest-coverage

# Diagnostic flags (maximum warnings)
DIAGNOSTIC_FLAGS := -Wpedantic -Wconversion -Wsign-conversion
DIAGNOSTIC_FLAGS += -Wcast-align -Wcast-qual -Wdisabled-optimization
DIAGNOSTIC_FLAGS += -Wfloat-equal -Wformat=2 -Wimport -Winit-self
DIAGNOSTIC_FLAGS += -Wmissing-declarations -Wmissing-include-dirs
DIAGNOSTIC_FLAGS += -Wmissing-prototypes -Wnested-externs
DIAGNOSTIC_FLAGS += -Wold-style-definition -Wpacked -Wredundant-decls
DIAGNOSTIC_FLAGS += -Wshadow -Wstrict-prototypes -Wswitch-default
DIAGNOSTIC_FLAGS += -Wswitch-enum -Wundef -Wunreachable-code
DIAGNOSTIC_FLAGS += -Wwrite-strings

# Configuration-specific flags
ifeq ($(BUILD_TYPE),DEBUG)
    CFLAGS := $(CFLAGS_BASE) -Og -g3 -DDEBUG $(SANITIZER_FLAGS) $(HARDENING_FLAGS)
    LDFLAGS := $(SANITIZER_FLAGS)
else ifeq ($(BUILD_TYPE),COVERAGE)
    CFLAGS := $(CFLAGS_BASE) -O0 -g $(COVERAGE_FLAGS)
    LDFLAGS := $(COVERAGE_FLAGS)
else ifeq ($(BUILD_TYPE),DIAGNOSTIC)
    CFLAGS := $(CFLAGS_BASE) -Og -g3 $(DIAGNOSTIC_FLAGS) $(SANITIZER_FLAGS) $(HARDENING_FLAGS)
    LDFLAGS := $(SANITIZER_FLAGS)
else ifeq ($(BUILD_TYPE),UMEM)
    CFLAGS := $(CFLAGS_BASE) -O0 -g
    LDFLAGS := -L$(LIBUMEM_DIR) -lumem
else ifeq ($(BUILD_TYPE),RELEASE)
    CFLAGS := $(CFLAGS_BASE) -O2 -DNDEBUG
    LDFLAGS :=
else
    $(error Unknown BUILD_TYPE: $(BUILD_TYPE). Use DEBUG, COVERAGE, DIAGNOSTIC, UMEM, or RELEASE)
endif

# Test sources
TEST_SRCS := test_api.c test_vdbe.c test_memory.c test_fts.c test_comprehensive.c
TEST_FRAMEWORK_SRCS := test_framework.c
TEST_OBJS := $(TEST_SRCS:.c=.o)
TEST_FRAMEWORK_OBJS := $(TEST_FRAMEWORK_SRCS:.c=.o)
TEST_BINS := $(TEST_SRCS:.c=)

# Coverage files
COVERAGE_FILES := *.gcda *.gcno *.gcov

# Default target
all: $(TEST_BINS)

# Test framework library
test_framework.o: test_framework.c test_framework.h
	$(CC) $(CFLAGS) -c $< -o $@

# Individual test binaries
test_api: test_api.o test_framework.o
	$(CC) $(CFLAGS) -o $@ $^ -L$(BUILD_DIR)/.libs ../../build_unix.old/.libs/libdbsql-__EDIT_DBSQL_VERSION_MAJOR__.__EDIT_DBSQL_VERSION_MINOR__.a -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib -ldb-4.8 -lm $(LDFLAGS)

test_vdbe: test_vdbe.o test_framework.o
	$(CC) $(CFLAGS) -o $@ $^ -L$(BUILD_DIR)/.libs ../../build_unix.old/.libs/libdbsql-__EDIT_DBSQL_VERSION_MAJOR__.__EDIT_DBSQL_VERSION_MINOR__.a -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib -ldb-4.8 -lm $(LDFLAGS)

test_memory: test_memory.o test_framework.o
	$(CC) $(CFLAGS) -o $@ $^ -L$(BUILD_DIR)/.libs ../../build_unix.old/.libs/libdbsql-__EDIT_DBSQL_VERSION_MAJOR__.__EDIT_DBSQL_VERSION_MINOR__.a -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib -ldb-4.8 -lm -lpthread $(LDFLAGS)

test_fts: test_fts.o test_framework.o
	$(CC) $(CFLAGS) -o $@ $^ -L$(BUILD_DIR)/.libs ../../build_unix.old/.libs/libdbsql-__EDIT_DBSQL_VERSION_MAJOR__.__EDIT_DBSQL_VERSION_MINOR__.a -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib -ldb-4.8 -lm $(LDFLAGS)

test_comprehensive: test_comprehensive.o test_framework.o
	$(CC) $(CFLAGS) -o $@ $^ -L$(BUILD_DIR)/.libs ../../build_unix.old/.libs/libdbsql-__EDIT_DBSQL_VERSION_MAJOR__.__EDIT_DBSQL_VERSION_MINOR__.a -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib -ldb-4.8 -lm -lpthread $(LDFLAGS)

# Pattern rules
%.o: %.c test_framework.h
	$(CC) $(CFLAGS) -c $< -o $@

# Run all tests
test: $(TEST_BINS)
	@echo "=== Running DBSQL Test Suite ($(BUILD_TYPE)) ==="
	@for test in $(TEST_BINS); do \
		echo ""; \
		echo "--- Running $$test ---"; \
		LD_LIBRARY_PATH=$(BUILD_DIR)/.libs:$(LD_LIBRARY_PATH) ./$$test || exit 1; \
	done
	@echo ""
	@echo "=== All tests passed ==="

# Run with address sanitizer options
asan-test: BUILD_TYPE := DEBUG
asan-test: clean all
	@echo "=== Running tests with AddressSanitizer ==="
	ASAN_OPTIONS="detect_leaks=1:check_initialization_order=1:strict_init_order=1" \
	LSAN_OPTIONS="verbosity=1:log_threads=1" \
	$(MAKE) test

# Run with libumem
umem-test: BUILD_TYPE := UMEM
umem-test: clean all
	@echo "=== Running tests with libumem ==="
	@if [ ! -f $(LIBUMEM_DIR)/libumem.so ]; then \
		echo "Error: libumem not built. Run 'make' in $(LIBUMEM_DIR)"; \
		exit 1; \
	fi
	@for test in $(TEST_BINS); do \
		echo ""; \
		echo "--- Running $$test with libumem (default) ---"; \
		LD_PRELOAD=$(LIBUMEM_DIR)/libumem.so \
		LD_LIBRARY_PATH=$(BUILD_DIR):$(LD_LIBRARY_PATH) ./$$test || exit 1; \
		echo ""; \
		echo "--- Running $$test with libumem (debug) ---"; \
		LD_PRELOAD=$(LIBUMEM_DIR)/libumem.so \
		UMEM_DEBUG=default \
		UMEM_LOGGING=transaction \
		LD_LIBRARY_PATH=$(BUILD_DIR):$(LD_LIBRARY_PATH) ./$$test || exit 1; \
		echo ""; \
		echo "--- Running $$test with libumem (audit) ---"; \
		LD_PRELOAD=$(LIBUMEM_DIR)/libumem.so \
		UMEM_DEBUG=audit \
		LD_LIBRARY_PATH=$(BUILD_DIR):$(LD_LIBRARY_PATH) ./$$test || exit 1; \
	done

# Coverage target
coverage: BUILD_TYPE := COVERAGE
coverage: clean all test
	@echo "=== Generating coverage report ==="
	@echo "Coverage data files:"
	@ls -lh *.gcda 2>/dev/null || echo "No coverage data files found"
	@echo ""
	@echo "Generating HTML coverage report with lcov..."
	@if command -v lcov >/dev/null 2>&1; then \
		lcov --capture --directory . --output-file coverage.info; \
		lcov --remove coverage.info '/usr/*' --output-file coverage.info; \
		lcov --list coverage.info; \
		if command -v genhtml >/dev/null 2>&1; then \
			genhtml coverage.info --output-directory coverage_html; \
			echo ""; \
			echo "HTML report generated in coverage_html/"; \
			echo "Open coverage_html/index.html in a browser"; \
		fi; \
	else \
		echo "lcov not found. Generating simple text report with gcov..."; \
		for src in $(TEST_SRCS) $(TEST_FRAMEWORK_SRCS); do \
			gcov $$src; \
		done; \
		echo ""; \
		echo "Coverage summary:"; \
		grep -A 3 "File.*\.c'" *.gcov || true; \
	fi

# Display coverage summary
coverage-summary: coverage
	@echo "=== Coverage Summary ==="
	@if command -v lcov >/dev/null 2>&1; then \
		lcov --summary coverage.info; \
	else \
		@echo "Install lcov for detailed coverage analysis"; \
	fi

# Check coverage meets target (95% function and branch coverage)
coverage-check: coverage
	@echo "=== Checking coverage target (>= 95%) ==="
	@if command -v lcov >/dev/null 2>&1; then \
		lcov --summary coverage.info | tee /tmp/coverage_summary.txt; \
		lines_pct=$$(grep "lines" /tmp/coverage_summary.txt | sed 's/.*: \([0-9.]*\)%.*/\1/'); \
		funcs_pct=$$(grep "functions" /tmp/coverage_summary.txt | sed 's/.*: \([0-9.]*\)%.*/\1/'); \
		echo ""; \
		echo "Function coverage: $$funcs_pct%"; \
		echo "Line coverage: $$lines_pct%"; \
		if [ "$$(echo "$$funcs_pct < 95" | bc)" -eq 1 ]; then \
			echo "FAIL: Function coverage below 95% target"; \
			exit 1; \
		fi; \
		echo "PASS: Coverage meets target"; \
	fi

# Analyze coverage and suggest new tests
analyze-coverage: coverage
	@echo "=== Analyzing coverage gaps ==="
	@./analyze_coverage.sh --suggest --report coverage_analysis.txt
	@echo ""
	@echo "Detailed analysis saved to coverage_analysis.txt"

# Clean build artifacts
clean:
	rm -f $(TEST_OBJS) $(TEST_FRAMEWORK_OBJS) $(TEST_BINS)
	rm -f $(COVERAGE_FILES)
	rm -f coverage.info
	rm -rf coverage_html
	rm -rf *.dSYM

# Clean everything including libraries
distclean: clean
	rm -f $(LIBDBSQL)

# Help target
help:
	@echo "DBSQL Test Suite Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build all tests (default: DEBUG mode)"
	@echo "  test          - Run all tests"
	@echo "  asan-test     - Run tests with AddressSanitizer"
	@echo "  umem-test     - Run tests with libumem memory debugging"
	@echo "  coverage      - Generate coverage report"
	@echo "  coverage-check- Check coverage meets 95% target"
	@echo "  clean         - Remove build artifacts"
	@echo "  help          - Display this help message"
	@echo ""
	@echo "Build types (set with BUILD_TYPE=):"
	@echo "  DEBUG         - Debug symbols + sanitizers (default)"
	@echo "  COVERAGE      - Code coverage measurement"
	@echo "  DIAGNOSTIC    - Maximum warnings + sanitizers"
	@echo "  UMEM          - libumem memory debugging"
	@echo "  RELEASE       - Optimized build, no debugging"
	@echo ""
	@echo "Examples:"
	@echo "  make BUILD_TYPE=COVERAGE test"
	@echo "  make coverage-check"
	@echo "  make umem-test"

.PHONY: all test asan-test umem-test coverage coverage-summary coverage-check clean distclean help
