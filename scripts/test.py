#!/usr/bin/env python3
"""
Test script for Warploom project

This script provides a command-line interface for running tests
using the warploom_scripts.controller.test_controller module.
"""

import sys
import argparse
from pathlib import Path

# Add parent directory to path to import warploom_scripts
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.controller.test_controller import TestController


def main():
    """Main entry point for test script."""
    parser = argparse.ArgumentParser(
        description="Run Warploom tests",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                    # Run all tests
  %(prog)s --verbose          # Run tests with verbose output
  %(prog)s --filter unit      # Run only unit tests
        """
    )

    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable verbose output"
    )

    parser.add_argument(
        "--filter",
        help="Filter tests by name or pattern"
    )

    args = parser.parse_args()

    # Create and execute the test controller
    controller = TestController(args)
    return controller.execute()


if __name__ == "__main__":
    sys.exit(main())
