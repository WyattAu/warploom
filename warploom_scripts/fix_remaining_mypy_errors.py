#!/usr/bin/env python3
"""
Fix remaining MyPy type errors systematically.
"""

import re
from pathlib import Path
from typing import List, Tuple

def remove_unused_type_ignores(content: str) -> str:
    """Remove unused type: ignore comments."""
    # Remove unused type: ignore comments
    content = re.sub(r'  # type: ignore\[unused-ignore\]', '', content)
    content = re.sub(r'  # type: ignore\[attr-defined\]', '', content)
    content = re.sub(r'  # type: ignore\[no-redef\]', '', content)
    content = re.sub(r'  # type: ignore\[no-any-return\]', '', content)
    content = re.sub(r'  # type: ignore\[assignment\]', '', content)
    return content

def add_return_type_to_test_functions(content: str) -> str:
    """Add return type annotations to test functions."""
    # Pattern to match test functions without return type
    # Matches: def test_function_name(self, arg1, arg2):
    # And replaces with: def test_function_name(self, arg1, arg2) -> None:
    pattern = r'(    def test_\w+\([^)]*\)):'
    replacement = r'\1 -> None:'
    return re.sub(pattern, replacement, content)

def add_return_type_to_functions(content: str) -> str:
    """Add return type annotations to functions."""
    # Pattern to match functions without return type (4-space indent)
    # Matches: def function_name(self, arg1, arg2):
    # And replaces with: def function_name(self, arg1, arg2) -> None:
    pattern = r'(    def \w+\([^)]*\)):'
    replacement = r'\1 -> None:'
    return re.sub(pattern, replacement, content)

def add_type_annotation_to_args(content: str) -> str:
    """Add type annotations to function arguments."""
    # Pattern to match functions with untyped arguments
    # This is a simple heuristic - add Any type to untyped args
    # Matches: def function_name(self, arg1, arg2):
    # And replaces with: def function_name(self, arg1: Any, arg2: Any):
    # Only for args that are not self
    pattern = r'(    def \w+\([^)]*),\s*(\w+)\):'
    replacement = r'\1, \2: Any):'
    return re.sub(pattern, replacement, content)

def fix_file(file_path: Path) -> Tuple[int, int]:
    """Fix a single file and return (fixes_applied, errors_remaining)."""
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()

    original_content = content

    # Apply fixes in order
    content = remove_unused_type_ignores(content)
    content = add_return_type_to_test_functions(content)
    content = add_return_type_to_functions(content)
    content = add_type_annotation_to_args(content)

    # Write back if changed
    if content != original_content:
        with open(file_path, 'w', encoding='utf-8') as f:
            f.write(content)
        return (1, 0)
    else:
        return (0, 0)

if __name__ == '__main__':
    # Fix all Python files in warploom_scripts
    warploom_scripts_dir = Path(__file__).parent
    total_fixes = 0

    for py_file in warploom_scripts_dir.rglob('*.py'):
        # Skip the fix script itself
        if py_file.name.startswith('fix_'):
            continue

        fixes, _ = fix_file(py_file)
        total_fixes += fixes
        if fixes > 0:
            print(f"Fixed {py_file}")

    print(f"\nTotal files fixed: {total_fixes}")
