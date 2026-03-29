# Rebuild Required - Debug Output Detected

## Issue

Background task b2f31b6 failed with debug output:

```
DEBUG: change_p4 addr=0 opcode=147 calling __str_nappend n=4 src=0x7ffff7fc09a7
DEBUG: change_p4 addr=1 opcode=147 __str_nappend returned p4.z=0x419f80
...
[stderr] Terminated
```

## Analysis

**Source Code Status**: ✅ Clean - No debug statements found
- Verified: No `fprintf` with "DEBUG:" in src/
- Verified: No debug output in current vdbe_method.c
- All debug statements were removed in earlier commits

**Binary Status**: ❌ Outdated - Using old compiled version
- The debug output is coming from old compiled binaries
- Binaries were built before debug statement removal
- Need to rebuild library and executables

## Root Cause

The `dbsql` binary and `libdbsql-0.4.so` library were compiled before the debug cleanup commits. The test suite is using these old binaries which still contain debug `fprintf` statements.

## Solution

Rebuild DBSQL library and binaries:

```bash
cd /home/gburd/ws/dbsql/build_unix.old

# Clean old binaries
make clean

# Rebuild with current source (no debug statements)
../dist/s_config
make

# Verify library is rebuilt
ls -lh .libs/libdbsql-0.4.so
ls -lh dbsql
```

## Verification

After rebuild:

1. **Test basic operation**:
   ```bash
   cd /home/gburd/ws/dbsql/test/src
   ./test_debug
   ```
   Should show NO debug output, only:
   ```
   Step 1: Creating environment...
   Result: 0
   SUCCESS: db = 0x...
   ...
   ```

2. **Run full test suite**:
   ```bash
   ./run_tests.sh
   ```
   Should execute cleanly without debug spam

3. **Check for debug output**:
   ```bash
   ./test_api 2>&1 | grep -i debug
   ```
   Should return no matches

## Impact

**Tests blocked by old binaries:**
- test_api.c - Using old library
- test_vdbe.c - Using old library
- test_memory.c - Using old library
- dbsql CLI - Old binary with debug output

**Coverage measurement blocked:**
- Cannot measure coverage with old instrumentation
- Need clean rebuild with --coverage flags

## Timeline

1. **Debug statements removed**: Earlier commits (tasks #79, #1)
2. **Library last built**: Before cleanup
3. **Tests executed**: Using old library
4. **Result**: Debug spam + terminated task

## Next Actions

1. ✅ **Identified issue** - Old binaries have debug code
2. ⏳ **Rebuild library** - `make clean && make` in build_unix.old/
3. ⏳ **Recompile tests** - Use rebuilt library
4. ⏳ **Execute tests** - Verify no debug output
5. ⏳ **Measure coverage** - With clean, instrumented build

## Build Commands

### Standard Build (No Debug)
```bash
cd /home/gburd/ws/dbsql/build_unix.old
make clean
../dist/s_config
make
```

### Coverage Build
```bash
cd /home/gburd/ws/dbsql/build_unix.old
make clean
../dist/s_config CFLAGS="--coverage -fprofile-arcs -ftest-coverage"
make
```

### Test Compilation
```bash
cd /home/gburd/ws/dbsql/test/src
./run_tests.sh  # Will use newly built library
```

## Status

- **Source code**: ✅ Clean
- **Binaries**: ❌ Outdated
- **Tests**: ⏳ Blocked pending rebuild
- **Coverage**: ⏳ Blocked pending rebuild

---

**Action Required**: Rebuild DBSQL library to remove debug output from binaries.
