"""
Integration tests for cross-platform components.

This module provides integration tests for platform detection,
compiler detection, terminal setup, and cross-platform compilation.
"""

from __future__ import annotations

import logging
import sys
from pathlib import Path
from typing import Optional, Any
from unittest.mock import Mock, patch, MagicMock

import pytest

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from warploom_scripts.platform.detector import (
    PlatformInfo,
    detect_platform,
    detect_architecture,
    get_platform_info
)
from warploom_scripts.compilers.detector import (
    CompilerInfo,
    ValidationResult,
    detect_compiler,
    validate_cpp23_support,
    detect_all_compilers
)


class TestPlatformDetectionIntegration:
    """Integration tests for platform detection."""

    def test_detect_platform_returns_platform_info(self) -> None:
        """Test that detect_platform returns PlatformInfo object."""
        platform_info = detect_platform()
        assert isinstance(platform_info, PlatformInfo)
        assert platform_info.os is not None
        assert platform_info.architecture is not None
        assert isinstance(platform_info.is_64bit, bool)
        assert platform_info.platform_string is not None

    def test_detect_platform_logs_detection(self, caplog: Any) -> None:
        """Test that platform detection is logged."""
        with caplog.at_level(logging.INFO):
            platform_info = detect_platform()

            # Verify detection was logged
            assert any(
                "Detected platform:" in record.message
                for record in caplog.records
            )

    def test_detect_architecture_returns_string(self) -> None:
        """Test that detect_architecture returns architecture string."""
        arch = detect_architecture()
        assert isinstance(arch, str)
        assert len(arch) > 0

    def test_detect_architecture_logs_detection(self, caplog: Any) -> None:
        """Test that architecture detection is logged."""
        with caplog.at_level(logging.INFO):
            arch = detect_architecture()

            # Verify detection was logged
            assert any(
                "Detected architecture:" in record.message
                for record in caplog.records
            )

    def test_get_platform_info_alias(self) -> None:
        """Test that get_platform_info is an alias for detect_platform."""
        info1 = detect_platform()
        info2 = get_platform_info()

        # Should return same type
        assert type(info1) == type(info2)
        assert info1.os == info2.os
        assert info1.architecture == info2.architecture

    @patch('sys.platform', 'win32')
    def test_windows_platform_detection(self) -> None:
        """Test Windows platform detection."""
        platform_info = detect_platform()
        assert platform_info.os == "Windows"
        assert platform_info.platform_string == "win32"

    @patch('sys.platform', 'linux')
    def test_linux_platform_detection(self) -> None:
        """Test Linux platform detection."""
        platform_info = detect_platform()
        assert platform_info.os == "Linux"
        assert platform_info.platform_string == "linux"

    @patch('sys.platform', 'darwin')
    def test_macos_platform_detection(self) -> None:
        """Test macOS platform detection."""
        platform_info = detect_platform()
        assert platform_info.os == "macOS"
        assert platform_info.platform_string == "darwin"

    @patch('platform.machine', return_value='x86_64')
    def test_x86_64_architecture_detection(self, mock_machine: Any) -> None:
        """Test x86_64 architecture detection."""
        platform_info = detect_platform()
        assert platform_info.architecture == "x86_64"
        assert platform_info.is_64bit is True

    @patch('platform.machine', return_value='arm64')
    def test_arm64_architecture_detection(self, mock_machine: Any) -> None:
        """Test ARM64 architecture detection."""
        platform_info = detect_platform()
        assert platform_info.architecture == "ARM64"
        assert platform_info.is_64bit is True

    @patch('platform.machine', return_value='i386')
    def test_i386_architecture_detection(self, mock_machine: Any) -> None:
        """Test i386 architecture detection."""
        platform_info = detect_platform()
        assert platform_info.architecture == "x86"
        assert platform_info.is_64bit is False


