"""
CLI argument parser for WarploomController.

This module provides the argument parser for the command-line interface,
including all commands, subcommands, and their arguments with help text
and usage examples.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any, Optional

from warploom_scripts.controller.base import BaseController


def create_parser() -> argparse.ArgumentParser:
    """Create the main argument parser for the controller.

    Returns:
        The configured argument parser with all subcommands.
    """
    parser = argparse.ArgumentParser(
        prog="WarploomController",
        description="Warploom build system controller - Build, test, package, and manage C++ projects",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Configure the build system
  python WarploomController.py configure --preset default

  # Build the engine with debug configuration
  python WarploomController.py build engine default default debug

  # Build with specific compiler
  python WarploomController.py build game default default release --compiler msvc

  # Clean all build artifacts
  python WarploomController.py clean

  # Clean specific target
  python WarploomController.py clean --target engine

  # Install build artifacts
  python WarploomController.py install standalone release

  # Run tests
  python WarploomController.py test engine debug

  # Create distribution package
  python WarploomController.py package standalone release

  # Format code (check only)
  python WarploomController.py format --check

  # Format specific files
  python WarploomController.py format --files src/main.cpp include/engine.hpp

  # Format only C++ files
  python WarploomController.py format --cpp-only

  # Lint code with auto-fix
  python WarploomController.py lint --fix

  # Lint specific directories
  python WarploomController.py lint --directories src include

  # Lint only Python files
  python WarploomController.py lint --python-only

For more information on a specific command, use:
  python WarploomController.py <command> --help
        """,
    )

    parser.add_argument(
        "--version",
        action="version",
        version="%(prog)s 0.1.1",
        help="Show version information and exit",
    )

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable verbose output (DEBUG level logging)",
    )

    # Create subparsers for commands
    subparsers = parser.add_subparsers(
        dest="command",
        title="Available commands",
        description="Use one of the following commands",
        required=True,
        metavar="<command>",
    )

    # Add all command subparsers
    _add_configure_command(subparsers)
    _add_build_command(subparsers)
    _add_clean_command(subparsers)
    _add_install_command(subparsers)
    _add_test_command(subparsers)
    _add_package_command(subparsers)
    _add_format_command(subparsers)
    _add_lint_command(subparsers)

    return parser


def _add_configure_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the configure command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "configure",
        help="Configure the build system with CMake",
        description="""
Configure the build system using CMake. This command sets up the build
directory, generates build files, and prepares the project for compilation.

At least one of --generator, --toolchain, or --preset must be specified.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Configure with a preset
  python WarploomController.py configure --preset default

  # Configure with specific generator and build type
  python WarploomController.py configure --generator "Ninja" --build-type Release

  # Configure with toolchain file
  python WarploomController.py configure --toolchain cmake/toolchains/emscripten.cmake

  # Configure with all options
  python WarploomController.py configure --preset default --build-type Debug
        """,
    )

    parser.add_argument(
        "--build-type",
        choices=BaseController.VALID_BUILD_TYPES,
        default="Release",
        help="CMake build type (default: Release)",
    )

    parser.add_argument(
        "--generator",
        type=str,
        help="CMake generator name (e.g., 'Ninja', 'Visual Studio 17 2022')",
    )

    parser.add_argument(
        "--toolchain",
        type=Path,
        help="Path to CMake toolchain file",
    )

    parser.add_argument(
        "--preset",
        type=str,
        help="CMake preset name",
    )

    parser.add_argument(
        "--compiler",
        choices=BaseController.VALID_COMPILERS,
        help="Compiler to use (auto-detected if not specified)",
    )


