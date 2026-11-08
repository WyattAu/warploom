#!/usr/bin/env python3
"""
Lint script for Warploom project

This script provides a command-line interface for linting code
using the warploom_scripts.controller.lint_controller module.
"""

import sys
import argparse
from pathlib import Path

# Add parent directory to path to import warploom_scripts
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.controller.lint_controller import lint


def main():
    """Main entry point for lint script."""
    parser = argparse.ArgumentParser(
        description="Lint Warploom code with clang-tidy and pylint",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                    # Lint all C++ and Python files
  %(prog)s --fix              # Apply automatic fixes
  %(prog)s --cpp-only         # Lint only C++ files
  %(prog)s --python-only      # Lint only Python files
  %(prog)s src/main.cpp       # Lint specific file
  %(prog)s src/               # Lint all files in directory
        """
    )

    parser.add_argument(
        "files",
        nargs="*",
        help="Specific files to lint (optional)"
    )

    parser.add_argument(
        "--directories",
        nargs="*",
        help="Directories to scan for files (optional)"
    )

    parser.add_argument(
        "--fix",
        action="store_true",
        help="Apply automatic fixes"
    )

    parser.add_argument(
        "--cpp-only",
        action="store_true",
        help="Lint only C++ files"
    )

    parser.add_argument(
        "--python-only",
        action="store_true",
        help="Lint only Python files"
    )

    args = parser.parse_args()

    # Call the lint function from the controller
    return lint(args)


if __name__ == "__main__":
    sys.exit(main())
