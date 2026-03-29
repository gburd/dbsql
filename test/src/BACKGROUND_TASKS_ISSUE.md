# Background Tasks Using Old Binary - Cleanup Needed

## Issue

Multiple background tasks are failing with debug output and exit code 143 (SIGTERM):

- **Task b2f31b6**: "Test 3a: CREATE table in /tmp/claude" - FAILED
- **Task bf6f92f**: "Test 3b: SELECT from persistent database" - FAILED

## Root Cause

Background tasks are executing the **old Nix-built `dbsql` binary** which contains debug fprintf statements:

```
DEBUG: change_p4 addr=0 opcode=147 calling __str_nappend n=4 src=0x7ffff7fc09a7
DEBUG: change_p4 addr=0 opcode=147 __str_nappend returned p4.z=0x419720 (setting P4_DYNAMIC)
...
[stderr] Terminated
```

**Binary path**: `/nix/store/n1cvq67jphicc2arqbrbpvfnh55m493m-dbsql-DBSQL-0.3.1-pre20260324_dirty-/bin/dbsql`

## Why Tasks Fail

1. **Debug output flood**: Excessive fprintf statements produce MBs of output
2. **Output buffer overflow**: Task output buffer fills up
3. **Timeout**: Tasks take too long due to I/O overhead
4. **SIGTERM (143)**: Task manager kills hung tasks

## Verification

### Old Binary Has Debug ❌
```bash
$ /nix/store/n1cvq67jphicc2arqbrbpvfnh55m493m-dbsql-*/bin/dbsql
# Produces: DEBUG: change_p4 addr=...
```

### New Binary is Clean ✅
```bash
$ /home/gburd/ws/dbsql/build_unix.old/.libs/dbsql
# No debug output
```

## Solution

### Option 1: Kill Background Tasks
```bash
# Find running dbsql processes
ps aux | grep dbsql

# Kill specific tasks
pkill -f "dbsql.*Test"

# Or kill all dbsql processes
pkill dbsql
```

### Option 2: Update Task Command

If tasks are defined in a configuration file, update to use absolute path:

```bash
# Bad (uses PATH lookup, finds old Nix binary)
dbsql < test.sql

# Good (uses clean local binary)
/home/gburd/ws/dbsql/build_unix.old/.libs/dbsql < test.sql
```

### Option 3: Update PATH for Background Tasks

Set PATH to prioritize local build:

```bash
export PATH="/home/gburd/ws/dbsql/build_unix.old/.libs:$PATH"
# Now background tasks will find clean binary first
```

### Option 4: Remove Old Binary

Prevent accidental use:

```bash
rm /home/gburd/ws/dbsql/result  # Removes symlink to old Nix build
```

## Impact

### Tasks Affected ❌
- Background tasks spawned by system/IDE
- Tasks using shell command lookup (`dbsql` without path)
- Tasks finding Nix binary via PATH

### Tasks Unaffected ✅
- test_debug.c - Links against clean library
- test_api.c - Will link against clean library
- Manual test execution - When using absolute paths
- run_tests.sh - Uses local build explicitly

## Recommended Cleanup

```bash
# 1. Kill any running background tasks
pkill -f dbsql

# 2. Remove old Nix build symlink
rm -f /home/gburd/ws/dbsql/result

# 3. Set PATH to prioritize local build
export PATH="/home/gburd/ws/dbsql/build_unix.old/.libs:$PATH"

# 4. Verify which dbsql is found
which dbsql
# Should show: /home/gburd/ws/dbsql/build_unix.old/.libs/dbsql

# 5. Test clean execution
echo "SELECT 1;" | dbsql :memory:
# Should NOT produce debug output
```

## Status

| Task | Status | Binary Used | Issue |
|------|--------|-------------|-------|
| b2f31b6 | ❌ FAILED (143) | Old Nix build | Debug flood + SIGTERM |
| bf6f92f | ❌ FAILED (143) | Old Nix build | Debug flood + SIGTERM |
| test_debug | ✅ PASSES | Clean local | No issues |
| test_api | ⏳ Ready | Will use clean | No issues expected |

## Prevention

To prevent future issues:

1. **Always use absolute paths** in scripts:
   ```bash
   DBSQL="/home/gburd/ws/dbsql/build_unix.old/.libs/dbsql"
   echo "SELECT 1;" | $DBSQL :memory:
   ```

2. **Set PATH in shell config**:
   ```bash
   # Add to ~/.bashrc or ~/.zshrc
   export PATH="/home/gburd/ws/dbsql/build_unix.old/.libs:$PATH"
   ```

3. **Document binary location** in README

4. **Rebuild Nix package** periodically to match source

## Timeline of Events

| Time | Event |
|------|-------|
| Pre-Mar 24 | Nix build created with debug code |
| Mar 24-26 | Source code cleaned (debug removed) |
| Mar 27 09:32 | Local build in build_unix.old/.libs/ |
| Mar 27 | Background tasks spawned using old binary |
| Mar 27 | Tasks b2f31b6 and bf6f92f fail with exit 143 |
| Mar 27 | Investigation identifies old binary as cause |

## Next Steps

1. ✅ **Identified root cause** - Old Nix binary
2. ⏳ **Kill background tasks** - Stop hung processes
3. ⏳ **Remove result symlink** - Prevent future use
4. ⏳ **Update PATH** - Prioritize local build
5. ⏳ **Re-test manually** - Using clean binary
6. ⏳ **Document in README** - Binary location and PATH setup

---

**Action Required**: Kill background tasks and remove old binary symlink.

```bash
pkill -f dbsql
rm /home/gburd/ws/dbsql/result
export PATH="/home/gburd/ws/dbsql/build_unix.old/.libs:$PATH"
```