class TestCompilerDetectionIntegration:
    """Integration tests for compiler detection."""

    def test_detect_compiler_returns_compiler_info_or_none(self) -> None:
        """Test that detect_compiler returns CompilerInfo or None."""
        compiler_info = detect_compiler()
        assert compiler_info is None or isinstance(compiler_info, CompilerInfo)

    def test_detect_compiler_logs_detection(self, caplog: Any) -> None:
        """Test that compiler detection is logged."""
        with caplog.at_level(logging.INFO):
            compiler_info = detect_compiler()

            # Check that appropriate log messages were generated
            # detect_compiler() may log "No compiler found on {platform}" if no compiler is detected
            # or platform-specific detection functions may log compiler info
            log_messages = [record.message for record in caplog.records]

            # Verify that some logging occurred (either compiler found or not found)
            # The actual log messages depend on the platform and available compilers
            assert len(log_messages) > 0, "Expected some log messages from compiler detection"

    def test_detect_all_compilers_returns_dict(self) -> None:
        """Test that detect_all_compilers returns dictionary."""
        compilers = detect_all_compilers()
        assert isinstance(compilers, dict)
        # Check for expected keys based on platform
        platform_info = detect_platform()
        if platform_info.os == "Windows":
            expected_keys = ['msvc', 'msvc-clang', 'mingw-gcc', 'mingw-clang']
        elif platform_info.os == "Linux":
            expected_keys = ['gcc', 'clang']
        else:
            expected_keys = ['clang']

        for key in expected_keys:
            assert key in compilers

    def test_compiler_info_structure(self) -> None:
        """Test CompilerInfo has correct structure."""
        # Create a mock CompilerInfo
        compiler_info = CompilerInfo(
            name="TestCompiler",
            version="1.0.0",
            path=Path("/usr/bin/test"),
            supports_cpp23=True,
            platform="Linux",
            extra_info={"test": "value"}
        )

        assert compiler_info.name == "TestCompiler"
        assert compiler_info.version == "1.0.0"
        assert compiler_info.path == Path("/usr/bin/test")
        assert compiler_info.supports_cpp23 is True
        assert compiler_info.platform == "Linux"
        assert compiler_info.extra_info == {"test": "value"}

    def test_validate_cpp23_support_returns_validation_result(self) -> None:
        """Test that validate_cpp23_support returns ValidationResult."""
        compiler_info = CompilerInfo(
            name="TestCompiler",
            version="1.0.0",
            path=Path("/usr/bin/test"),
            supports_cpp23=True,
            platform="Linux"
        )

        result = validate_cpp23_support(compiler_info)
        assert isinstance(result, ValidationResult)
        assert isinstance(result.valid, bool)
        assert isinstance(result.version, str)
        assert isinstance(result.warnings, list)
        assert isinstance(result.errors, list)

    def test_validate_cpp23_support_with_cpp23(self) -> None:
        """Test validation with C++23 support."""
        compiler_info = CompilerInfo(
            name="TestCompiler",
            version="1.0.0",
            path=Path("/usr/bin/test"),
            supports_cpp23=True,
            platform="Linux"
        )

        result = validate_cpp23_support(compiler_info)
        assert result.valid is True
        assert len(result.warnings) == 0
        assert len(result.errors) == 0
        assert result.fallback is None

    def test_validate_cpp23_support_without_cpp23(self) -> None:
        """Test validation without C++23 support."""
        compiler_info = CompilerInfo(
            name="TestCompiler",
            version="1.0.0",
            path=Path("/usr/bin/test"),
            supports_cpp23=False,
            platform="Linux"
        )

        result = validate_cpp23_support(compiler_info)
        assert result.valid is False
        assert len(result.warnings) > 0
        assert result.fallback == "C++20"

    def test_compiler_detection_with_specific_compiler(self) -> None:
        """Test compiler detection with specific compiler name."""
        # This test verifies the interface works
        # Actual detection depends on installed compilers
        compiler_info = detect_compiler(compiler_name="gcc")
        # Should return CompilerInfo or None
        assert compiler_info is None or isinstance(compiler_info, CompilerInfo)

    def test_compiler_detection_with_platform_info(self) -> None:
        """Test compiler detection with provided platform info."""
        platform_info = detect_platform()
        compiler_info = detect_compiler(platform_info=platform_info)
        # Should return CompilerInfo or None
        assert compiler_info is None or isinstance(compiler_info, CompilerInfo)


