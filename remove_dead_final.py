#!/usr/bin/env python3
"""Remove dead code blocks from vdbe.c - handles #if 0 ... #else ... #endif patterns"""

import re

def find_and_remove_blocks(content):
    """Find and remove all #if 0 DISABLED blocks"""

    blocks_removed = 0
    lines = content.split('\n')
    result_lines = []
    i = 0

    while i < len(lines):
        line = lines[i]

        # Check if this is a #if 0 DISABLED block
        if line.strip().startswith('#if 0') and 'DISABLED' in line:
            # Check if it's the intentional debug block (has fprintf/vdbe_print_op)
            # Look ahead to see block content
            is_debug_block = False
            if i + 1 < len(lines) and 'fprintf' in lines[i+1]:
                is_debug_block = True

            if is_debug_block:
                # Keep intentional debug blocks
                result_lines.append(line)
                i += 1
                continue

            # This is a block to remove
            # Find the corresponding #endif or #else
            depth = 1
            block_start = i
            else_line = None

            i += 1
            while i < len(lines) and depth > 0:
                if lines[i].strip().startswith('#if'):
                    depth += 1
                elif lines[i].strip().startswith('#else') and depth == 1:
                    else_line = i
                elif lines[i].strip().startswith('#endif'):
                    depth -= 1
                    if depth == 0:
                        # Found the closing #endif
                        if else_line is not None:
                            # There's an #else clause - keep it, remove markers
                            # Add lines from #else to #endif (excluding the markers)
                            for j in range(else_line + 1, i):
                                result_lines.append(lines[j])
                        # Skip the #endif
                        blocks_removed += 1
                        print(f"Removed block at line {block_start + 1}: {lines[block_start][:80]}")
                        break
                i += 1
        else:
            result_lines.append(line)

        i += 1

    return '\n'.join(result_lines), blocks_removed

def main():
    file_path = 'src/vdbe.c'

    print(f"Reading {file_path}...")
    with open(file_path, 'r') as f:
        original = f.read()

    original_if0_count = original.count('#if 0')
    print(f"Original #if 0 blocks: {original_if0_count}")

    print("\nRemoving DISABLED blocks...")
    cleaned, blocks_removed = find_and_remove_blocks(original)

    remaining_if0_count = cleaned.count('#if 0')
    chars_removed = len(original) - len(cleaned)

    print(f"\nBlocks removed: {blocks_removed}")
    print(f"Characters removed: {chars_removed}")
    print(f"Remaining #if 0 blocks: {remaining_if0_count}")

    if remaining_if0_count > 0:
        print("\nRemaining #if 0 blocks (should be intentional debug blocks):")
        for match in re.finditer(r'#if 0.*', cleaned):
            line_start = cleaned[:match.start()].count('\n') + 1
            preview = match.group(0)[:100]
            print(f"  Line {line_start}: {preview}")

    print(f"\nWriting cleaned file...")
    with open(file_path, 'w') as f:
        f.write(cleaned)

    print("Done!")

if __name__ == '__main__':
    main()
