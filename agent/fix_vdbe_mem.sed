#!/usr/bin/sed -f
# Fix mem_t structure field accesses for SQLite 3.52.0 modernization
#
# 1. Fix ->i accesses (but not ->int, ->isXxx, ->iDb, ->iKey, etc.)
# Match ->i followed by space, operator, semicolon, or closing paren
s/\(->i\)\([ ;)=!<>+\-*/%&|]\)/->u.i\2/g

# 2. Fix ->r accesses (but not ->rc, ->real, ->reg, ->rowid, etc.)
# Match ->r followed by space, operator, semicolon, or closing paren
s/\(->r\)\([ ;)=!<>+\-*/%]\)/->u.r\2/g

# 3. Remove all zShort references - they need manual handling
# We'll leave these for manual inspection since they need different logic
