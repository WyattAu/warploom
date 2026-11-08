#!/usr/bin/env python3
"""
Build script for Warploom project

This script provides a command-line interface for building project targets
using the warploom_scripts.controller.build_controller module.
"""

import sys
import argparse
from pathlib import Path

# Add parent directory to path to import warploom_scripts
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.controller.build_controller import build


def main():
    """Main entry point for build script."""
    parser = argparse.ArgumentParser(
        description="Build Warploom project targets",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                    # Build all targets (release)
  %(prog)s --target engine    # Build engine only
  %(prog)s --config debug     # Build debug configuration
  %(prog)s --clean            # Clean before building
  %(prog)s --parallel 4       # Use 4 parallel jobs
        """
    )

    parser.add_argument(
        "--target",
        choices=["engine", "game", "standalone", "all"],
        default="all",
        help="Target to build (default: all)"
    )

    parser.add_argument(
        "--config",
        choices=["debug", "release"],
        default="release",
        help="Build configuration (default: release)"
    )

    parser.add_argument(
        "--compiler",
        help="Compiler to use (e.g., msvc, gcc, clang)"
    )

    parser.add_argument(
        "--clean",
        action="store_true",
        help="Clean build artifacts before building"
    )

    parser.add_argument(
        "--parallel",
        type=int,
        help="Number of parallel jobs (default: auto-detect)"
    )

    parser.add_argument(
        "--preset",
        default="default",
        help="CMake preset to use (default: default)"
    )

    parser.add_argument(
        "--pipeline",
        default="default",
        help="Build pipeline name (default: default)"
    )

    args = parser.parse_args()

    # Call the build function from the controller
    return build(args)


if __name__ == "__main__":
    sys.exit(main())
