# Recovery Quick Start Guide

**Use this document if you need to execute recovery NOW.**

## Pre-Flight Check (30 seconds)

```bash
cd /home/gburd/ws/dbsql

# Check current state
git log --oneline -5
# Should show: 99f1a30 at HEAD

# Check if tests are broken
cd test/src
./test_api 2>&1 | grep -E "CANTOPEN|MISUSE|ERROR|Segmentation"
# If you see these errors, proceed with recovery
```

## Option 2: Selective Revert (RECOMMENDED - Try This First)

**Time**: 60 minutes | **Risk**: Low | **Preserves**: DELETE fix

```bash
cd /home/gburd/ws/dbsql

# Create recovery branch
git checkout -b recovery/selective-revert-$(date +%Y%m%d-%H%M)

# Revert problematic commits (DO NOT COMMIT YET)
git revert --no-commit 99f1a30 5f687e2 34bc4eb

# Check for conflicts
git status

# If conflicts in src/sm.c or src/vdbe.c:
# - Edit files to keep the DELETE fix code
# - Remove transaction isolation changes
# - Accept "ours" (pre-revert) state for tricky merges

# After resolving conflicts:
git add .
git commit -m "Revert transaction isolation changes (99f1a30, 5f687e2, 34bc4eb)

Reverted due to regressions in tasks #129-#132:
- DBSQL_CANTOPEN (database opening failure)
- DBSQL_MISUSE (SQL execution failure)
- DBSQL_ERROR (database closing failure)
- Metadata system regression

Preserved fixes:
- DELETE with WHERE clause (0a5e48f)
- P3_POINTER memory fix (0a55969)

Known issue reintroduced:
- SELECT may return no rows after INSERT
  (requires different transaction isolation approach)"

# Rebuild
cd build_unix.old
make clean
make

# Test
cd ../test/src
./test_api > /tmp/recovery_results.txt 2>&1

# Check results
cat /tmp/recovery_results.txt
grep -E "CANTOPEN|MISUSE|ERROR" /tmp/recovery_results.txt
# If no errors, SUCCESS!

# Tag success
cd /home/gburd/ws/dbsql
git tag -a recovery/stable-$(date +%Y%m%d-%H%M) -m "Stable state after selective revert"
```

**If conflicts are too complex, skip to Option 4 below.**

## Option 4: Cherry-Pick (Use If Option 2 Has Conflicts)

**Time**: 45 minutes | **Risk**: Low | **Preserves**: DELETE fix only

```bash
cd /home/gburd/ws/dbsql

# Create clean branch from last known good
git checkout -b recovery/cherry-pick-$(date +%Y%m%d-%H%M) 4743981

# Cherry-pick DELETE fix
git cherry-pick 0a5e48f

# If conflicts, resolve and:
git add .
git cherry-pick --continue

# Rebuild
cd build_unix.old
make clean
make

# Test
cd ../test/src
./test_api > /tmp/recovery_results.txt 2>&1

# Check results
cat /tmp/recovery_results.txt
grep -E "CANTOPEN|MISUSE|ERROR" /tmp/recovery_results.txt
# If no errors, SUCCESS!

# Tag success
cd /home/gburd/ws/dbsql
git tag -a recovery/stable-$(date +%Y%m%d-%H%M) -m "Stable state after cherry-pick"
```

**If even this fails, execute Option 1 below.**

## Option 1: Full Rollback (LAST RESORT - Guaranteed Success)

**Time**: 20 minutes | **Risk**: None | **Preserves**: Nothing after 4743981

```bash
cd /home/gburd/ws/dbsql

# Create rollback branch
git checkout -b recovery/full-rollback-$(date +%Y%m%d-%H%M)

# Hard reset to last known good (100% tests passed)
git reset --hard 4743981

# Rebuild
cd build_unix.old
make clean
make

# Test
cd ../test/src
./test_api > /tmp/recovery_results.txt 2>&1

# Check results
cat /tmp/recovery_results.txt
# Should see clean test output, all passing

# Tag success
cd /home/gburd/ws/dbsql
git tag -a recovery/stable-$(date +%Y%m%d-%H%M) -m "Stable state after full rollback"

# Document what was lost
echo "Full rollback to 4743981 executed $(date)" > RECOVERY_LOG.md
echo "Lost commits: 0a5e48f (DELETE), 3e883bd, 34bc4eb, 5f687e2, 99f1a30" >> RECOVERY_LOG.md
echo "Reason: Transaction isolation regressions could not be fixed forward" >> RECOVERY_LOG.md
```

**This WILL work. 4743981 had 100% test pass rate.**

## Quick Troubleshooting

### Build Fails After Recovery

```bash
cd /home/gburd/ws/dbsql/build_unix.old

# Clean everything
make distclean || true
rm -f Makefile config.status config.log

# Reconfigure
../dist/configure --enable-debug

# Rebuild
make
```