def _add_build_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the build command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "build",
        help="Build the project",
        description="""
Build the project using the specified target, pipeline, preset, and configuration.
This command compiles the source code and generates build artifacts.

The compiler is auto-detected if not specified. Use --clean to clean
before building.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Build engine with debug configuration
  python WarploomController.py build engine default default debug

  # Build game with release configuration
  python WarploomController.py build game default default release

  # Build with specific compiler
  python WarploomController.py build standalone default default debug --compiler msvc

  # Clean before building
  python WarploomController.py build engine default default release --clean

  # Build all targets
  python WarploomController.py build all default default debug
        """,
    )

    parser.add_argument(
        "target",
        choices=BaseController.VALID_TARGETS,
        help="Target to build (engine, game, standalone, or all)",
    )

    parser.add_argument(
        "pipeline",
        type=str,
        help="Build pipeline name",
    )

    parser.add_argument(
        "preset",
        type=str,
        help="CMake preset name",
    )

    parser.add_argument(
        "config",
        choices=BaseController.VALID_CONFIGS,
        help="Build configuration (debug or release)",
    )

    parser.add_argument(
        "--compiler",
        choices=BaseController.VALID_COMPILERS,
        help="Compiler to use (auto-detected if not specified)",
    )

    parser.add_argument(
        "--clean",
        action="store_true",
        help="Clean build artifacts before building",
    )


def _add_clean_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the clean command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "clean",
        help="Clean build artifacts",
        description="""
Clean build artifacts for the specified target. If no target is specified,
all targets are cleaned.

This command removes build directories, intermediate files, and other
build artifacts.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Clean all targets
  python WarploomController.py clean

  # Clean specific target
  python WarploomController.py clean --target engine

  # Clean game target
  python WarploomController.py clean --target game
        """,
    )

    parser.add_argument(
        "--target",
        choices=BaseController.VALID_TARGETS,
        help="Target to clean (default: all)",
    )


def _add_install_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the install command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "install",
        help="Install build artifacts",
        description="""
Install build artifacts for the specified target and configuration.
This command copies compiled binaries, libraries, and other files
to the installation directory.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Install standalone with release configuration
  python WarploomController.py install standalone release

  # Install engine with debug configuration
  python WarploomController.py install engine debug

  # Install game with release configuration
  python WarploomController.py install game release

  # Install all targets
  python WarploomController.py install all release
        """,
    )

    parser.add_argument(
        "target",
        choices=BaseController.VALID_TARGETS,
        help="Target to install (engine, game, standalone, or all)",
    )

    parser.add_argument(
        "config",
        choices=BaseController.VALID_CONFIGS,
        help="Build configuration (debug or release)",
    )

    parser.add_argument(
        "--compiler",
        choices=BaseController.VALID_COMPILERS,
        help="Compiler to use (auto-detected if not specified)",
    )


def _add_test_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the test command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "test",
        help="Run tests",
        description="""
Run tests for the specified target and configuration.
This command executes unit tests and integration tests using CTest.

Additional test filtering options can be added in the future.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run tests for engine with debug configuration
  python WarploomController.py test engine debug

  # Run tests for game with release configuration
  python WarploomController.py test game release

  # Run tests for standalone
  python WarploomController.py test standalone debug

  # Run tests for all targets
  python WarploomController.py test all release
        """,
    )

    parser.add_argument(
        "target",
        choices=BaseController.VALID_TARGETS,
        help="Target to test (engine, game, standalone, or all)",
    )

    parser.add_argument(
        "config",
        choices=BaseController.VALID_CONFIGS,
        help="Build configuration (debug or release)",
    )

    parser.add_argument(
        "--compiler",
        choices=BaseController.VALID_COMPILERS,
        help="Compiler to use (auto-detected if not specified)",
    )


def _add_package_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the package command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "package",
        help="Create distribution packages",
        description="""
Create distribution packages for the specified target and configuration.
This command uses CPack to create installable packages in various formats.

Additional package format options can be added in the future.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Package standalone with release configuration
  python WarploomController.py package standalone release

  # Package engine with debug configuration
  python WarploomController.py package engine debug

  # Package game with release configuration
  python WarploomController.py package game release

  # Package all targets
  python WarploomController.py package all release
        """,
    )

    parser.add_argument(
        "target",
        choices=BaseController.VALID_TARGETS,
        help="Target to package (engine, game, standalone, or all)",
    )

    parser.add_argument(
        "config",
        choices=BaseController.VALID_CONFIGS,
        help="Build configuration (debug or release)",
    )

    parser.add_argument(
        "--compiler",
        choices=BaseController.VALID_COMPILERS,
        help="Compiler to use (auto-detected if not specified)",
    )