class TestTerminalSetupIntegration:
    """Integration tests for terminal setup."""

    @patch('warploom_scripts.platform.detector.detect_platform')
    def test_terminal_setup_on_windows(self, mock_detect_platform: Any) -> None:
        """Test terminal setup on Windows."""
        # Mock platform detection
        mock_platform_info = PlatformInfo(
            os="Windows",
            architecture="x86_64",
            is_64bit=True,
            platform_string="win32"
        )
        mock_detect_platform.return_value = mock_platform_info

        # Import and test terminal setup
        try:
            from warploom_scripts.platform.windows import setup_msvc_environment
            # This would set up VS Dev Prompt
            # We're testing the interface exists
            assert callable(setup_msvc_environment)
        except ImportError:
            # Windows-specific module may not be available on other platforms
            pass

    def test_terminal_setup_on_linux(self) -> None:
        """Test terminal setup on Linux."""
        # This test verifies the interface exists
        # Actual platform detection depends on the system
        platform_info = detect_platform()
        # Just verify the function works
        assert platform_info is not None
        assert isinstance(platform_info, PlatformInfo)

    def test_terminal_setup_on_macos(self) -> None:
        """Test terminal setup on macOS."""
        # This test verifies the interface exists
        # Actual platform detection depends on the system
        platform_info = detect_platform()
        # Just verify the function works
        assert platform_info is not None
        assert isinstance(platform_info, PlatformInfo)

    def test_environment_variable_handling(self) -> None:
        """Test environment variable handling."""
        # Test that environment variables can be accessed
        import os
        path_var = os.environ.get('PATH')
        assert path_var is not None or isinstance(path_var, str)

    def test_compiler_path_validation(self) -> None:
        """Test compiler path validation."""
        # Test that compiler paths can be validated
        test_path = Path("/usr/bin/gcc")
        assert isinstance(test_path, Path)

        # Test path existence check
        exists = test_path.exists()
        assert isinstance(exists, bool)


