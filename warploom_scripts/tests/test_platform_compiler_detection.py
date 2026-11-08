#!/usr/bin/env python3
"""
Test script for platform and compiler detection.

This script tests the platform and compiler detection modules
to ensure they work correctly on Windows.
"""

import sys
from pathlib import Path
from typing import List, Tuple

# Add warploom_scripts to path
sys.path.insert(0, str(Path(__file__).parent.parent / "warploom_scripts"))

from warploom_scripts.logging.logger import setup_logging, get_logger
from warploom_scripts.platform.detector import detect_platform, detect_architecture
from warploom_scripts.platform.windows import detect_msvc, detect_mingw, detect_mingw_clang
from warploom_scripts.compilers.detector import detect_compiler, detect_all_compilers, validate_cpp23_support
from warploom_scripts.compilers.msvc import detect_msvc as detect_msvc_detailed
from warploom_scripts.compilers.gcc import detect_gcc as detect_gcc_detailed
from warploom_scripts.compilers.clang import detect_clang as detect_clang_detailed


def test_platform_detection() -> bool:
    """Test platform detection."""
    logger = get_logger(__name__)

    logger.info("=" * 60)
    logger.info("Testing Platform Detection")
    logger.info("=" * 60)

    try:
        # Test platform detection
        platform_info = detect_platform()
        logger.info(f"Platform: {platform_info.os}")
        logger.info(f"Architecture: {platform_info.architecture}")
        logger.info(f"64-bit: {platform_info.is_64bit}")
        logger.info(f"Platform String: {platform_info.platform_string}")

        # Test architecture detection
        arch = detect_architecture()
        logger.info(f"Architecture (separate): {arch}")

        logger.info("✓ Platform detection successful")
        return True

    except Exception as e:
        logger.error(f"✗ Platform detection failed: {e}")
        return False


def test_windows_compiler_detection() -> bool:
    """Test Windows compiler detection."""
    logger = get_logger(__name__)

    logger.info("=" * 60)
    logger.info("Testing Windows Compiler Detection")
    logger.info("=" * 60)

    success = True

    # Test MSVC detection
    try:
        msvc_info = detect_msvc()
        if msvc_info:
            logger.info(f"✓ MSVC found: {msvc_info.version}")
            logger.info(f"  Edition: {msvc_info.edition}")
            logger.info(f"  Year: {msvc_info.year}")
            logger.info(f"  Path: {msvc_info.path}")
            logger.info(f"  vcvars: {msvc_info.vcvars_path}")
        else:
            logger.warning("✗ MSVC not found")
            success = False
    except Exception as e:
        logger.error(f"✗ MSVC detection failed: {e}")
        success = False

    # Test MinGW-GCC detection
    try:
        mingw_gcc_info = detect_mingw()
        if mingw_gcc_info:
            logger.info(f"✓ MinGW-GCC found: {mingw_gcc_info.version}")
            logger.info(f"  Environment: {mingw_gcc_info.environment}")
            logger.info(f"  Path: {mingw_gcc_info.path}")
            logger.info(f"  gcc: {mingw_gcc_info.gcc_path}")
            logger.info(f"  g++: {mingw_gcc_info.gxx_path}")
        else:
            logger.warning("✗ MinGW-GCC not found")
    except Exception as e:
        logger.error(f"✗ MinGW-GCC detection failed: {e}")

    # Test MinGW-Clang detection
    try:
        mingw_clang_info = detect_mingw_clang()
        if mingw_clang_info:
            logger.info(f"✓ MinGW-Clang found: {mingw_clang_info.version}")
            logger.info(f"  Environment: {mingw_clang_info.environment}")
            logger.info(f"  Path: {mingw_clang_info.path}")
            logger.info(f"  clang: {mingw_clang_info.gcc_path}")
            logger.info(f"  clang++: {mingw_clang_info.gxx_path}")
        else:
            logger.warning("✗ MinGW-Clang not found")
    except Exception as e:
        logger.error(f"✗ MinGW-Clang detection failed: {e}")

    return success


