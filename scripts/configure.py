#!/usr/bin/env python3
"""
Configure Script

Configures the build system with CMake, including support for
build type, generator, toolchain, and preset options.
"""

from __future__ import annotations

import argparse
import sys

from warploom_scripts.controller.configure_controller import configure


def main() -> int:
    """Main entry point for configure script.

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    parser = argparse.ArgumentParser(
        description="Configure Warploom build system with CMake",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    python configure.py --preset debug
    python configure.py --preset msvc-debug
    python configure.py --build-type Debug --generator "Ninja"
    python configure.py --toolchain cmake/toolchains/emscripten.cmake
        """,
    )

    parser.add_argument(
        "--build-type",
        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
        default="Release",
        help="Build configuration (default: Release)",
    )
    parser.add_argument(
        "--generator",
        help="CMake generator (e.g., 'Ninja', 'Visual Studio 17 2022')",
    )
    parser.add_argument(
        "--toolchain",
        help="Path to CMake toolchain file",
    )
    parser.add_argument(
        "--preset",
        help="CMake preset name from CMakePresets.json",
    )
    parser.add_argument(
        "--configure-conan",
        action="store_true",
        help="Configure Conan dependencies",
    )
    parser.add_argument(
        "--configure-vcpkg",
        action="store_true",
        help="Configure vcpkg dependencies",
    )

    args = parser.parse_args()

    # Call configure function from controller
    return configure(args)


if __name__ == "__main__":
    sys.exit(main())
