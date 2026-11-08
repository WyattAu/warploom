#!/usr/bin/env python3
"""
Terminal Environment Setup Verification Script

This script tests the terminal environment setup functions for cross-platform compilation.
It verifies that MSVC, MinGW, and Linux environment setup work correctly.
"""

import sys
from pathlib import Path
from typing import List, Tuple

# Add workspace root to path to import warploom_scripts
workspace_root = Path(__file__).parent.parent.parent
sys.path.insert(0, str(workspace_root))

from warploom_scripts.platform.windows import setup_msvc_environment, setup_mingw_environment
from warploom_scripts.platform.linux import setup_linux_environment
from warploom_scripts.utils.terminal_utils import execute_with_terminal_setup, detect_terminal_type
from warploom_scripts.logging.logger import get_logger

logger = get_logger(__name__)


def test_terminal_type_detection() -> bool:
    """Test terminal type detection."""
    logger.info("Testing terminal type detection...")

    try:
        terminal_type = detect_terminal_type()
        logger.info(f"Detected terminal type: {terminal_type}")

        if terminal_type in ['vsdevcmd', 'msys2', 'default', 'unknown']:
            logger.info("✓ Terminal type detection successful")
            return True
        else:
            logger.error(f"✗ Unknown terminal type: {terminal_type}")
            return False

    except Exception as e:
        logger.error(f"✗ Terminal type detection failed: {e}")
        return False


def test_msvc_setup() -> bool:
    """Test MSVC environment setup (Windows only)."""
    import platform

    if platform.system().lower() != 'windows':
        logger.info("Skipping MSVC setup test (not on Windows)")
        return True

    logger.info("Testing MSVC environment setup...")

    try:
        env = setup_msvc_environment(arch='x64')

        # Check for required environment variables
        required_vars = ['INCLUDE', 'LIB', 'LIBPATH', 'PATH']
        missing_vars = [var for var in required_vars if var not in env]

        if missing_vars:
            logger.error(f"✗ Missing environment variables: {missing_vars}")
            return False

        logger.info(f"✓ MSVC environment setup successful")
        logger.debug(f"Environment variables set: {list(env.keys())}")
        return True

    except Exception as e:
        logger.error(f"✗ MSVC environment setup failed: {e}")
        return False


def test_mingw_setup() -> bool:
    """Test MinGW environment setup (Windows only)."""
    import platform

    if platform.system().lower() != 'windows':
        logger.info("Skipping MinGW setup test (not on Windows)")
        return True

    logger.info("Testing MinGW environment setup...")

    try:
        env = setup_mingw_environment(environment='UCRT64')

        # Check for required environment variables
        required_vars = ['MSYSTEM', 'MSYSTEM_PREFIX', 'PATH']
        missing_vars = [var for var in required_vars if var not in env]

        if missing_vars:
            logger.error(f"✗ Missing environment variables: {missing_vars}")
            return False

        logger.info(f"✓ MinGW environment setup successful")
        logger.debug(f"Environment variables set: {list(env.keys())}")
        return True

    except Exception as e:
        logger.error(f"✗ MinGW environment setup failed: {e}")
        return False


def test_linux_setup() -> bool:
    """Test Linux environment setup (Linux only)."""
    import platform

    if platform.system().lower() != 'linux':
        logger.info("Skipping Linux setup test (not on Linux)")
        return True

    logger.info("Testing Linux environment setup...")

    try:
        # Test GCC setup
        env = setup_linux_environment(compiler='gcc')

        # Check for required environment variables
        required_vars = ['CC', 'CXX']
        missing_vars = [var for var in required_vars if var not in env]

        if missing_vars:
            logger.error(f"✗ Missing environment variables: {missing_vars}")
            return False

        logger.info(f"✓ Linux GCC environment setup successful")
        logger.debug(f"CC: {env['CC']}, CXX: {env['CXX']}")

        # Test Clang setup
        env = setup_linux_environment(compiler='clang')

        if 'CC' not in env or 'CXX' not in env:
            logger.error(f"✗ Missing environment variables for Clang")
            return False

        logger.info(f"✓ Linux Clang environment setup successful")
        logger.debug(f"CC: {env['CC']}, CXX: {env['CXX']}")

        return True

    except Exception as e:
        logger.error(f"✗ Linux environment setup failed: {e}")
        return False


def test_terminal_invocation() -> bool:
    """Test terminal invocation with simple commands."""
    logger.info("Testing terminal invocation...")

    try:
        import platform

        if platform.system().lower() == 'windows':
            # Test with default terminal
            exit_code = execute_with_terminal_setup('echo "Hello from terminal"', compiler=None)

            if exit_code == 0:
                logger.info("✓ Terminal invocation successful")
                return True
            else:
                logger.error(f"✗ Terminal invocation failed with exit code: {exit_code}")
                return False
        else:
            # Test with Linux terminal
            exit_code = execute_with_terminal_setup('echo "Hello from terminal"', compiler='gcc')

            if exit_code == 0:
                logger.info("✓ Terminal invocation successful")
                return True
            else:
                logger.error(f"✗ Terminal invocation failed with exit code: {exit_code}")
                return False

    except Exception as e:
        logger.error(f"✗ Terminal invocation failed: {e}")
        return False


def main() -> int:
    """Run all terminal setup tests."""
    logger.info("=" * 60)
    logger.info("Terminal Environment Setup Verification")
    logger.info("=" * 60)

    results: List[Tuple[str, bool]] = []

    # Test terminal type detection
    results.append(("Terminal Type Detection", test_terminal_type_detection()))

    # Test MSVC setup (Windows only)
    results.append(("MSVC Setup", test_msvc_setup()))

    # Test MinGW setup (Windows only)
    results.append(("MinGW Setup", test_mingw_setup()))

    # Test Linux setup (Linux only)
    results.append(("Linux Setup", test_linux_setup()))

    # Test terminal invocation
    results.append(("Terminal Invocation", test_terminal_invocation()))

    # Print summary
    logger.info("=" * 60)
    logger.info("Test Summary")
    logger.info("=" * 60)

    for test_name, result in results:
        status = "✓ PASS" if result else "✗ FAIL"
        logger.info(f"{test_name}: {status}")

    # Calculate overall result
    passed = sum(1 for _, result in results if result)
    total = len(results)

    logger.info("=" * 60)
    logger.info(f"Tests Passed: {passed}/{total}")
    logger.info("=" * 60)

    # Return exit code
    return 0 if passed == total else 1


if __name__ == '__main__':
    sys.exit(main())
