#!/usr/bin/env python3
"""
Clean script for Warploom project

This script provides a command-line interface for cleaning build artifacts
using the warploom_scripts.controller.clean_controller module.
"""

import sys
import argparse
from pathlib import Path

# Add parent directory to path to import warploom_scripts
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.controller.clean_controller import clean


def main():
    """Main entry point for clean script."""
    parser = argparse.ArgumentParser(
        description="Clean Warploom build artifacts",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                    # Clean all build artifacts
  %(prog)s --target engine    # Clean engine artifacts only
  %(prog)s --target game      # Clean game artifacts only
        """
    )

    parser.add_argument(
        "--target",
        choices=["engine", "game", "standalone", "all"],
        default="all",
        help="Target to clean (default: all)"
    )

    args = parser.parse_args()

    # Call the clean function from the controller
    return clean(args)


if __name__ == "__main__":
    sys.exit(main())