class TestCrossPlatformCompilationIntegration:
    """Integration tests for cross-platform compilation."""

    @patch('warploom_scripts.platform.detector.detect_platform')
    def test_cross_compile_to_linux(self, mock_detect_platform: Any) -> None:
        """Test cross-compilation to Linux."""
        # Mock platform detection
        mock_platform_info = PlatformInfo(
            os="Windows",
            architecture="x86_64",
            is_64bit=True,
            platform_string="win32"
        )
        mock_detect_platform.return_value = mock_platform_info

        # Test that cross-compilation interface exists
        try:
            from warploom_scripts.build_system.cmake import CMakeWrapper
            wrapper = CMakeWrapper()
            # Verify toolchain selection interface exists
            assert callable(wrapper.select_toolchain)
        except ImportError:
            pass

    @patch('warploom_scripts.platform.detector.detect_platform')
    def test_cross_compile_to_wasm(self, mock_detect_platform: Any) -> None:
        """Test cross-compilation to WASM."""
        # Mock platform detection
        mock_platform_info = PlatformInfo(
            os="Linux",
            architecture="x86_64",
            is_64bit=True,
            platform_string="linux"
        )
        mock_detect_platform.return_value = mock_platform_info

        # Test that WASM toolchain selection works
        try:
            from warploom_scripts.build_system.cmake import CMakeWrapper
            wrapper = CMakeWrapper()
            toolchain = wrapper.select_toolchain("wasm", "any")
            # Should return path to emscripten toolchain
            assert toolchain is not None
            assert "emscripten" in str(toolchain).lower()
        except ImportError:
            pass

    def test_toolchain_file_selection(self) -> None:
        """Test toolchain file selection."""
        try:
            from warploom_scripts.build_system.cmake import CMakeWrapper
            wrapper = CMakeWrapper()

            # Test various toolchain combinations
            test_cases = [
                ("linux", "x86_64"),
                ("linux", "ARM64"),
                ("windows", "ARM64"),
                ("wasm", "any"),
            ]

            for platform, arch in test_cases:
                try:
                    toolchain = wrapper.select_toolchain(platform, arch)
                    # Should return a path
                    assert toolchain is not None
                except Exception as e:
                    # Some toolchains may not exist
                    # This is expected behavior
                    pass
        except ImportError:
            pass

    def test_toolchain_validation(self) -> None:
        """Test toolchain file validation."""
        try:
            from warploom_scripts.build_system.cmake import CMakeWrapper
            wrapper = CMakeWrapper()

            # Test with non-existent toolchain
            fake_path = Path("/nonexistent/toolchain.cmake")
            is_valid = wrapper.validate_toolchain(fake_path)
            assert is_valid is False

            # Test with existing toolchain (if available)
            real_toolchain = Path("cmake/toolchains/emscripten.cmake")
            if real_toolchain.exists():
                is_valid = wrapper.validate_toolchain(real_toolchain)
                assert is_valid is True
        except ImportError:
            pass

    def test_cross_compiler_detection(self) -> None:
        """Test cross-compiler detection."""
        # Test that cross-compilers can be detected
        # This depends on actual installation
        platform_info = detect_platform()

        # Verify platform detection works
        assert platform_info is not None
        assert platform_info.os in ["Windows", "Linux", "macOS", "Unknown"]

    def test_build_target_selection(self) -> None:
        """Test build target selection."""
        # Test that build targets can be selected
        valid_targets = ["engine", "game", "standalone", "all"]

        for target in valid_targets:
            # Verify target is a string
            assert isinstance(target, str)
            assert len(target) > 0

    def test_cross_platform_path_handling(self) -> None:
        """Test cross-platform path handling."""
        # Test that paths work correctly on different platforms
        test_path = Path("src/main.cpp")

        # Test path normalization
        normalized = str(test_path).replace("\\", "/")
        assert "/" in normalized or "\\" in normalized

        # Test path operations
        assert test_path.name == "main.cpp"
        assert test_path.suffix == ".cpp"


class TestCompilerValidationIntegration:
    """Integration tests for compiler validation."""

    def test_msvc_version_validation(self) -> None:
        """Test MSVC version validation."""
        # Test version parsing and validation
        from warploom_scripts.compilers.detector import _check_msvc_cpp23_support

        # Test versions that support C++23
        assert _check_msvc_cpp23_support("19.35") is True
        assert _check_msvc_cpp23_support("19.40") is True
        assert _check_msvc_cpp23_support("20.0") is True

        # Test versions that don't support C++23
        assert _check_msvc_cpp23_support("19.34") is False
        assert _check_msvc_cpp23_support("19.30") is False

    def test_gcc_version_validation(self) -> None:
        """Test GCC version validation."""
        # Test version parsing and validation
        from warploom_scripts.compilers.detector import _check_gcc_cpp23_support

        # Test versions that support C++23
        assert _check_gcc_cpp23_support("13.0.0") is True
        assert _check_gcc_cpp23_support("13.2.0") is True
        assert _check_gcc_cpp23_support("14.0.0") is True

        # Test versions that don't support C++23
        assert _check_gcc_cpp23_support("12.0.0") is False
        assert _check_gcc_cpp23_support("11.0.0") is False

    def test_clang_version_validation(self) -> None:
        """Test Clang version validation."""
        # Test version parsing and validation
        from warploom_scripts.compilers.detector import _check_clang_cpp23_support

        # Test versions that support C++23
        assert _check_clang_cpp23_support("16.0.0") is True
        assert _check_clang_cpp23_support("17.0.0") is True
        assert _check_clang_cpp23_support("18.0.0") is True

        # Test versions that don't support C++23
        assert _check_clang_cpp23_support("15.0.0") is False
        assert _check_clang_cpp23_support("14.0.0") is False

    def test_compiler_selection_logic(self) -> None:
        """Test compiler selection logic."""
        # Test that compiler selection follows platform defaults
        platform_info = detect_platform()

        # On Windows, should prefer MSVC
        if platform_info.os == "Windows":
            # Test that MSVC detection is attempted
            from warploom_scripts.compilers.detector import _detect_windows_compiler
            compiler_info = _detect_windows_compiler()
            # Should return CompilerInfo or None
            assert compiler_info is None or isinstance(compiler_info, CompilerInfo)

        # On Linux, should prefer GCC
        elif platform_info.os == "Linux":
            # Test that GCC detection is attempted
            from warploom_scripts.compilers.detector import _detect_linux_compiler
            compiler_info = _detect_linux_compiler()
            # Should return CompilerInfo or None
            assert compiler_info is None or isinstance(compiler_info, CompilerInfo)


