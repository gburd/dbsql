#!/usr/bin/env python3
"""Remove remaining dead code blocks from vdbe.c"""

import re

def remove_dead_blocks(content):
    """Remove all dead code blocks while preserving intentional debug blocks"""

    # List of (start_line_pattern, description) for blocks to remove
    # We'll find each block and remove it

    blocks_removed = 0

    # Block 4: keylistStack opcodes (OP_ListWrite, OP_ListRewind, OP_ListRead)
    pattern = r'#if 0\s+/\*\s*DISABLED - keylistStack/x\.expr_list removed.*?\*/.*?#endif\s+/\*\s*DISABLED.*?keylistStack.*?\*/'
    if re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, flags=re.DOTALL)
        blocks_removed += 1
        print("Removed Block 4: keylistStack opcodes")

    # Block 5: Legacy Sort opcodes (OP_SortPut, etc.)
    pattern = r'#if 0\s+/\*\s*DISABLED - Legacy Sort opcodes.*?\*/.*?#endif\s+/\*\s*DISABLED.*?Sort opcodes.*?\*/'
    if re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, flags=re.DOTALL)
        blocks_removed += 1
        print("Removed Block 5: Legacy Sort opcodes")

    # Block 6: Legacy callback (xCallback/azResColumn removed)
    pattern = r'#if 0\s+/\*\s*DISABLED - Legacy callback \(xCallback/azResColumn removed\).*?\*/.*?#endif\s+/\*\s*DISABLED.*?callback.*?\*/'
    if re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, flags=re.DOTALL)
        blocks_removed += 1
        print("Removed Block 6: Legacy callback (azResColumn)")

    # Block 7: Legacy File/COPY opcodes
    pattern = r'#if 0\s+/\*\s*DISABLED - Legacy File/COPY opcodes.*?\*/.*?#endif\s+/\*\s*DISABLED.*?File/COPY.*?\*/'
    if re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, flags=re.DOTALL)
        blocks_removed += 1
        print("Removed Block 7: Legacy File/COPY opcodes")

    # Block 8: Legacy aggregate opcodes (p->agg removed)
    pattern = r'#if 0\s+/\*\s*DISABLED - Legacy aggregate opcodes.*?\*/.*?#endif\s+/\*\s*DISABLED.*?aggregate opcodes.*?\*/'
    if re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, flags=re.DOTALL)
        blocks_removed += 1
        print("Removed Block 8: Legacy aggregate opcodes")

    # Block 9: Legacy stack-based OP_AggFunc
    pattern = r'#if 0\s+/\*\s*DISABLED - Legacy stack-based OP_AggFunc.*?\*/.*?#endif\s+/\*\s*DISABLED.*?OP_AggFunc.*?\*/'
    if re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, flags=re.DOTALL)
        blocks_removed += 1
        print("Removed Block 9: Legacy OP_AggFunc")

    # Block 10: Legacy Set opcodes (p->aSet removed) - first occurrence
    pattern = r'#if 0\s+/\*\s*DISABLED - Legacy Set opcodes \(p->aSet removed\).*?\*/.*?#endif\s+/\*\s*DISABLED.*?Set opcodes.*?\*/'
    if re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, flags=re.DOTALL)
        blocks_removed += 1
        print("Removed Block 10: Legacy Set opcodes")

    # Blocks 11-14: Individual aSet blocks - remove all instances
    pattern = r'#if 0\s+/\*\s*DISABLED - aSet removed in modernization.*?\*/.*?#endif\s+/\*\s*DISABLED.*?aSet.*?\*/'
    count = 0
    while re.search(pattern, content, re.DOTALL):
        content = re.sub(pattern, '', content, count=1, flags=re.DOTALL)
        count += 1
        blocks_removed += 1
    if count > 0:
        print(f"Removed Blocks 11-14: {count} individual aSet blocks")

    return content, blocks_removed

def main():
    file_path = 'src/vdbe.c'

    print(f"Reading {file_path}...")
    with open(file_path, 'r') as f:
        original = f.read()

    print("Removing remaining dead code blocks...")
    cleaned, blocks_removed = remove_dead_blocks(original)

    chars_removed = len(original) - len(cleaned)

    print(f"\nTotal blocks removed: {blocks_removed}")
    print(f"Characters removed: {chars_removed}")

    # Check remaining #if 0 blocks
    remaining_blocks = cleaned.count('#if 0')
    print(f"Remaining #if 0 blocks: {remaining_blocks}")

    if remaining_blocks > 0:
        print("\nRemaining blocks (should be intentional debug blocks):")
        for match in re.finditer(r'#if 0.*?(?:#endif|$)', cleaned, re.DOTALL):
            block_text = match.group(0)
            # Show first 150 chars
            preview = block_text[:150].replace('\n', ' ')
            print(f"  - {preview}...")

    print(f"\nWriting cleaned file...")
    with open(file_path, 'w') as f:
        f.write(cleaned)

    print("Done!")

if __name__ == '__main__':
    content = None  # Declare at module level for error handling
    main()
