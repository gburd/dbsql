# Critical Bug Fix: str.c Type Mismatch

## The Fix

**File:** `/home/gburd/ws/dbsql/src/common/str.c`
**Line:** 177
**Change:** `unsigned long n;` → `int n;`

## Impact

**Before:** 34/41 tests passing (82.9%)
**After:** 41/41 tests passing (100%)

## What Was Broken

The `__str_nappend()` function had a type mismatch:
- Variable declared as `unsigned long n`
- Value read as `va_arg(ap, int)`
- When -1 passed to indicate "use strlen", became 0xFFFFFFFFFFFFFFFF
- Check `if (n <= 0)` always false for unsigned
- Passed huge negative size to `strncpy()`, triggering ASAN crash

## What Now Works

1. SQL parser error handling (was crashing)
2. SELECT queries (tests 15-17)
3. Transaction COMMIT (tests 32-33)
4. Transaction ROLLBACK (tests 39-40)
5. Invalid SQL error handling (test 26)

## Files Modified

- `/home/gburd/ws/dbsql/src/common/str.c` - One line changed

## Test Execution

```bash
cd /home/gburd/ws/dbsql/test/src
ASAN_OPTIONS=detect_leaks=0 LD_LIBRARY_PATH=../../build_unix.old/.libs ./test_api

# Tests: 41
# Passed: 41
# Failed: 0
# Time: 0.072988 seconds
```
