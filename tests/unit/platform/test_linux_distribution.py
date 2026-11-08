#!/usr/bin/env python3
"""
Unit tests for Linux distribution detection.

Tests Linux distribution detection functionality including
distribution identification, package manager detection, and CachyOS detection.
"""

from __future__ import annotations

import sys
import os
import builtins
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch, MagicMock, mock_open

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent))

from warploom_scripts.platform.linux import (
    LinuxDistribution,
    PackageManager,
    detect_linux_distribution,
    is_cachyos,
    detect_package_manager,
)


def _mock_open_os_release(os_release_content: str):
    """
    Create a mock for open() that only intercepts /etc/os-release.
    
    This allows the logging configuration file to be loaded normally
    while mocking the /etc/os-release file content.
    
    Args:
        os_release_content: The content to return for /etc/os-release
        
    Returns:
        A mock function that can be used with patch()
    """
    # Save reference to the real open() function before mocking
    _real_open = open
    
    def _open_side_effect(file_path, *args, **kwargs):
        """
        Side effect function for open() that returns different content based on file path.
        
        Args:
            file_path: The path to the file being opened
            
        Returns:
            A file-like object with the appropriate content
        """
        file_path_str = str(file_path)
        if "/etc/os-release" in file_path_str or "os-release" in file_path_str:
            # Return a mock file object for /etc/os-release
            mock_file = MagicMock()
            mock_file.__enter__ = MagicMock(return_value=mock_file)
            mock_file.__exit__ = MagicMock(return_value=False)
            mock_file.read = MagicMock(return_value=os_release_content)
            return mock_file
        else:
            # Use the real open() for all other files (like logging config)
            return _real_open(file_path, *args, **kwargs)
    
    return _open_side_effect


class TestLinuxDistributionDetection(unittest.TestCase):
    """Test cases for Linux distribution detection."""

    def setUp(self):
        """Set up test fixtures."""
        # Clear cache before each test
        import warploom_scripts.platform.linux as linux_module
        linux_module._distribution_cache = None
        linux_module._package_manager_cache = None

    def test_cachyos_detection(self):
        """Test CachyOS detection."""
        cachyos_content = """ID=cachyos
NAME=CachyOS
VERSION=2023.12.01
VERSION_ID=2023.12.01
ID_LIKE=arch
PRETTY_NAME="CachyOS"
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            with patch("builtins.open", side_effect=_mock_open_os_release(cachyos_content)):
                distro = detect_linux_distribution()

                self.assertEqual(distro.name, "CachyOS")
                self.assertEqual(distro.version, "2023.12.01")
                self.assertEqual(distro.family, "arch")
                self.assertEqual(distro.package_manager, "pacman")
                self.assertTrue(distro.is_cachyos)

    def test_arch_linux_detection(self):
        """Test Arch Linux detection."""
        arch_content = """ID=arch
NAME=Arch Linux
PRETTY_NAME="Arch Linux"
ID_LIKE=arch
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            with patch("builtins.open", side_effect=_mock_open_os_release(arch_content)):
                distro = detect_linux_distribution()

                self.assertEqual(distro.name, "Arch Linux")
                self.assertEqual(distro.version, "")
                self.assertEqual(distro.family, "arch")
                self.assertEqual(distro.package_manager, "pacman")
                self.assertFalse(distro.is_cachyos)

    def test_ubuntu_detection(self):
        """Test Ubuntu detection."""
        ubuntu_content = """ID=ubuntu
NAME=Ubuntu
VERSION="22.04.3 LTS (Jammy Jellyfish)"
VERSION_ID="22.04"
ID_LIKE=debian
PRETTY_NAME="Ubuntu 22.04.3 LTS"
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            with patch("builtins.open", side_effect=_mock_open_os_release(ubuntu_content)):
                distro = detect_linux_distribution()

                self.assertEqual(distro.name, "Ubuntu 22.04.3 LTS")
                self.assertEqual(distro.version, "22.04.3 LTS")
                self.assertEqual(distro.family, "debian")
                self.assertEqual(distro.package_manager, "apt")
                self.assertFalse(distro.is_cachyos)

    def test_debian_detection(self):
        """Test Debian detection."""
        debian_content = """ID=debian
