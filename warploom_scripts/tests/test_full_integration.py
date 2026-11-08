#!/usr/bin/env python3
"""
Comprehensive Integration Tests for OmniCPP Build System

This module tests the integration of all components:
- Logging system
- Platform detection
- Compiler detection
- Terminal setup
- Build system
- Controller

These tests verify that all components work together correctly.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

# Add project root to path
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

import pytest

from warploom_scripts.logging import (
    get_logger,
    setup_logging,
    log_info,
    log_warning,
    log_error,
    log_success,
    is_logging_initialized,
)
from warploom_scripts.platform import (
    detect_platform,
    detect_architecture,
    get_platform_info,
    PlatformInfo,
)
from warploom_scripts.compilers import (
    detect_compiler,
    validate_cpp23_support,
    detect_all_compilers,
    CompilerInfo,
    ValidationResult,
)
from warploom_scripts.utils.terminal_utils import (
    setup_terminal_environment,
    execute_with_terminal_setup,
    detect_terminal_type,
    TerminalEnvironment,
)


class TestLoggingIntegration:
    """Test logging system integration."""

    def test_logging_initialization(self) -> None:
        """Test that logging system initializes correctly."""
        # Initialize logging
        setup_logging()

        # Verify logging is initialized
        assert is_logging_initialized(), "Logging should be initialized"

        # Get logger
        logger = get_logger(__name__)
        assert logger is not None, "Logger should not be None"

        # Test logging functions
        log_info("Test info message")
        log_warning("Test warning message")
        log_error("Test error message")
        log_success("Test success message")

    def test_multiple_loggers(self) -> None:
        """Test that multiple loggers can be created."""
        setup_logging()

        logger1 = get_logger("module1")
        logger2 = get_logger("module2")
        logger3 = get_logger("module3")

        assert logger1 is not None
        assert logger2 is not None
        assert logger3 is not None

        # All loggers should work
        logger1.info("Logger 1 message")
        logger2.info("Logger 2 message")
        logger3.info("Logger 3 message")


class TestPlatformDetectionIntegration:
    """Test platform detection integration."""

    def test_platform_detection(self) -> None:
        """Test that platform detection works."""
        setup_logging()
        logger = get_logger(__name__)

        # Detect platform
        platform_info = detect_platform()

        # Verify platform info
        assert platform_info is not None, "Platform info should not be None"
        assert platform_info.os in ["Windows", "Linux", "macOS"], \
            f"Unknown OS: {platform_info.os}"
        assert platform_info.architecture in ["x86_64", "ARM64", "x86"], \
            f"Unknown architecture: {platform_info.architecture}"
        assert isinstance(platform_info.is_64bit, bool), \
            "is_64bit should be boolean"

        logger.info(f"Detected platform: {platform_info.os} {platform_info.architecture}")

    def test_architecture_detection(self) -> None:
        """Test that architecture detection works."""
        setup_logging()
        logger = get_logger(__name__)

        # Detect architecture
        arch = detect_architecture()

        # Verify architecture
        assert arch is not None, "Architecture should not be None"
        assert arch in ["x86_64", "ARM64", "x86"], \
            f"Unknown architecture: {arch}"

        logger.info(f"Detected architecture: {arch}")

    def test_get_platform_info(self) -> None:
        """Test that get_platform_info works."""
        setup_logging()
        logger = get_logger(__name__)

        # Get platform info
        platform_info = get_platform_info()

        # Verify platform info
        assert platform_info is not None, "Platform info should not be None"
        assert platform_info.os in ["Windows", "Linux", "macOS"], \
            f"Unknown OS: {platform_info.os}"

        logger.info(f"Platform info: {platform_info.os} {platform_info.architecture}")


class TestCompilerDetectionIntegration:
    """Test compiler detection integration."""

    def test_compiler_detection(self) -> None:
        """Test that compiler detection works."""
        setup_logging()
        logger = get_logger(__name__)

        # Detect platform first
        platform_info = detect_platform()

        # Detect compiler
        compiler_info = detect_compiler(platform_info=platform_info)

        # Verify compiler info
        if compiler_info:
            assert compiler_info.name is not None, "Compiler name should not be None"
            assert compiler_info.version is not None, "Compiler version should not be None"
            assert compiler_info.path is not None, "Compiler path should not be None"
            assert isinstance(compiler_info.supports_cpp23, bool), \
                "supports_cpp23 should be boolean"

            logger.info(
                f"Detected compiler: {compiler_info.name} "
                f"{compiler_info.version}"
            )
        else:
            logger.warning("No compiler detected")

    def test_cpp23_validation(self) -> None:
        """Test that C++23 validation works."""
        setup_logging()
        logger = get_logger(__name__)

        # Detect platform and compiler
        platform_info = detect_platform()
        compiler_info = detect_compiler(platform_info=platform_info)

        if compiler_info:
            # Validate C++23 support
            validation = validate_cpp23_support(compiler_info)

            # Verify validation result
            assert validation is not None, "Validation result should not be None"
            assert isinstance(validation.valid, bool), \
                "valid should be boolean"
            assert isinstance(validation.version, str), \
                "version should be string"
            assert isinstance(validation.warnings, list), \
                "warnings should be list"
            assert isinstance(validation.errors, list), \
                "errors should be list"

            if validation.valid:
                logger.info(
                    f"Compiler supports C++23: "
                    f"{compiler_info.supports_cpp23}"
                )
            else:
                logger.warning(
                    f"Compiler does not support C++23, "
                    f"fallback: {validation.fallback}"
                )
                for warning in validation.warnings:
                    logger.warning(warning)

    def test_detect_all_compilers(self) -> None:
        """Test that detect_all_compilers works."""
        setup_logging()
        logger = get_logger(__name__)

        # Detect platform
        platform_info = detect_platform()

        # Detect all compilers
        compilers = detect_all_compilers(platform_info=platform_info)

        # Verify compilers dictionary
        assert compilers is not None, "Compilers dictionary should not be None"
        assert isinstance(compilers, dict), "Compilers should be dictionary"

        # Log available compilers
        available = [name for name, info in compilers.items() if info is not None]
        if available:
            logger.info(f"Available compilers: {', '.join(available)}")
        else:
            logger.warning("No compilers found")


class TestTerminalSetupIntegration:
    """Test terminal setup integration."""

    def test_terminal_environment_setup(self) -> None:
        """Test that terminal environment setup works."""
        setup_logging()
        logger = get_logger(__name__)

        # Test with different compilers
        compilers = ["msvc", "mingw-gcc", "mingw-clang", "gcc", "clang"]

        for compiler in compilers:
            # Setup terminal environment
            terminal_env = setup_terminal_environment(compiler)

            # Verify terminal environment
            assert terminal_env is not None, "Terminal environment should not be None"
            assert terminal_env.compiler == compiler, \
                f"Compiler mismatch: {terminal_env.compiler} != {compiler}"

            logger.info(f"Terminal environment for {compiler}: {terminal_env.terminal_type}")

    def test_terminal_type_detection(self) -> None:
        """Test that terminal type detection works."""
        setup_logging()
        logger = get_logger(__name__)

        # Detect terminal type
        terminal_type = detect_terminal_type()

        # Verify terminal type
        assert terminal_type in ["vsdevcmd", "msys2", "default", "unknown"], \
            f"Unknown terminal type: {terminal_type}"

        logger.info(f"Detected terminal type: {terminal_type}")

    def test_execute_with_terminal_setup(self) -> None:
        """Test that execute_with_terminal_setup works."""
        setup_logging()
        logger = get_logger(__name__)

        # Test simple command execution
        # Use a command that should work on all platforms
        result = execute_with_terminal_setup(
            'python -c "print(\'Hello from terminal setup\')"',
            compiler=None,
            cwd=str(project_root),
        )

        # Verify result
        assert isinstance(result, int), "Result should be integer"
        logger.info(f"Command execution result: {result}")


class TestFullIntegration:
    """Test full integration of all components."""

    def test_logging_platform_compiler_integration(self) -> None:
        """Test that logging, platform detection, and compiler detection work together."""
        # Initialize logging
        setup_logging()
        logger = get_logger(__name__)

        # Detect platform
        platform_info = detect_platform()
        logger.info(f"Platform: {platform_info.os} {platform_info.architecture}")

        # Detect compiler
        compiler_info = detect_compiler(platform_info=platform_info)

        if compiler_info:
            # Validate C++23 support
            validation = validate_cpp23_support(compiler_info)

            # Log all information
            logger.info(f"Compiler: {compiler_info.name} {compiler_info.version}")
            logger.info(f"C++23 Support: {validation.valid}")

            # Verify all components are working
            assert platform_info is not None
            assert compiler_info is not None
            assert validation is not None

    def test_terminal_setup_with_compiler_detection(self) -> None:
        """Test that terminal setup works with compiler detection."""
        # Initialize logging
        setup_logging()
        logger = get_logger(__name__)

        # Detect platform and compiler
        platform_info = detect_platform()
        compiler_info = detect_compiler(platform_info=platform_info)

        if compiler_info:
            # Setup terminal environment
            terminal_env = setup_terminal_environment(compiler_info.name.lower())

            # Verify integration
            assert terminal_env is not None
            assert terminal_env.compiler == compiler_info.name.lower()

            logger.info(
                f"Terminal setup for {compiler_info.name}: "
                f"{terminal_env.terminal_type}"
            )

    def test_cross_platform_scenarios(self) -> None:
        """Test cross-platform scenarios."""
        # Initialize logging
        setup_logging()
        logger = get_logger(__name__)

        # Detect platform
        platform_info = detect_platform()
        logger.info(f"Testing on {platform_info.os}")

        # Detect all compilers
        compilers = detect_all_compilers(platform_info=platform_info)

        # Log available compilers
        available = [name for name, info in compilers.items() if info is not None]
        if available:
            logger.info(f"Available compilers: {', '.join(available)}")

            # Test terminal setup for each available compiler
            for compiler_name in available:
                terminal_env = setup_terminal_environment(compiler_name)
                assert terminal_env is not None
                logger.info(f"Terminal setup for {compiler_name}: OK")
        else:
            logger.warning("No compilers available for testing")

    def test_error_handling_integration(self) -> None:
        """Test error handling across all components."""
        # Initialize logging
        setup_logging()
        logger = get_logger(__name__)

        # Test error handling
        try:
            # This should raise an error
            raise ValueError("Test error")
        except ValueError as e:
            logger.error(f"Caught expected error: {e}")
            assert str(e) == "Test error"

        # Test logging errors
        log_error("Test error message")
        log_warning("Test warning message")


class TestBuildSystemIntegration:
    """Test build system integration."""

    def test_build_context_creation(self) -> None:
        """Test that build context can be created."""
        setup_logging()
        logger = get_logger(__name__)

        # Import build components
        from warploom_scripts.build import BuildContext

        # Create build context
        context = BuildContext(
            product="engine",
            task="Build Project",
            arch="x64",
            build_type="Release",
            compiler="msvc",
            is_cross_compilation=False,
            lib_flag=True,
            st_flag=False,
        )

        # Verify context
        assert context is not None, "Build context should not be None"
        assert context.product == "engine"
        assert context.build_type == "Release"
        assert context.compiler == "msvc"

        logger.info(f"Build context created: {context.product} {context.build_type}")

    def test_build_manager_initialization(self) -> None:
        """Test that build manager can be initialized."""
        setup_logging()
        logger = get_logger(__name__)

        # Import build manager
        from warploom_scripts.build import BuildManager

        # Initialize build manager
        build_manager = BuildManager(project_root)

        # Verify build manager
        assert build_manager is not None, "Build manager should not be None"

        logger.info("Build manager initialized successfully")


def run_integration_tests() -> int:
    """Run all integration tests.

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    logger = get_logger(__name__)
    logger.info("Running comprehensive integration tests...")

    # Run pytest
    exit_code = pytest.main([str(Path(__file__).parent), "-v"])

    if exit_code == 0:
        logger.info("All integration tests passed")
    else:
        logger.error(f"Integration tests failed with exit code: {exit_code}")

    return exit_code


if __name__ == "__main__":
    sys.exit(run_integration_tests())