def _add_format_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the format command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "format",
        help="Format code with clang-format and black",
        description="""
Format code using clang-format for C++ files and black for Python files.
This command ensures consistent code style across the project.

If no files or directories are specified, the current directory is scanned.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Format all files in current directory
  python WarploomController.py format

  # Check formatting without modifying files
  python WarploomController.py format --check

  # Format specific files
  python WarploomController.py format --files src/main.cpp include/engine.hpp

  # Format specific directories
  python WarploomController.py format --directories src include

  # Format only C++ files
  python WarploomController.py format --cpp-only

  # Format only Python files
  python WarploomController.py format --python-only

  # Dry run (show what would be formatted)
  python WarploomController.py format --dry-run

  # Check and format specific files
  python WarploomController.py format --files src/main.cpp --check
        """,
    )

    parser.add_argument(
        "--files",
        nargs="+",
        type=Path,
        help="Specific files to format",
    )

    parser.add_argument(
        "--directories",
        nargs="+",
        type=Path,
        help="Directories to scan for files to format",
    )

    parser.add_argument(
        "--check",
        action="store_true",
        help="Only check formatting without modifying files",
    )

    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Run in dry-run mode (show what would be formatted)",
    )

    parser.add_argument(
        "--cpp-only",
        action="store_true",
        help="Only format C++ files",
    )

    parser.add_argument(
        "--python-only",
        action="store_true",
        help="Only format Python files",
    )


def _add_lint_command(subparsers: argparse._SubParsersAction[Any]) -> None:
    """Add the lint command subparser.

    Args:
        subparsers: The subparsers object to add the command to.
    """
    parser = subparsers.add_parser(
        "lint",
        help="Run static analysis with clang-tidy, pylint, and mypy",
        description="""
Run static analysis tools to check code quality and identify potential issues.
This command uses clang-tidy for C++ files and pylint/mypy for Python files.

If no files or directories are specified, the current directory is scanned.
        """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Lint all files in current directory
  python WarploomController.py lint

  # Lint with automatic fixes
  python WarploomController.py lint --fix

  # Lint specific files
  python WarploomController.py lint --files src/main.cpp warploom_scripts/controller/base.py

  # Lint specific directories
  python WarploomController.py lint --directories src include

  # Lint only C++ files
  python WarploomController.py lint --cpp-only

  # Lint only Python files
  python WarploomController.py lint --python-only

  # Lint with fixes for specific files
  python WarploomController.py lint --files src/main.cpp --fix
        """,
    )

    parser.add_argument(
        "--files",
        nargs="+",
        type=Path,
        help="Specific files to lint",
    )

    parser.add_argument(
        "--directories",
        nargs="+",
        type=Path,
        help="Directories to scan for files to lint",
    )

    parser.add_argument(
        "--fix",
        action="store_true",
        help="Apply automatic fixes where possible",
    )

    parser.add_argument(
        "--cpp-only",
        action="store_true",
        help="Only lint C++ files",
    )

    parser.add_argument(
        "--python-only",
        action="store_true",
        help="Only lint Python files",
    )


def parse_args(args: Optional[list[str]] = None) -> argparse.Namespace:
    """Parse command-line arguments.

    Args:
        args: Optional list of arguments to parse. If None, uses sys.argv[1:].

    Returns:
        Parsed arguments as a Namespace object.
    """
    parser = create_parser()
    parsed_args = parser.parse_args(args)
    return parsed_args


__all__ = [
    "create_parser",
    "parse_args",
]