NAME=Debian GNU/Linux
VERSION="12 (bookworm)"
VERSION_ID="12"
ID_LIKE=debian
PRETTY_NAME="Debian GNU/Linux 12 (bookworm)"
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            with patch("builtins.open", side_effect=_mock_open_os_release(debian_content)):
                distro = detect_linux_distribution()

                self.assertEqual(distro.name, "Debian GNU/Linux 12 (bookworm)")
                self.assertEqual(distro.version, "12 (bookworm)")
                self.assertEqual(distro.family, "debian")
                self.assertEqual(distro.package_manager, "apt")
                self.assertFalse(distro.is_cachyos)

    def test_fedora_detection(self):
        """Test Fedora detection."""
        fedora_content = """ID=fedora
NAME=Fedora Linux
VERSION="38 (Workstation Edition)"
VERSION_ID="38"
ID_LIKE=fedora
PRETTY_NAME="Fedora Linux 38 (Workstation Edition)"
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            with patch("builtins.open", side_effect=_mock_open_os_release(fedora_content)):
                distro = detect_linux_distribution()

                self.assertEqual(distro.name, "Fedora Linux 38 (Workstation Edition)")
                self.assertEqual(distro.version, "38 (Workstation Edition)")
                self.assertEqual(distro.family, "fedora")
                self.assertEqual(distro.package_manager, "dnf")
                self.assertFalse(distro.is_cachyos)

    def test_opensuse_detection(self):
        """Test openSUSE detection."""
        opensuse_content = """ID=opensuse
NAME=openSUSE Tumbleweed
VERSION="20240115"
VERSION_ID="20240115"
ID_LIKE=suse opensuse
PRETTY_NAME="openSUSE Tumbleweed"
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            with patch("builtins.open", side_effect=_mock_open_os_release(opensuse_content)):
                distro = detect_linux_distribution()

                self.assertEqual(distro.name, "openSUSE Tumbleweed 20240115")
                self.assertEqual(distro.version, "20240115")
                self.assertEqual(distro.family, "suse")
                self.assertEqual(distro.package_manager, "zypper")
                self.assertFalse(distro.is_cachyos)

    def test_unknown_distribution(self):
        """Test unknown distribution handling."""
        unknown_content = """ID=unknown
NAME=Unknown Linux
VERSION="1.0"
PRETTY_NAME="Unknown Linux 1.0"
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            with patch("builtins.open", side_effect=_mock_open_os_release(unknown_content)):
                distro = detect_linux_distribution()

                self.assertEqual(distro.name, "Unknown Linux 1.0")
                self.assertEqual(distro.version, "1.0")
                self.assertEqual(distro.family, "unknown")
                self.assertEqual(distro.package_manager, "unknown")
                self.assertFalse(distro.is_cachyos)

    def test_missing_os_release(self):
        """Test handling of missing /etc/os-release."""
        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = False

            distro = detect_linux_distribution()

            self.assertEqual(distro.name, "Unknown")
            self.assertEqual(distro.version, "")
            self.assertEqual(distro.family, "unknown")
            self.assertEqual(distro.package_manager, "unknown")
            self.assertFalse(distro.is_cachyos)

    def test_cachyos_vs_arch_differentiation(self):
        """Test CachyOS vs Arch Linux differentiation."""
        # Test Arch Linux first
        arch_content = """ID=arch
NAME=Arch Linux
ID_LIKE=arch
"""

        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            # Clear cache
            import warploom_scripts.platform.linux as linux_module
            linux_module._distribution_cache = None

            with patch("builtins.open", side_effect=_mock_open_os_release(arch_content)):
                arch_distro = detect_linux_distribution()

                self.assertFalse(arch_distro.is_cachyos)
                self.assertEqual(arch_distro.name, "Arch Linux")

            # Clear cache and test CachyOS
            linux_module._distribution_cache = None
            cachyos_content = """ID=cachyos