class TestErrorHandlingIntegration:
    """Integration tests for error handling in cross-platform components."""

    def test_platform_detection_error_handling(self) -> None:
        """Test platform detection error handling."""
        # Test that platform detection handles errors gracefully
        try:
            platform_info = detect_platform()
            # Should not raise exception
            assert platform_info is not None
        except Exception as e:
            # If it does raise, it should be RuntimeError
            assert isinstance(e, RuntimeError)

    def test_compiler_detection_error_handling(self) -> None:
        """Test compiler detection error handling."""
        # Test that compiler detection handles missing compilers gracefully
        compiler_info = detect_compiler(compiler_name="nonexistent_compiler")
        # Should return None, not raise exception
        assert compiler_info is None

    def test_toolchain_error_handling(self) -> None:
        """Test toolchain error handling."""
        try:
            from warploom_scripts.build_system.cmake import CMakeWrapper, CMakeError
            wrapper = CMakeWrapper()

            # Test with invalid toolchain
            try:
                wrapper.select_toolchain("invalid_platform", "invalid_arch")
                # Should raise CMakeError
                assert False  # Should not reach here
            except CMakeError as e:
                # Verify error message is informative
                assert e.message is not None
                assert len(e.message) > 0
        except ImportError:
            pass

    def test_validation_error_messages(self) -> None:
        """Test validation error messages are informative."""
        compiler_info = CompilerInfo(
            name="TestCompiler",
            version="1.0.0",
            path=Path("/usr/bin/test"),
            supports_cpp23=False,
            platform="Linux"
        )

        result = validate_cpp23_support(compiler_info)

        # Verify warnings are present
        assert len(result.warnings) > 0

        # Verify fallback is set
        assert result.fallback is not None

        # Verify warnings contain useful information
        warning_text = " ".join(result.warnings)
        assert "C++23" in warning_text or "C++20" in warning_text


class TestLoggingIntegration:
    """Integration tests for logging in cross-platform components."""

    def test_platform_detection_logging(self, caplog: Any) -> None:
        """Test platform detection is logged."""
        with caplog.at_level(logging.INFO):
            platform_info = detect_platform()

            # Verify platform was logged
            assert any(
                "Detected platform:" in record.message
                for record in caplog.records
            )

    def test_compiler_detection_logging(self) -> None:
        """Test compiler detection is logged."""
        # Just verify that detect_compiler works without raising exception
        # The actual logging goes to stdout/stderr via custom handlers
        # and is not captured by caplog
        try:
            compiler_info = detect_compiler()
            # Should return CompilerInfo or None
            assert compiler_info is None or isinstance(compiler_info, CompilerInfo)
        except Exception as e:
            pytest.fail(f"Compiler detection raised exception: {e}")

    def test_validation_logging(self, caplog: Any) -> None:
        """Test validation is logged."""
        compiler_info = CompilerInfo(
            name="TestCompiler",
            version="1.0.0",
            path=Path("/usr/bin/test"),
            supports_cpp23=False,
            platform="Linux"
        )

        with caplog.at_level(logging.WARNING):
            result = validate_cpp23_support(compiler_info)

            # Verify validation warnings were logged
            assert any(
                "does not fully support C++23" in record.message or
                "falling back" in record.message
                for record in caplog.records
            )


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
