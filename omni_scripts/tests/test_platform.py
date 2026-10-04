"""
Unit tests for platform modules.

Tests for detector, windows, linux, and macos modules.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any
from unittest.mock import patch, MagicMock
import pytest

# Add parent directories to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from omni_scripts.platform.detector import detect_platform, detect_architecture, get_platform_info, PlatformInfo

# Import Linux platform module for CachyOS tests
try:
    from omni_scripts.platform import linux
    from omni_scripts.platform.linux import (
        LinuxDistribution,
        PackageManager,
        CachyOSInfo,
        NixInfo,
        detect_linux_distribution,
        is_cachyos,
        get_cachyos_info,
        get_cachyos_compiler_flags,
        get_cachyos_linker_flags,
        get_cachyos_package_manager_config,
        is_nix_environment,
        get_nix_info,
    )
    LINUX_AVAILABLE = True
except ImportError:
    LINUX_AVAILABLE = False


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


class TestPlatformModules:
    """Unit tests for platform module imports."""

    def test_import_detector_module(self) -> None:
        """Test that detector module can be imported."""
        try:
            from omni_scripts.platform import detector
            assert detector is not None
        except ImportError:
            pytest.skip("detector module not available")

    def test_import_windows_module(self) -> None:
        """Test that windows module can be imported."""
        try:
            from omni_scripts.platform import windows
            assert windows is not None
        except ImportError:
            pytest.skip("windows module not available")

    def test_import_linux_module(self) -> None:
        """Test that linux module can be imported."""
        try:
            from omni_scripts.platform import linux
            assert linux is not None
        except ImportError:
            pytest.skip("linux module not available")

    def test_import_macos_module(self) -> None:
        """Test that macos module can be imported."""
        try:
            from omni_scripts.platform import macos
            assert macos is not None
        except ImportError:
            pytest.skip("macos module not available")


@pytest.mark.skipif(not LINUX_AVAILABLE, reason="Linux module not available")
class TestCachyOSDetection:
    """Unit tests for CachyOS detection functionality."""

    @patch('builtins.open', new_callable=MagicMock)
    @patch('pathlib.Path.exists')
    def test_cachyos_detection(self, mock_exists: MagicMock, mock_open: MagicMock) -> None:
        """Test CachyOS detection from /etc/os-release."""
        mock_exists.return_value = True
        mock_open.return_value.__enter__.return_value.read.return_value = (
            'ID=cachyos\n'
            'NAME="CachyOS"\n'
            'VERSION="2024.01.01"\n'
            'VERSION_ID="2024.01.01"\n'
            'ID_LIKE=arch\n'
        )

        # Clear cache
        linux._distribution_cache = None

        distro = detect_linux_distribution()
        assert distro.is_cachyos is True
        assert "CachyOS" in distro.name
        assert distro.family == "arch"
        assert distro.package_manager == "pacman"

    @patch('builtins.open', new_callable=MagicMock)
    @patch('pathlib.Path.exists')
    def test_arch_linux_detection(self, mock_exists: MagicMock, mock_open: MagicMock) -> None:
        """Test Arch Linux detection (not CachyOS)."""
        mock_exists.return_value = True
        mock_open.return_value.__enter__.return_value.read.return_value = (
            'ID=arch\n'
            'NAME="Arch Linux"\n'
            'VERSION=""\n'
            'ID_LIKE=arch\n'
        )

        # Clear cache
        linux._distribution_cache = None

        distro = detect_linux_distribution()
        assert distro.is_cachyos is False
        assert "Arch Linux" in distro.name
        assert distro.family == "arch"
        assert distro.package_manager == "pacman"

    def test_is_cachyos_function(self) -> None:
        """Test is_cachyos() function."""
        # Clear cache
        linux._distribution_cache = None

        # This test will pass on CachyOS, fail on other systems
        # We just verify the function returns a boolean
        result = is_cachyos()
        assert isinstance(result, bool)

    @patch('builtins.open', new_callable=MagicMock)
    @patch('pathlib.Path.exists')
    @patch('subprocess.run')
    def test_get_cachyos_info(self, mock_run: MagicMock, mock_exists: MagicMock,
                                   mock_open: MagicMock) -> None:
        """Test get_cachyos_info() function."""
        mock_exists.return_value = True
        mock_open.return_value.__enter__.return_value.read.return_value = (
            'ID=cachyos\n'
            'NAME="CachyOS KDE"\n'
            'VERSION="2024.01.01"\n'
            'ID_LIKE=arch\n'
        )
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = "6.6.8-cachyos\n"

        # Clear cache
        linux._distribution_cache = None

        info = get_cachyos_info()
        assert info is not None
        assert isinstance(info, CachyOSInfo)
        assert info.version == "2024.01.01"
        assert info.variant == "kde"
        assert info.kernel_version == "6.6.8-cachyos"
        assert info.gcc_version == "14.2.1"  # 2024 release uses GCC 14
        assert info.supports_lto is True
        assert info.supports_native_optimizations is True


@pytest.mark.skipif(not LINUX_AVAILABLE, reason="Linux module not available")
class TestCachyOSCompilerFlags:
    """Unit tests for CachyOS compiler flags."""

    def test_cachyos_gcc_release_flags(self) -> None:
        """Test CachyOS GCC release build flags."""
        flags = get_cachyos_compiler_flags('gcc', 'release')

        assert '-march=native' in flags
        assert '-O3' in flags
        assert '-flto' in flags
        assert '-DNDEBUG' in flags
        assert '-fstack-protector-strong' in flags
        assert '-D_FORTIFY_SOURCE=2' in flags
        assert '-Wall' in flags
        assert '-Wextra' in flags
        assert '-Wpedantic' in flags

    def test_cachyos_gcc_debug_flags(self) -> None:
        """Test CachyOS GCC debug build flags."""
        flags = get_cachyos_compiler_flags('gcc', 'debug')

        assert '-g' in flags
        assert '-O0' in flags
        assert '-DDEBUG' in flags
        assert '-fstack-protector-strong' in flags
        assert '-D_FORTIFY_SOURCE=2' in flags
        assert '-Wall' in flags
        assert '-Wextra' in flags
        assert '-Wpedantic' in flags

    def test_cachyos_clang_release_flags(self) -> None:
        """Test CachyOS Clang release build flags."""
        flags = get_cachyos_compiler_flags('clang', 'release')

        assert '-march=native' in flags
        assert '-O3' in flags
        assert '-flto' in flags
        assert '-DNDEBUG' in flags
        assert '-fstack-protector-strong' in flags
        assert '-D_FORTIFY_SOURCE=2' in flags
        assert '-Wall' in flags
        assert '-Wextra' in flags
        assert '-Wpedantic' in flags

    def test_cachyos_clang_debug_flags(self) -> None:
        """Test CachyOS Clang debug build flags."""
        flags = get_cachyos_compiler_flags('clang', 'debug')

        assert '-g' in flags
        assert '-O0' in flags
        assert '-DDEBUG' in flags
        assert '-fstack-protector-strong' in flags
        assert '-D_FORTIFY_SOURCE=2' in flags
        assert '-Wall' in flags
        assert '-Wextra' in flags
        assert '-Wpedantic' in flags

    def test_cachyos_compiler_flags_invalid_compiler(self) -> None:
        """Test CachyOS compiler flags with invalid compiler."""
        with pytest.raises(ValueError, match="Invalid compiler"):
            get_cachyos_compiler_flags('invalid', 'release')

    def test_cachyos_compiler_flags_invalid_build_type(self) -> None:
        """Test CachyOS compiler flags with invalid build type."""
        with pytest.raises(ValueError, match="Invalid build type"):
            get_cachyos_compiler_flags('gcc', 'invalid')


@pytest.mark.skipif(not LINUX_AVAILABLE, reason="Linux module not available")
class TestCachyOSLinkerFlags:
    """Unit tests for CachyOS linker flags."""

    def test_cachyos_linker_flags(self) -> None:
        """Test CachyOS linker flags."""
        flags = get_cachyos_linker_flags()

        assert '-Wl,--as-needed' in flags
        assert '-Wl,--no-undefined' in flags
        assert '-Wl,-z,relro' in flags
        assert '-Wl,-z,now' in flags


@pytest.mark.skipif(not LINUX_AVAILABLE, reason="Linux module not available")
class TestCachyOSPackageManagerConfig:
    """Unit tests for CachyOS package manager configuration."""

    def test_cachyos_package_manager_config(self) -> None:
        """Test CachyOS package manager configuration."""
        config = get_cachyos_package_manager_config()

        assert isinstance(config, dict)
        assert config['package_manager'] == 'pacman'
        assert config['pacman_conf'] == '/etc/pacman.conf'
        assert config['mirrorlist'] == '/etc/pacman.d/mirrorlist'
        assert config['cache_dir'] == '/var/cache/pacman/pkg'
        assert config['database_dir'] == '/var/lib/pacman'
        assert config['log_file'] == '/var/log/pacman.log'
        assert config['use_color'] == 'always'
        assert config['verbose_pkg_lists'] == 'true'
        assert config['check_space'] == 'true'
        assert config['parallel_downloads'] == '5'


@pytest.mark.skipif(not LINUX_AVAILABLE, reason="Linux module not available")
class TestNixDetection:
    """Unit tests for Nix environment detection functionality."""

    @patch.dict(os.environ, {'IN_NIX_SHELL': '1'})
    def test_is_nix_environment_in_nix_shell(self) -> None:
        """Test Nix environment detection when IN_NIX_SHELL is set."""
        # Clear cache
        linux._nix_environment_cache = None

        result = is_nix_environment()
        assert result is True

    @patch.dict(os.environ, {}, clear=True)
    def test_is_nix_environment_not_in_nix_shell(self) -> None:
        """Test Nix environment detection when not in Nix shell."""
        # Clear cache
        linux._nix_environment_cache = None

        result = is_nix_environment()
        assert result is False

    @patch.dict(os.environ, {'NIX_PATH': '/nix/var/nix/profiles/default'})
    def test_is_nix_environment_with_nix_path(self) -> None:
        """Test Nix environment detection with NIX_PATH set."""
        # Clear cache
        linux._nix_environment_cache = None

        result = is_nix_environment()
        assert result is True

    @patch.dict(os.environ, {'NIX_PROFILES': '/nix/var/nix/profiles/default'})
    def test_is_nix_environment_with_nix_profiles(self) -> None:
        """Test Nix environment detection with NIX_PROFILES set."""
        # Clear cache
        linux._nix_environment_cache = None

        result = is_nix_environment()
        assert result is True

    @patch.dict(os.environ, {'IN_NIX_SHELL': 'pure'})
    def test_get_nix_info_in_nix_shell(self) -> None:
        """Test get_nix_info when in Nix shell."""
        # Clear cache
        linux._nix_environment_cache = None
        linux._nix_info_cache = None

        info = get_nix_info()
        assert info is not None
        assert isinstance(info, NixInfo)
        assert info.is_nix_environment is True
        assert info.in_nix_shell == 'pure'

    @patch.dict(os.environ, {}, clear=True)
    def test_get_nix_info_not_in_nix_shell(self) -> None:
        """Test get_nix_info when not in Nix shell."""
        # Clear cache
        linux._nix_environment_cache = None
        linux._nix_info_cache = None

        info = get_nix_info()
        assert info is None

    @patch('subprocess.run')
    @patch.dict(os.environ, {'IN_NIX_SHELL': '1'})
    def test_get_nix_info_with_version(self, mock_run: MagicMock) -> None:
        """Test get_nix_info retrieves Nix version."""
        # Clear cache
        linux._nix_environment_cache = None
        linux._nix_info_cache = None

        # Mock nix --version output
        mock_run.return_value.returncode = 0
        mock_run.return_value.stdout = "nix (Nix) 2.18.1"

        info = get_nix_info()
        assert info is not None
        assert info.nix_version == "2.18.1"

    @patch.dict(os.environ, {'NIX_STORE_DIR': '/custom/nix/store'})
    def test_get_nix_info_custom_store_path(self) -> None:
        """Test get_nix_info with custom NIX_STORE_DIR."""
        # Clear cache
        linux._nix_environment_cache = None
        linux._nix_info_cache = None

        info = get_nix_info()
        assert info is not None
        assert info.nix_store_path == '/custom/nix/store'

    @patch.dict(os.environ, {'IN_NIX_SHELL': '1', 'NIX_PROFILES': '/profile1:/profile2'})
    def test_get_nix_info_multiple_profiles(self) -> None:
        """Test get_nix_info with multiple Nix profiles."""
        # Clear cache
        linux._nix_environment_cache = None
        linux._nix_info_cache = None

        info = get_nix_info()
        assert info is not None
        assert len(info.nix_profiles) == 2
        assert '/profile1' in info.nix_profiles
        assert '/profile2' in info.nix_profiles

    @patch.dict(os.environ, {'IN_NIX_SHELL': '1', 'PATH': '/nix/store/.../bin:/usr/bin'})
    def test_get_nix_info_flake_environment(self) -> None:
        """Test get_nix_info detects flake environment."""
        # Clear cache
        linux._nix_environment_cache = None
        linux._nix_info_cache = None

        info = get_nix_info()
        assert info is not None
        # Flake detection looks for /nix/store/ or /.nix-profile/ in PATH
        assert info.is_flake_environment is True

    def test_nix_info_creation(self) -> None:
        """Test NixInfo dataclass creation."""
        info = NixInfo(
            is_nix_environment=True,
            nix_version="2.18.1",
            nix_store_path="/nix/store",
            nix_profiles=["/nix/var/nix/profiles/default"],
            is_flake_environment=True,
            in_nix_shell="pure"
        )

        assert info.is_nix_environment is True
        assert info.nix_version == "2.18.1"
        assert info.nix_store_path == "/nix/store"
        assert len(info.nix_profiles) == 1
        assert info.is_flake_environment is True
        assert info.in_nix_shell == "pure"


if __name__ == '__main__':
    pytest.main([__file__, '-v'])
