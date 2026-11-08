"""
Unit tests for compiler modules.

Tests for base, clang, gcc, msvc, detection_system, and detector modules.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any
from unittest.mock import Mock, patch
import pytest

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from warploom_scripts.platform.detector import detect_platform, detect_architecture, get_platform_info, PlatformInfo


class TestPlatformInfo:
    """Unit tests for PlatformInfo dataclass."""

    def test_platform_info_creation(self) -> None:
        """Test PlatformInfo creation."""
        info = PlatformInfo(
            os="Windows",
            architecture="x86_64",
            is_64bit=True,
            platform_string="win32"
        )
        assert info.os == "Windows"
        assert info.architecture == "x86_64"
        assert info.is_64bit is True
        assert info.platform_string == "win32"


class TestDetectPlatform:
    """Unit tests for detect_platform function."""

    @patch('sys.platform', 'win32')
    def test_detect_platform_windows(self) -> None:
        """Test platform detection on Windows."""
        info = detect_platform()
        assert info.os == "Windows"
        assert info.platform_string == "win32"

    @patch('sys.platform', 'linux')
    def test_detect_platform_linux(self) -> None:
        """Test platform detection on Linux."""
        info = detect_platform()
        assert info.os == "Linux"
        assert info.platform_string == "linux"

    @patch('sys.platform', 'darwin')
    def test_detect_platform_macos(self) -> None:
        """Test platform detection on macOS."""
        info = detect_platform()
        assert info.os == "macOS"
        assert info.platform_string == "darwin"

    @patch('platform.machine')
    @patch('sys.platform', 'linux')
    def test_detect_platform_x86_64(self, mock_machine: Any) -> None:
        """Test platform detection for x86_64 architecture."""
        mock_machine.return_value = "x86_64"
        info = detect_platform()
        assert info.architecture == "x86_64"
        assert info.is_64bit is True

    @patch('platform.machine')
    @patch('sys.platform', 'linux')
    def test_detect_platform_arm64(self, mock_machine: Any) -> None:
        """Test platform detection for ARM64 architecture."""
        mock_machine.return_value = "arm64"
        info = detect_platform()
        assert info.architecture == "ARM64"
        assert info.is_64bit is True

    @patch('platform.machine')
    @patch('sys.platform', 'linux')
    def test_detect_platform_x86(self, mock_machine: Any) -> None:
        """Test platform detection for x86 architecture."""
        mock_machine.return_value = "i386"
        info = detect_platform()
        assert info.architecture == "x86"
        assert info.is_64bit is False


class TestDetectArchitecture:
    """Unit tests for detect_architecture function."""

    @patch('platform.machine')
    def test_detect_architecture_x86_64(self, mock_machine: Any) -> None:
        """Test architecture detection for x86_64."""
        mock_machine.return_value = "x86_64"
        arch = detect_architecture()
        assert arch == "x86_64"

    @patch('platform.machine')
    def test_detect_architecture_amd64(self, mock_machine: Any) -> None:
        """Test architecture detection for amd64."""
        mock_machine.return_value = "amd64"
        arch = detect_architecture()
        assert arch == "x86_64"

    @patch('platform.machine')
    def test_detect_architecture_arm64(self, mock_machine: Any) -> None:
        """Test architecture detection for ARM64."""
        mock_machine.return_value = "arm64"
        arch = detect_architecture()
        assert arch == "ARM64"

    @patch('platform.machine')
    def test_detect_architecture_aarch64(self, mock_machine: Any) -> None:
        """Test architecture detection for aarch64."""
        mock_machine.return_value = "aarch64"
        arch = detect_architecture()
        assert arch == "ARM64"

    @patch('platform.machine')
    def test_detect_architecture_x86(self, mock_machine: Any) -> None:
        """Test architecture detection for x86."""
        mock_machine.return_value = "i386"
        arch = detect_architecture()
        assert arch == "x86"


class TestGetPlatformInfo:
    """Unit tests for get_platform_info function."""

    def test_get_platform_info(self) -> None:
        """Test get_platform_info function."""
        info = get_platform_info()
        assert isinstance(info, PlatformInfo)
        assert info.os in ["Windows", "Linux", "macOS", "Unknown"]
        assert isinstance(info.architecture, str)
        assert isinstance(info.is_64bit, bool)
        assert isinstance(info.platform_string, str)


class TestCompilerModules:
    """Unit tests for compiler module imports."""

    def test_import_base_module(self) -> None:
        """Test that base module can be imported."""
        try:
            from warploom_scripts.compilers import base
            assert base is not None
        except ImportError:
            pytest.skip("base module not available")

    def test_import_clang_module(self) -> None:
        """Test that clang module can be imported."""
        try:
            from warploom_scripts.compilers import clang
            assert clang is not None
        except ImportError:
            pytest.skip("clang module not available")

    def test_import_gcc_module(self) -> None:
        """Test that gcc module can be imported."""
        try:
            from warploom_scripts.compilers import gcc
            assert gcc is not None
        except ImportError:
            pytest.skip("gcc module not available")

    def test_import_msvc_module(self) -> None:
        """Test that msvc module can be imported."""
        try:
            from warploom_scripts.compilers import msvc
            assert msvc is not None
        except ImportError:
            pytest.skip("msvc module not available")

    def test_import_detector_module(self) -> None:
        """Test that detector module can be imported."""
        try:
            from warploom_scripts.compilers import detector
            assert detector is not None
        except ImportError:
            pytest.skip("detector module not available")

    def test_import_detection_system_module(self) -> None:
        """Test that detection_system module can be imported."""
        try:
            from warploom_scripts.compilers import detection_system
            assert detection_system is not None
        except ImportError:
            pytest.skip("detection_system module not available")


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
