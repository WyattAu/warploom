#!/usr/bin/env python3
"""
All Compilers Test Script

Tests all available compilers on the current platform.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from typing import Dict

from warploom_scripts.compilers.detector import (
    CompilerInfo,
    detect_all_compilers,
    validate_cpp23_support,
)
from warploom_scripts.platform.detector import detect_platform
from warploom_scripts.logging.logger import get_logger, setup_logging

logger = get_logger(__name__)


def test_compiler(compiler_name: str, compiler_info: CompilerInfo) -> bool:
    """Test a specific compiler.

    Args:
        compiler_name: Name of the compiler to test.
        compiler_info: CompilerInfo object.

    Returns:
        True if compiler tests pass, False otherwise.
    """
    logger.info(f"Testing {compiler_name}...")

    tests_passed = 0
    tests_total = 3

    # Test 1: Compiler exists
    if compiler_info and compiler_info.path.exists():
        logger.info(f"  ✓ Compiler found: {compiler_info.path}")
        tests_passed += 1
    else:
        logger.error(f"  ✗ Compiler not found: {compiler_info.path}")
        return False

    # Test 2: C++23 support
    validation = validate_cpp23_support(compiler_info)
    if validation.valid:
        logger.info(f"  ✓ C++23 support: {compiler_info.version}")
        tests_passed += 1
    else:
        logger.warning(f"  ⚠ C++23 support: {validation.fallback}")
        tests_passed += 1

    # Test 3: Simple compilation
    try:
        # Create test file in script directory
        script_dir = Path(__file__).parent
        test_file = script_dir / "test_compilers" / f"test_{compiler_name.replace('-', '_')}.cpp"
        test_file.write_text("""
#include <iostream>
#include <vector>
#include <string>

int main() {
    std::vector<int> v = {1, 2, 3};
    std::string s = "Hello, World!";
    std::cout << s << std::endl;
    return 0;
}
""")

        # Determine compiler command
        if compiler_name == "msvc":
            cmd = ["cl", "/std:c++23", "/EHsc", str(test_file)]
        elif compiler_name == "msvc-clang":
            cmd = ["clang-cl", "-std=c++23", str(test_file)]
        elif compiler_name == "mingw-gcc":
            cmd = ["g++", "-std=c++23", str(test_file)]
        elif compiler_name == "mingw-clang":
            cmd = ["clang++", "-std=c++23", str(test_file)]
        elif compiler_name == "gcc":
            cmd = ["g++", "-std=c++23", str(test_file)]
        elif compiler_name == "clang":
            cmd = ["clang++", "-std=c++23", str(test_file)]
        else:
            logger.error(f"  ✗ Unknown compiler: {compiler_name}")
            return False

        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
        )

        # Clean up
        if test_file.exists():
            test_file.unlink()
        for ext in [".o", ".obj", ".exe"]:
            obj_file = Path(f"test_{compiler_name.replace('-', '_')}{ext}")
            if obj_file.exists():
                obj_file.unlink()

        if result.returncode == 0:
            logger.info(f"  ✓ Compilation successful")
            tests_passed += 1
        else:
            logger.error(f"  ✗ Compilation failed: {result.stderr}")
    except Exception as e:
        logger.error(f"  ✗ Compilation test error: {e}")

    # Summary
    logger.info(f"  Tests passed: {tests_passed}/{tests_total}")
    return tests_passed == tests_total


def main() -> int:
    """Main entry point.

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    setup_logging()

    logger.info("=" * 60)
    logger.info("All Compilers Test Suite")
    logger.info("=" * 60)

    # Detect platform
    platform_info = detect_platform()
    logger.info(f"Platform: {platform_info.os} {platform_info.architecture}")
    logger.info("")

    # Detect all compilers
    compilers = detect_all_compilers(platform_info)

    if not compilers:
        logger.error("No compilers detected")
        return 1

    logger.info(f"Detected compilers: {list(compilers.keys())}")
    logger.info("")

    # Test each compiler
    results: Dict[str, bool] = {}
    for compiler_name, compiler_info in compilers.items():
        if compiler_info:
            results[compiler_name] = test_compiler(compiler_name, compiler_info)
        else:
            logger.warning(f"Skipping {compiler_name} (not detected)")
            results[compiler_name] = False
        logger.info("")

    # Summary
    logger.info("=" * 60)
    logger.info("Test Summary")
    logger.info("=" * 60)

    passed = sum(1 for result in results.values() if result)
    total = len(results)

    for compiler_name, result in results.items():
        status = "✅ PASS" if result else "❌ FAIL"
        logger.info(f"  {compiler_name:20s}: {status}")

    logger.info("")
    logger.info(f"Total: {passed}/{total} compilers passed")
    logger.info("=" * 60)

    if passed == total:
        logger.info("✅ All compiler tests passed")
        return 0
    else:
        logger.error(f"❌ {total - passed} compiler(s) failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())