NAME=CachyOS
ID_LIKE=arch
"""

            with patch("builtins.open", side_effect=_mock_open_os_release(cachyos_content)):
                cachyos_distro = detect_linux_distribution()

                self.assertTrue(cachyos_distro.is_cachyos)
                self.assertEqual(cachyos_distro.name, "CachyOS")

    def test_is_cachyos_function(self):
        """Test is_cachyos() function."""
        with patch("pathlib.Path.exists") as mock_exists:
            mock_exists.return_value = True

            # Test CachyOS
            import warploom_scripts.platform.linux as linux_module
            linux_module._distribution_cache = None

            cachyos_content = """ID=cachyos
NAME=CachyOS
"""
            with patch("builtins.open", side_effect=_mock_open_os_release(cachyos_content)):
                self.assertTrue(is_cachyos())

            # Clear cache and test non-CachyOS
            linux_module._distribution_cache = None
            arch_content = """ID=arch
NAME=Arch Linux
"""
            with patch("builtins.open", side_effect=_mock_open_os_release(arch_content)):
                self.assertFalse(is_cachyos())


class TestPackageManagerDetection(unittest.TestCase):
    """Test cases for package manager detection."""

    def setUp(self):
        """Set up test fixtures."""
        # Clear cache before each test
        import warploom_scripts.platform.linux as linux_module
        linux_module._package_manager_cache = None

    @patch("warploom_scripts.platform.linux._find_executable")
    def test_pacman_detection(self, mock_find):
        """Test pacman detection."""
        mock_find.side_effect = lambda name: Path("/usr/bin/pacman") if name == "pacman" else None

        pm = detect_package_manager()

        self.assertEqual(pm.name, "pacman")
        self.assertEqual(pm.command, "pacman")
        self.assertEqual(pm.family, "arch")

    @patch("warploom_scripts.platform.linux._find_executable")
    def test_apt_detection(self, mock_find):
        """Test apt detection."""
        mock_find.side_effect = lambda name: Path("/usr/bin/apt") if name == "apt" else None

        pm = detect_package_manager()

        self.assertEqual(pm.name, "apt")
        self.assertEqual(pm.command, "apt")
        self.assertEqual(pm.family, "debian")

    @patch("warploom_scripts.platform.linux._find_executable")
    def test_apt_get_detection(self, mock_find):
        """Test apt-get detection."""
        mock_find.side_effect = lambda name: Path("/usr/bin/apt-get") if name == "apt-get" else None

        pm = detect_package_manager()

        self.assertEqual(pm.name, "apt")
        self.assertEqual(pm.command, "apt-get")
        self.assertEqual(pm.family, "debian")

    @patch("warploom_scripts.platform.linux._find_executable")
    def test_dnf_detection(self, mock_find):
        """Test dnf detection."""
        mock_find.side_effect = lambda name: Path("/usr/bin/dnf") if name == "dnf" else None

        pm = detect_package_manager()

        self.assertEqual(pm.name, "dnf")
        self.assertEqual(pm.command, "dnf")
        self.assertEqual(pm.family, "fedora")

    @patch("warploom_scripts.platform.linux._find_executable")
    def test_yum_detection(self, mock_find):
        """Test yum detection."""
        mock_find.side_effect = lambda name: Path("/usr/bin/yum") if name == "yum" else None

        pm = detect_package_manager()

        self.assertEqual(pm.name, "dnf")
        self.assertEqual(pm.command, "yum")
        self.assertEqual(pm.family, "fedora")

    @patch("warploom_scripts.platform.linux._find_executable")
    def test_zypper_detection(self, mock_find):
        """Test zypper detection."""
        mock_find.side_effect = lambda name: Path("/usr/bin/zypper") if name == "zypper" else None

        pm = detect_package_manager()

        self.assertEqual(pm.name, "zypper")
        self.assertEqual(pm.command, "zypper")
        self.assertEqual(pm.family, "suse")

    @patch("warploom_scripts.platform.linux._find_executable")
    def test_no_package_manager(self, mock_find):
        """Test no package manager detection."""
        mock_find.return_value = None

        pm = detect_package_manager()

        self.assertEqual(pm.name, "unknown")
        self.assertEqual(pm.command, "")
        self.assertEqual(pm.family, "unknown")


if __name__ == "__main__":
    unittest.main()