def test_compiler_detector() -> bool:
    """Test compiler detector module."""
    logger = get_logger(__name__)

    logger.info("=" * 60)
    logger.info("Testing Compiler Detector Module")
    logger.info("=" * 60)

    success = True

    # Test auto-detection
    try:
        compiler_info = detect_compiler()
        if compiler_info:
            logger.info(f"✓ Auto-detected compiler: {compiler_info.name}")
            logger.info(f"  Version: {compiler_info.version}")
            logger.info(f"  Path: {compiler_info.path}")
            logger.info(f"  C++23: {compiler_info.supports_cpp23}")
            logger.info(f"  Platform: {compiler_info.platform}")

            # Test C++23 validation
            validation = validate_cpp23_support(compiler_info)
            logger.info(f"  Validation: {'✓ Valid' if validation.valid else '✗ Invalid'}")
            if validation.warnings:
                for warning in validation.warnings:
                    logger.warning(f"    Warning: {warning}")
            if validation.fallback:
                logger.info(f"    Fallback: {validation.fallback}")
        else:
            logger.warning("✗ No compiler auto-detected")
            success = False
    except Exception as e:
        logger.error(f"✗ Compiler detection failed: {e}")
        success = False

    # Test detection of all compilers
    try:
        all_compilers = detect_all_compilers()
        logger.info(f"✓ Detected {len(all_compilers)} compiler types")
        for name, info in all_compilers.items():
            if info:
                logger.info(f"  {name}: {info.version} (C++23: {info.supports_cpp23})")
            else:
                logger.info(f"  {name}: Not found")
    except Exception as e:
        logger.error(f"✗ All compilers detection failed: {e}")
        success = False

    return success


def test_detailed_compiler_detection() -> bool:
    """Test detailed compiler detection modules."""
    logger = get_logger(__name__)

    logger.info("=" * 60)
    logger.info("Testing Detailed Compiler Detection")
    logger.info("=" * 60)

    # Test MSVC detailed detection
    try:
        msvc_info = detect_msvc_detailed()
        if msvc_info:
            logger.info(f"✓ MSVC detailed: {msvc_info.version}")
            logger.info(f"  Edition: {msvc_info.edition}")
            logger.info(f"  Year: {msvc_info.year}")
            logger.info(f"  Install Path: {msvc_info.install_path}")
            logger.info(f"  vcvars Path: {msvc_info.vcvars_path}")
            logger.info(f"  C++23: {msvc_info.supports_cpp23}")
            logger.info(f"  Toolset: {msvc_info.toolset_version}")
        else:
            logger.warning("✗ MSVC detailed not found")
    except Exception as e:
        logger.error(f"✗ MSVC detailed detection failed: {e}")

    # Test GCC detailed detection (if on Linux)
    try:
        gcc_info = detect_gcc_detailed()
        if gcc_info:
            logger.info(f"✓ GCC detailed: {gcc_info.version}")
            logger.info(f"  Path: {gcc_info.path}")
            logger.info(f"  g++ Path: {gcc_info.gxx_path}")
            logger.info(f"  C++23: {gcc_info.supports_cpp23}")
            logger.info(f"  Target: {gcc_info.target}")
            logger.info(f"  Prefix: {gcc_info.install_prefix}")
        else:
            logger.warning("✗ GCC detailed not found")
    except Exception as e:
        logger.error(f"✗ GCC detailed detection failed: {e}")

    # Test Clang detailed detection
    try:
        clang_info = detect_clang_detailed()
        if clang_info:
            logger.info(f"✓ Clang detailed: {clang_info.version}")
            logger.info(f"  Path: {clang_info.path}")
            logger.info(f"  clang++ Path: {clang_info.clangxx_path}")
            logger.info(f"  C++23: {clang_info.supports_cpp23}")
            logger.info(f"  Target: {clang_info.target}")
            logger.info(f"  Prefix: {clang_info.install_prefix}")
        else:
            logger.warning("✗ Clang detailed not found")
    except Exception as e:
        logger.error(f"✗ Clang detailed detection failed: {e}")

    return True


def main() -> int:
    """Main test function."""
    # Setup logging
    setup_logging()
    logger = get_logger(__name__)

    logger.info("=" * 60)
    logger.info("Platform and Compiler Detection Test Suite")
    logger.info("=" * 60)

    results: List[Tuple[str, bool]] = []

    # Run tests
    results.append(("Platform Detection", test_platform_detection()))
    results.append(("Windows Compiler Detection", test_windows_compiler_detection()))
    results.append(("Compiler Detector", test_compiler_detector()))
    results.append(("Detailed Compiler Detection", test_detailed_compiler_detection()))

    # Print summary
    logger.info("=" * 60)
    logger.info("Test Summary")
    logger.info("=" * 60)

    passed = sum(1 for _, result in results if result)
    total = len(results)

    for name, result in results:
        status = "✓ PASS" if result else "✗ FAIL"
        logger.info(f"{status}: {name}")

    logger.info("=" * 60)
    logger.info(f"Results: {passed}/{total} tests passed")
    logger.info("=" * 60)

    # Return exit code
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
