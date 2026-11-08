#!/usr/bin/env python3
"""
Format script for Warploom project

This script provides a command-line interface for formatting code
using the warploom_scripts.controller.format_controller module.
"""

import sys
import argparse
from pathlib import Path

# Add parent directory to path to import warploom_scripts
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.controller.format_controller import format_code


def main():
    """Main entry point for format script."""
    parser = argparse.ArgumentParser(
        description="Format Warploom code with clang-format and black",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                    # Format all C++ and Python files
  %(prog)s --check            # Check formatting without modifying
  %(prog)s --cpp-only         # Format only C++ files
  %(prog)s --python-only      # Format only Python files
  %(prog)s src/main.cpp       # Format specific file
  %(prog)s src/               # Format all files in directory
        """
    )

    parser.add_argument(
        "files",
        nargs="*",
        help="Specific files to format (optional)"
    )

    parser.add_argument(
        "--directories",
        nargs="*",
        help="Directories to scan for files (optional)"
    )

    parser.add_argument(
        "--check",
        action="store_true",
        help="Check formatting without modifying files"
    )

    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Run in dry-run mode (show changes without applying)"
    )

    parser.add_argument(
        "--cpp-only",
        action="store_true",
        help="Format only C++ files"
    )

    parser.add_argument(
        "--python-only",
        action="store_true",
        help="Format only Python files"
    )

    args = parser.parse_args()

    # Call the format_code function from the controller
    return format_code(args)


if __name__ == "__main__":
    sys.exit(main())
