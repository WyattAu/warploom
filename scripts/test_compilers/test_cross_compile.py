#!/usr/bin/env python3
"""
Cross-Compilation Test Script

Tests cross-compilation to Linux and WASM.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from typing import Dict, List

from warploom_scripts.build_system.cmake import CMakeWrapper
from warploom_scripts.logging.logger import get_logger, setup_logging

logger = get_logger(__name__)


def test_linux_cross_compile(toolchain_file: Path) -> bool:
    """Test cross-compilation to Linux.

    Args:
        toolchain_file: Path to Linux toolchain file.

    Returns:
        True if cross-compilation succeeds, False otherwise.
    """
    logger.info(f"Testing Linux cross-compilation with {toolchain_file.name}...")

    try:
        # Create CMake wrapper
        cmake = CMakeWrapper(
            source_dir=Path.cwd(),
            build_dir=Path.cwd() / "build" / "test_linux",
        )

        # Configure with Linux toolchain
        result = cmake.configure(
            build_type="Release",
            toolchain=toolchain_file,
            preset=None,
        )

        if result != 0:
            logger.error("Linux cross-compilation configuration failed")
            return False

        # Build
        result = cmake.build(
            target="all",
            config="Release",
            parallel=4,
        )

        if result == 0:
            logger.info("✅ Linux cross-compilation successful")
            return True
        else:
            logger.error("❌ Linux cross-compilation build failed")
            return False

    except Exception as e:
        logger.error(f"Linux cross-compilation error: {e}")
        return False


def test_wasm_cross_compile(toolchain_file: Path) -> bool:
    """Test cross-compilation to WASM.

    Args:
        toolchain_file: Path to WASM toolchain file.

    Returns:
        True if cross-compilation succeeds, False otherwise.
    """
    logger.info(f"Testing WASM cross-compilation with {toolchain_file.name}...")

    try:
        # Create CMake wrapper
        cmake = CMakeWrapper(
            source_dir=Path.cwd(),
            build_dir=Path.cwd() / "build" / "test_wasm",
        )

        # Configure with WASM toolchain
        result = cmake.configure(
            build_type="Release",
            toolchain=toolchain_file,
            preset=None,
        )

        if result != 0:
            logger.error("WASM cross-compilation configuration failed")
            return False

        # Build
        result = cmake.build(
            target="all",
            config="Release",
            parallel=4,
        )

        if result == 0:
            logger.info("✅ WASM cross-compilation successful")
            return True
        else:
            logger.error("❌ WASM cross-compilation build failed")
            return False

    except Exception as e:
        logger.error(f"WASM cross-compilation error: {e}")
        return False


def test_toolchain_file(toolchain_file: Path) -> bool:
    """Test if toolchain file is valid.

    Args:
        toolchain_file: Path to toolchain file.

    Returns:
        True if toolchain file is valid, False otherwise.
    """
    logger.info(f"Validating toolchain file: {toolchain_file}")

    if not toolchain_file.exists():
        logger.error(f"Toolchain file not found: {toolchain_file}")
        return False

    try:
        content = toolchain_file.read_text(encoding="utf-8")

        # Check for required CMake variables
        required_vars = [
            "CMAKE_SYSTEM_NAME",
            "CMAKE_SYSTEM_PROCESSOR",
            "CMAKE_C_COMPILER",
            "CMAKE_CXX_COMPILER",
        ]

        missing_vars = [var for var in required_vars if var not in content]

        if missing_vars:
            logger.error(f"Missing required variables: {missing_vars}")
            return False

        logger.info("✅ Toolchain file is valid")
        return True

    except Exception as e:
        logger.error(f"Error reading toolchain file: {e}")
        return False


def main() -> int:
    """Main entry point.

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    setup_logging()

    logger.info("=" * 60)
    logger.info("Cross-Compilation Test Suite")
    logger.info("=" * 60)

    # Get toolchain directory
    toolchain_dir = Path.cwd() / "cmake" / "toolchains"

    if not toolchain_dir.exists():
        logger.error(f"Toolchain directory not found: {toolchain_dir}")
        return 1

    # Test Linux cross-compilation
    logger.info("")
    logger.info("Testing Linux Cross-Compilation")
    logger.info("-" * 60)

    linux_toolchains = [
        "x86-linux-gnu.cmake",
        "arm64-linux-gnu.cmake",
    ]

    linux_results: Dict[str, bool] = {}
    for toolchain_name in linux_toolchains:
        toolchain_file = toolchain_dir / toolchain_name
        if toolchain_file.exists():
            if test_toolchain_file(toolchain_file):
                linux_results[toolchain_name] = test_linux_cross_compile(toolchain_file)
            else:
                linux_results[toolchain_name] = False
        else:
            logger.warning(f"Toolchain not found: {toolchain_name}")
            linux_results[toolchain_name] = False

    # Test WASM cross-compilation
    logger.info("")
    logger.info("Testing WASM Cross-Compilation")
    logger.info("-" * 60)

    wasm_toolchain = toolchain_dir / "emscripten.cmake"
    wasm_result = False

    if wasm_toolchain.exists():
        if test_toolchain_file(wasm_toolchain):
            wasm_result = test_wasm_cross_compile(wasm_toolchain)
        else:
            wasm_result = False
    else:
        logger.warning("WASM toolchain not found: emscripten.cmake")
        wasm_result = False

    # Summary
    logger.info("")
    logger.info("=" * 60)
    logger.info("Cross-Compilation Test Summary")
    logger.info("=" * 60)

    logger.info("Linux Cross-Compilation:")
    for toolchain_name, result in linux_results.items():
        status = "✅ PASS" if result else "❌ FAIL"
        logger.info(f"  {toolchain_name}: {status}")

    logger.info("")
    logger.info("WASM Cross-Compilation:")
    status = "✅ PASS" if wasm_result else "❌ FAIL"
    logger.info(f"  emscripten.cmake: {status}")

    logger.info("")

    # Overall result
    linux_passed = sum(1 for result in linux_results.values() if result)
    linux_total = len(linux_results)

    if linux_passed == linux_total and wasm_result:
        logger.info("✅ All cross-compilation tests passed")
        return 0
    else:
        logger.error(f"❌ Some cross-compilation tests failed")
        logger.error(f"  Linux: {linux_passed}/{linux_total} passed")
        logger.error(f"  WASM: {'PASS' if wasm_result else 'FAIL'}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
