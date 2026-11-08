#!/usr/bin/env python3
"""
Package script for Warploom project

This script provides a command-line interface for creating distribution packages
using the warploom_scripts.controller.package_controller module.
"""

import sys
import argparse
from pathlib import Path

# Add parent directory to path to import warploom_scripts
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.controller.package_controller import package


def main():
    """Main entry point for package script."""
    parser = argparse.ArgumentParser(
        description="Create Warploom distribution packages",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                    # Package all targets (release)
  %(prog)s --target engine    # Package engine only
  %(prog)s --config debug     # Package debug configuration
  %(prog)s --format ZIP       # Create ZIP package
  %(prog)s --output-dir dist  # Output to custom directory
        """
    )

    parser.add_argument(
        "--target",
        choices=["engine", "game", "standalone", "all"],
        default="all",
        help="Target to package (default: all)"
    )

    parser.add_argument(
        "--config",
        choices=["debug", "release"],
        default="release",
        help="Build configuration (default: release)"
    )

    parser.add_argument(
        "--format",
        choices=["ZIP", "TGZ", "7Z", "DEB", "RPM", "NSIS", "WIX", "CYGWIN", "PACKAGEMAKER", "OSXX11", "NUGET", "FREEBSD"],
        help="Package format (default: platform-specific)"
    )

    parser.add_argument(
        "--output-dir",
        help="Output directory for packages"
    )

    args = parser.parse_args()

    # Call the package function from the controller
    return package(args)


if __name__ == "__main__":
    sys.exit(main())
