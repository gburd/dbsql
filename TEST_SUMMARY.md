# DBSQL Test Suite Summary

## Quick Reference

**Total Tests:** 68 functional tests + 20 validation scripts + 4 smoke tests = **92 total tests**

**Test Location:** `/home/gburd/ws/dbsql/test/`

## Test Execution Quick Start

### Prerequisites
```bash
# Build the project first
cd /home/gburd/ws/dbsql
cp dist/RELEASE .
./dist/s_config
./dist/s_include
cd build_unix
../dist/configure
make
```

### Run Smoke Tests
```bash
# Minimal validation
./build_unix/dbsql :memory: "SELECT 1;"

# Basic SQL operations
./build_unix/dbsql :memory: < test/smoke.sql
```

### Run Full Test Suite
```bash
cd test
tclsh test.tcl
```

## Test Categories

| Category | Count | Priority | Expected Pass Rate |
|----------|-------|----------|-------------------|
| Core SQL | 15 | HIGH | 80-90% |
| DDL Operations | 8 | HIGH | 90-95% |
| Constraints | 5 | MEDIUM | 70-80% |
| Transactions | 4 | HIGH | 85-95% |
| Triggers | 4 | MEDIUM | 60-70% |
| Functions | 4 | MEDIUM | 70-80% |
| Advanced SQL | 8 | LOW | 50-60% |
| API Tests | 8 | HIGH | 80-90% |
| Stress Tests | 8 | MEDIUM | 60-70% |
| Threading | 1 | HIGH | 85-95% |

## Known Limitations

Current implementation is based on legacy SQLite (pre-3.52.0). The following SQLite 3.52.0 features are **NOT** supported and will cause test failures:

- Window functions
- UPSERT statements
- Generated columns
- FILTER clause
- Advanced CTEs
- Some JSON functions
- Expression indexes

## Test Result Format

Tests use Berkeley DB-style output:
```
Test001: Description
    Test001.a: subtest description
    Test001.b: another subtest
PASS: Test001 completed
```

Failures are marked with `FAIL:` prefix for easy grepping.

## Regression Testing

After code changes:
```bash
# Run smoke test
./build_unix/dbsql :memory: "SELECT 1;"

# Run affected test category
cd test
tclsh test.tcl
# (interactive) run_test testname
```

## More Information

See `test_baseline_report.md` for comprehensive documentation.

## Contact

For test failures or questions, see `test/README` for test system documentation.
