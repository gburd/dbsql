#!/usr/bin/env python3
"""Remove all dead code blocks from vdbe.c"""

import re

# List of dead code blocks to remove (start line, identifying comment)
DEAD_BLOCKS = [
    # Block 3: Legacy callback opcodes around line 1039
    (r'#if 0\s+/\*\s*DISABLED - Legacy callback opcodes.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*callback.*?\*/'),

    # Block 4: Legacy callback around line 1135
    (r'#if 0\s+/\*\s*DISABLED - Legacy callback \(xCallback.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*callback.*?\*/'),

    # Block 5: Legacy stack-based OP_Function around line 1377
    (r'#if 0\s+/\*\s*DISABLED - Legacy stack-based OP_Function.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*OP_Function.*?\*/'),

    # Block 6: keylistStack around line 4428
    (r'#if 0\s+/\*\s*DISABLED - keylistStack.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*keylistStack.*?\*/'),

    # Block 7: Legacy Sort opcodes around line 4552
    (r'#if 0\s+/\*\s*DISABLED - Legacy Sort opcodes.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*Sort opcodes.*?\*/'),

    # Block 8: Legacy callback (xCallback/azResColumn) around line 4756
    (r'#if 0\s+/\*\s*DISABLED - Legacy callback \(xCallback/azResColumn.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*callback.*azResColumn.*?\*/'),

    # Block 9: Legacy File/COPY opcodes around line 4803
    (r'#if 0\s+/\*\s*DISABLED - Legacy File/COPY opcodes.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*File/COPY.*?\*/'),

    # Block 10: Legacy aggregate opcodes around line 5092
    (r'#if 0\s+/\*\s*DISABLED - Legacy aggregate opcodes.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*aggregate opcodes.*?\*/'),

    # Block 11: Legacy stack-based OP_AggFunc around line 5115
    (r'#if 0\s+/\*\s*DISABLED - Legacy stack-based OP_AggFunc.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*OP_AggFunc.*?\*/'),

    # Block 12-16: Legacy Set opcodes around line 5745 onwards
    (r'#if 0\s+/\*\s*DISABLED - Legacy Set opcodes.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*Set opcodes.*?\*/'),
    (r'#if 0\s+/\*\s*DISABLED - aSet removed in modernization.*?\*/',
     r'#endif\s+/\*\s*DISABLED.*aSet.*?\*/'),
]

def remove_dead_code_blocks(content):
    """Remove all dead code blocks from content"""

    # Process each dead block pattern
    for start_pattern, end_pattern in DEAD_BLOCKS:
        # Create a pattern that matches from #if 0 to corresponding #endif
        # Use DOTALL to match across lines
        pattern = start_pattern + r'.*?' + end_pattern
        content = re.sub(pattern, '', content, flags=re.DOTALL | re.MULTILINE)

    return content

def main():
    file_path = 'src/vdbe.c'

    print(f"Reading {file_path}...")
    with open(file_path, 'r') as f:
        original = f.read()

    print("Removing dead code blocks...")
    cleaned = remove_dead_code_blocks(original)

    # Count how many blocks were removed
    blocks_removed = original.count('#if 0') - cleaned.count('#if 0')
    chars_removed = len(original) - len(cleaned)

    print(f"Blocks removed: {blocks_removed}")
    print(f"Characters removed: {chars_removed}")

    # Verify we didn't accidentally remove the intentional debug block at line 5992
    if '#if 0' in cleaned:
        # Find remaining #if 0 blocks
        remaining = re.findall(r'#if 0.*?(?:#endif|$)', cleaned, re.DOTALL)
        print(f"\nRemaining #if 0 blocks: {len(remaining)}")
        for i, block in enumerate(remaining):
            preview = block[:200] + '...' if len(block) > 200 else block
            print(f"\nBlock {i+1}:")
            print(preview)

    print(f"\nWriting cleaned file...")
    with open(file_path, 'w') as f:
        f.write(cleaned)

    print("Done!")

if __name__ == '__main__':
    main()