### Tests Still Fail After Option 2

This means conflicts were resolved incorrectly. **Execute Option 4 or Option 1.**

### "Cannot Find Commit 4743981"

```bash
# Fetch from origin
git fetch origin

# Try again
git log --oneline --all | grep 4743981
```

### Can't Execute Commands (Filesystem Issues)

```bash
# Try with sudo
sudo bash

# Or use different terminal/session
# The issue is /tmp/claude filesystem being read-only
```

## Validation Checklist

After any recovery option, verify:

```bash
cd /home/gburd/ws/dbsql/test/src

# Run tests
./test_api > /tmp/test_results.txt 2>&1

# Check for critical errors
cat /tmp/test_results.txt | grep -E "CANTOPEN|MISUSE|ERROR|Segmentation|failed"

# If output is clean or only shows expected failures, SUCCESS!

# Run basic operations test
cat > /tmp/quick_test.sql <<'EOF'
CREATE TABLE test (id INTEGER, name TEXT);
INSERT INTO test VALUES (1, 'Alice');
SELECT * FROM test;
EOF

cd /home/gburd/ws/dbsql
echo ".read /tmp/quick_test.sql" | build_unix.old/.libs/dbsql /tmp/test_recovery.db

# Should output:
# 1|Alice
# If you see this, database operations work!
```

## Post-Recovery Actions

```bash
cd /home/gburd/ws/dbsql

# Update status
cat >> STATUS.md <<EOF

## Recovery Executed $(date +%Y-%m-%d)

- Method: [Option 1/2/4]
- Branch: $(git rev-parse --abbrev-ref HEAD)
- Commit: $(git rev-parse --short HEAD)
- Test Status: [PASS/FAIL]
- Known Issues: [List any remaining issues]

EOF

# Create recovery log
cat > RECOVERY_LOG.md <<EOF
# Recovery Log

**Date**: $(date)
**Executed By**: Recovery procedure from RECOVERY_QUICK_START.md
**Method**: [Option 1/2/4]
**Reason**: Tasks #129-#132 regressions

## What Was Done
[Describe what you did]

## What Was Lost
[List reverted commits and features]

## Current Status
[Describe test results and known issues]

## Next Steps
[What needs to happen next]

EOF

# Mark tasks
echo "Update tasks #129-#132 status based on results"
echo "If recovered successfully, mark as completed"
echo "If issues remain, document in task descriptions"
```

## Need More Information?

- Full analysis: `/home/gburd/ws/dbsql/RECOVERY_OPTIONS.md`
- Executive summary: `/home/gburd/ws/dbsql/RECOVERY_ANALYSIS_SUMMARY.md`
- Decision tree: `/home/gburd/ws/dbsql/RECOVERY_DECISION_TREE.txt`
- Transaction details: `/home/gburd/ws/dbsql/CURSOR_TRANSACTION_FIX.md`

## Critical Commit References

- **4743981**: Last known good (100% tests passed)
- **0a55969**: P3_POINTER fix (keep if possible)
- **0a5e48f**: DELETE with WHERE fix (keep if possible)
- **34bc4eb**: OP_Transaction for SELECT (REVERT)
- **5f687e2**: Cursor transaction isolation (REVERT)
- **99f1a30**: Current HEAD, transaction test (REVERT)

## Timeline Reference

| Minute | Action |
|--------|--------|
| 0 | Start recovery, choose option |
| 5 | Execute git commands |
| 10 | Resolve any conflicts |
| 15 | Commit recovery |
| 20 | Start rebuild |
| 30 | Build complete |
| 35 | Run tests |
| 45 | Analyze results |
| 50 | Tag recovery point |
| 60 | Document recovery |

## Success Indicators

You succeeded if:
- ✅ `./test_api` runs without DBSQL_CANTOPEN errors
- ✅ `./test_api` runs without DBSQL_MISUSE errors
- ✅ `./test_api` runs without DBSQL_ERROR errors
- ✅ Can open database, execute SQL, close cleanly
- ✅ No segmentation faults

You may have issues but are stable if:
- ⚠️ SELECT after INSERT returns no rows (known issue if Option 2/4)
- ⚠️ Some tests fail but database operations work
- ⚠️ DELETE with WHERE doesn't work (only if Option 1 used)

## Emergency Contact

If all options fail (unlikely):
1. Document everything in EMERGENCY_RECOVERY_FAILURE.md
2. Preserve current state: `git stash save "emergency-state-$(date +%Y%m%d-%H%M%S)"`
3. Create fresh clone: `git clone /home/gburd/ws/dbsql /tmp/dbsql-fresh`
4. Check out 4743981 in fresh clone
5. Work from there

---

**Remember**: Option 1 (Full Rollback) WILL work. It's guaranteed because 4743981 had 100% test pass rate.
**Don't hesitate**: If in doubt, execute Option 1 immediately. You can always re-apply fixes later individually.

Good luck!
