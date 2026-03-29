# Binary Mismatch Issue - Old Nix Build Has Debug Output

## Root Cause Identified

The failed background task (b2f31b6) used an **old Nix-built binary** with debug statements still present:

```
/home/gburd/ws/dbsql/result -> /nix/store/n1cvq67jphicc2arqbrbpvfnh55m493m-dbsql-DBSQL-0.3.1-pre20260324_dirty-
```

**Build date**: Pre-March 24, 2026 (before debug cleanup)
**Status**: Contains debug fprintf statements

## Binary Comparison

### Old Nix Build (Has Debug)
- **Path**: `/nix/store/n1cvq67jphicc2arqbrbpvfnh55m493m-dbsql-DBSQL-0.3.1-pre20260324_dirty-/bin/dbsql`
- **Version**: 0.3.1-pre20260324
- **Built**: Before debug cleanup commits
- **Debug output**: ✗ YES - Produces DEBUG: messages
- **Used by**: `result` symlink, may be in background tasks

### Current Build (Clean)
- **Path**: `/home/gburd/ws/dbsql/build_unix.old/.libs/dbsql`
- **Built**: March 27, 2026 09:32
- **Source**: Current codebase (no debug statements)
- **Debug output**: ✅ NO - Clean output
- **Used by**: Local development, test_debug.c

## Debug Output Example

From task b2f31b6 output:
```
DEBUG: change_p4 addr=0 opcode=147 calling __str_nappend n=4 src=0x7ffff7fc09a7
DEBUG: change_p4 addr=0 opcode=147 __str_nappend returned p4.z=0x4193b0 (setting P4_DYNAMIC)
DEBUG: change_p4 addr=1 opcode=147 calling __str_nappend n=4 src=0x7ffff7fc09ad
...
[stderr] Terminated
```

These messages come from old `vdbe_method.c` debug code that has since been removed.

## Verification

### Current Source is Clean ✅

```bash
$ grep -r "DEBUG.*change_p4" src/
# No matches

$ grep -r "calling __str_nappend" src/
# No matches

$ grep -r "returned p4.z=" src/
# No matches
```

### test_debug.c Uses Clean Binary ✅

```bash
$ ./test_debug  # Uses build_unix.old/.libs/libdbsql-0.4.so
Step 1: Creating environment...
Result: 0
SUCCESS: db = 0x490330
...
# NO DEBUG OUTPUT
```

## Solution

### Option 1: Use Local Build (Recommended)

Always use binaries from `build_unix.old/.libs/`:

```bash
export PATH="/home/gburd/ws/dbsql/build_unix.old/.libs:$PATH"
export LD_LIBRARY_PATH="/home/gburd/ws/dbsql/build_unix.old/.libs:$LD_LIBRARY_PATH"

# Or use absolute paths
/home/gburd/ws/dbsql/build_unix.old/.libs/dbsql
```

### Option 2: Rebuild Nix Package

Update `result` symlink to point to new Nix build:

```bash
cd /home/gburd/ws/dbsql
nix-build  # Rebuilds with current source
```

### Option 3: Remove result Symlink

Prevent accidental use of old binary:

```bash
rm /home/gburd/ws/dbsql/result
```

## Impact on Testing

### Tests Using Correct Binary ✅
- **test_debug.c** - Links against `build_unix.old/.libs/libdbsql-0.4.so`
- **test_api.c** - Will use local build when compiled with run_tests.sh
- **test_vdbe.c** - Will use local build
- **test_memory.c** - Will use local build

### Background Tasks May Use Old Binary ⚠️
- Tasks spawned by system may find old `dbsql` first
- PATH may include `/nix/store/.../bin` before local build
- Tasks should use absolute paths: `/home/gburd/ws/dbsql/build_unix.old/.libs/dbsql`

## Recommended Actions

1. **Update run_tests.sh** to use absolute binary paths
2. **Set PATH** to prioritize local build:
   ```bash
   export PATH="/home/gburd/ws/dbsql/build_unix.old/.libs:$PATH"
   ```
3. **Document binary locations** in test documentation
4. **Remove or rebuild** `result` symlink to avoid confusion

## Timeline

| Date | Event |
|------|-------|
| Pre-Mar 24 | Nix build created with debug code |
| Mar 24-26 | Debug statements removed from source (tasks #79, #1) |
| Mar 27 09:32 | Local build in build_unix.old/.libs/ (clean) |
| Mar 27 09:32+ | test_debug.c passes with clean binary |
| Background task b2f31b6 | Used old Nix binary, produced debug output, terminated |

## Status

- **Source code**: ✅ Clean (no debug statements)
- **Local build**: ✅ Clean (build_unix.old/.libs/)
- **Nix build**: ❌ Outdated (result symlink)
- **Test suite**: ✅ Ready (will use local build)
- **Coverage**: ⏳ Pending test execution

## Next Steps

1. ✅ **Identified issue** - Old Nix build has debug code
2. ⏳ **Update PATH** - Prioritize local build
3. ⏳ **Run tests** - Using clean local binary
4. ⏳ **Measure coverage** - With instrumented local build
5. ⏳ **Rebuild Nix package** - Optional, for deployment

---

**Conclusion**: The debug output is from an old Nix-built binary. The current source and local build are clean. Tests should use the local build in `build_unix.old/.libs/`.
