#!/usr/bin/env python3
"""
Fix type annotations in test files by adding type hints for mock parameters.
"""

import re
from pathlib import Path
from typing import List, Match

# Files to fix
TEST_FILES = [
    "warploom_scripts/tests/test_build_system_integration.py",
    "warploom_scripts/tests/test_controller_integration.py",
    "warploom_scripts/tests/test_logging_integration.py",
    "warploom_scripts/tests/test_cross_platform_integration.py",
]

def fix_test_file(file_path: str) -> None:
    """Fix type annotations in a test file."""
    path = Path(file_path)
    if not path.exists():
        print(f"File not found: {file_path}")
        return

    content = path.read_text(encoding='utf-8')
    original_content = content

    # Pattern to find test methods with mock parameters
    # Match: def test_xxx(self, mock_name) -> None:
    pattern = r'(def test_\w+\(self,\s+)(\w+)(\)\s*->\s*None:)'

    def replace_func(match: Match[str]) -> str:
        """Replace with type annotation."""
        prefix = match.group(1)
        param_name = match.group(2)
        suffix = match.group(3)
        return f'{prefix}{param_name}: Any{suffix}'

    # Apply replacement
    content = re.sub(pattern, replace_func, content)

    # Also fix caplog parameter
    caplog_pattern = r'(def test_\w+\(self,\s+)(caplog)(\)\s*->\s*None:)'
    content = re.sub(caplog_pattern, replace_func, content)

    # Add Any import if not present
    if 'from typing import' in content and 'Any' not in content:
        # Find the typing import line
        import_pattern = r'(from typing import [^\n]+)'
        import_match = re.search(import_pattern, content)
        if import_match:
            old_import = import_match.group(1)
            new_import = old_import.rstrip() + ', Any'
            content = content.replace(old_import, new_import)

    # Write back if changed
    if content != original_content:
        path.write_text(content, encoding='utf-8')
        print(f"Fixed: {file_path}")
    else:
        print(f"No changes needed: {file_path}")

def main() -> None:
    """Main function."""
    for test_file in TEST_FILES:
        fix_test_file(test_file)

if __name__ == '__main__':
    main()
