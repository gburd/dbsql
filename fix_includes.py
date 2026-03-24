#!/usr/bin/env python3
"""Fix dbsql_config.h includes to use inc/ prefix"""

import os
import re
from pathlib import Path

def fix_include(file_path):
    """Fix the include directive in a single file"""
    try:
        with open(file_path, 'r') as f:
            content = f.read()

        # Check if file has the old include
        if '#include "dbsql_config.h"' not in content:
            return False

        # Replace the include
        new_content = content.replace(
            '#include "dbsql_config.h"',
            '#include "inc/dbsql_config.h"'
        )

        # Write back
        with open(file_path, 'w') as f:
            f.write(new_content)

        return True
    except Exception as e:
        print(f"Error processing {file_path}: {e}")
        return False

def main():
    # Find all .c and .y files in src/
    src_dir = Path('src')

    # Patterns to skip
    skip_patterns = ['.bak', '.backup', '.pre-fix', '.phase2', '_CONVERTED']

    updated = []

    for file_path in src_dir.rglob('*'):
        if file_path.is_file() and file_path.suffix in ['.c', '.y']:
            # Skip backup files
            if any(pattern in str(file_path) for pattern in skip_patterns):
                continue

            if fix_include(file_path):
                updated.append(str(file_path))
                print(f"Updated: {file_path}")

    print(f"\nTotal files updated: {len(updated)}")

if __name__ == '__main__':
    main()
