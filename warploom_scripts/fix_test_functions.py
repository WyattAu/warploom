#!/usr/bin/env python3
"""
Fix malformed function definitions in test files.
"""

import re
from pathlib import Path

def fix_test_functions(file_path: Path) -> None:
    """Fix malformed function definitions in a test file."""
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Pattern to match malformed function definitions
    # Matches: def test_() -> None:() -> bool:function_name(self, ...) -> None:
    # And replaces with: def test_function_name(self, ...) -> None:
    pattern = r'def test_\(\) -> None:\(\) -> bool:(\w+)\((.*?)\) -> None:'
    replacement = r'def test_\1(\2) -> None:'

    # Apply the fix
    fixed_content = re.sub(pattern, replacement, content)

    # Write back
    with open(file_path, 'w', encoding='utf-8') as f:
        f.write(fixed_content)

    print(f"Fixed {file_path}")

if __name__ == '__main__':
    # Fix all test files
    tests_dir = Path(__file__).parent / 'tests'
    for test_file in tests_dir.glob('test_*.py'):
        print(f"Processing {test_file}...")
        fix_test_functions(test_file)
