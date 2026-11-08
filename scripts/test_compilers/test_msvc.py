#!/usr/bin/env python3
"""
MSVC Compiler Test Script

Tests MSVC compiler installation and C++23 support.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from warploom_scripts.compilers.detector import detect_compiler, validate_cpp23_support
from warploom_scripts.platform.detector import detect_platform
from warploom_scripts.logging.logger import get_logger, setup_logging

logger = get_logger(__name__)


def test_msvc_installation() -> bool:
    """Test MSVC compiler installation.

    Returns:
        True if MSVC is installed and working, False otherwise.
    """
    logger.info("Testing MSVC installation...")

    try:
        # Test cl.exe
        result = subprocess.run(
            ["cl", "/?"],
            capture_output=True,
            text=True,
            timeout=10,
        )

        if result.returncode == 0:
            logger.info("MSVC compiler (cl.exe) found")
            return True
        else:
            logger.error("MSVC compiler (cl.exe) not found")
            return False
    except FileNotFoundError:
        logger.error("MSVC compiler (cl.exe) not found in PATH")
        return False
    except Exception as e:
        logger.error(f"Error testing MSVC: {e}")
        return False


def test_msvc_cpp23_support() -> bool:
    """Test MSVC C++23 support.

    Returns:
        True if MSVC supports C++23, False otherwise.
    """
    logger.info("Testing MSVC C++23 support...")

    try:
        # Create a simple C++23 test file
        test_file = Path("test_msvc_cpp23.cpp")
        test_file.write_text("""
#include <iostream>
#include <vector>
#include <string>

int main() {
    // C++23 features
    auto lambda = []<typename T>(T x) { return x * 2; };
    std::vector<int> v = {1, 2, 3};
    std::string s = "Hello, C++23!";

    std::cout << s << std::endl;
    return 0;
}
""")

        # Compile with C++23
        result = subprocess.run(
            ["cl", "/std:c++23", "/EHsc", str(test_file)],
            capture_output=True,
            text=True,
            timeout=30,
        )

        # Clean up
        if test_file.exists():
            test_file.unlink()
        for ext in [".obj", ".exe"]:
            obj_file = Path(f"test_msvc_cpp23{ext}")
            if obj_file.exists():
                obj_file.unlink()

        if result.returncode == 0:
            logger.info("MSVC C++23 support confirmed")
            return True
        else:
            logger.error(f"MSVC C++23 compilation failed: {result.stderr}")
            return False
    except Exception as e:
        logger.error(f"Error testing MSVC C++23 support: {e}")
        return False


def test_msvc_standard_library() -> bool:
    """Test MSVC standard library.

    Returns:
        True if MSVC standard library is working, False otherwise.
    """
    logger.info("Testing MSVC standard library...")

    try:
        # Create a simple standard library test
        test_file = Path("test_msvc_stdlib.cpp")
        test_file.write_text("""
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>

int main() {
    std::vector<int> v = {1, 2, 3, 4, 5};
    std::unique_ptr<int> ptr = std::make_unique<int>(42);
    std::sort(v.begin(), v.end());

    std::cout << "Standard library test passed" << std::endl;
    return 0;
}
""")

        # Compile
        result = subprocess.run(
            ["cl", "/std:c++23", "/EHsc", str(test_file)],
            capture_output=True,
            text=True,
            timeout=30,
        )

        # Clean up
        if test_file.exists():
            test_file.unlink()
        for ext in [".obj", ".exe"]:
            obj_file = Path(f"test_msvc_stdlib{ext}")
            if obj_file.exists():
                obj_file.unlink()

        if result.returncode == 0:
            logger.info("MSVC standard library test passed")
            return True
        else:
            logger.error(f"MSVC standard library test failed: {result.stderr}")
            return False
    except Exception as e:
        logger.error(f"Error testing MSVC standard library: {e}")
        return False


def main() -> int:
    """Main entry point.

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    setup_logging()

    logger.info("=" * 60)
    logger.info("MSVC Compiler Test Suite")
    logger.info("=" * 60)

    # Detect platform
    platform_info = detect_platform()
    logger.info(f"Platform: {platform_info.os} {platform_info.architecture}")

    # Detect compiler
    compiler_info = detect_compiler(compiler_name="msvc", platform_info=platform_info)

    if not compiler_info:
        logger.error("MSVC compiler not detected")
        return 1

    logger.info(f"Compiler: {compiler_info.name} {compiler_info.version}")
    logger.info(f"Path: {compiler_info.path}")
    logger.info(f"C++23 Support: {compiler_info.supports_cpp23}")

    # Validate C++23 support
    validation = validate_cpp23_support(compiler_info)
    logger.info(f"Validation: {validation.valid}")
    if validation.warnings:
        for warning in validation.warnings:
            logger.warning(warning)
    if validation.errors:
        for error in validation.errors:
            logger.error(error)

    # Run tests
    tests_passed = 0
    tests_total = 3

    if test_msvc_installation():
        tests_passed += 1

    if test_msvc_cpp23_support():
        tests_passed += 1

    if test_msvc_standard_library():
        tests_passed += 1

    # Summary
    logger.info("=" * 60)
    logger.info(f"Tests Passed: {tests_passed}/{tests_total}")
    logger.info("=" * 60)

    if tests_passed == tests_total:
        logger.info("✅ All MSVC tests passed")
        return 0
    else:
        logger.error("❌ Some MSVC tests failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())
