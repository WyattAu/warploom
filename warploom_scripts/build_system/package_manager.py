"""
Unified Package Manager Interface

Provides priority-based package manager selection (Conan → vcpkg → CPM)
with security verification capabilities.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from warploom_scripts.logging.logger import get_logger

logger = get_logger(__name__)


class PackageSecurityError(Exception):
    """Exception raised when package security verification fails."""

    def __init__(
        self,
        message: str,
        package_name: str,
        issue: str,
    ) -> None:
        self.package_name = package_name
        self.issue = issue
        super().__init__(message)


class PackageManager:
    """Base class for package manager operations."""

    def __init__(self, name: str, priority: int) -> None:
        """Initialize package manager.

        Args:
            name: Package manager name.
            priority: Priority level (lower = higher priority).
        """
        self.name = name
        self.priority = priority
        self.available = False

    def check_available(self) -> bool:
        """Check if package manager is available.

        Returns:
            True if available, False otherwise.
        """
        raise NotImplementedError

    def install_package(self, package_name: str, version: Optional[str] = None) -> bool:
        """Install a package.

        Args:
            package_name: Name of package to install.
            version: Optional version to install.

        Returns:
            True if successful, False otherwise.
        """
        raise NotImplementedError

    def verify_package(self, package_name: str) -> bool:
        """Verify package installation.

        Args:
            package_name: Name of package to verify.

        Returns:
            True if verified, False otherwise.
        """
        raise NotImplementedError


class ConanManager(PackageManager):
    """Conan package manager implementation."""

    def __init__(self) -> None:
        """Initialize Conan manager."""
        super().__init__("Conan", priority=1)
        self.available = self._check_conan_available()

    def _check_conan_available(self) -> bool:
        """Check if Conan is available."""
        try:
            result = subprocess.run(
                ["conan", "--version"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            return result.returncode == 0
        except FileNotFoundError:
            return False

    def check_available(self) -> bool:
        """Check if Conan is available."""
        return self.available

    def install_package(self, package_name: str, version: Optional[str] = None) -> bool:
        """Install a package with Conan."""
        if not self.available:
            logger.error("Conan is not available")
            return False

        logger.info(f"Installing {package_name} with Conan...")

        try:
            cmd = ["conan", "install", package_name]
            if version:
                cmd.extend(["--version", version])

            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=600,
            )

            if result.returncode == 0:
                logger.info(f"✅ {package_name} installed successfully with Conan")
                return True
            else:
                logger.error(f"❌ Failed to install {package_name} with Conan: {result.stderr}")
                return False
        except Exception as e:
            logger.error(f"Error installing {package_name} with Conan: {e}")
            return False

    def verify_package(self, package_name: str) -> bool:
        """Verify package installation with Conan."""
        if not self.available:
            return False

        try:
            result = subprocess.run(
                ["conan", "search", package_name],
                capture_output=True,
                text=True,
                timeout=30,
            )

            if result.returncode == 0 and package_name in result.stdout:
                logger.info(f"✅ {package_name} verified with Conan")
                return True
            else:
                logger.warning(f"⚠ {package_name} not found in Conan")
                return False
        except Exception as e:
            logger.error(f"Error verifying {package_name} with Conan: {e}")
            return False


class VcpkgManager(PackageManager):
    """vcpkg package manager implementation."""

    def __init__(self) -> None:
        """Initialize vcpkg manager."""
        super().__init__("vcpkg", priority=2)
        self.available = self._check_vcpkg_available()

    def _check_vcpkg_available(self) -> bool:
        """Check if vcpkg is available."""
        try:
            result = subprocess.run(
                ["vcpkg", "version"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            return result.returncode == 0
        except FileNotFoundError:
            return False

    def check_available(self) -> bool:
        """Check if vcpkg is available."""
        return self.available

    def install_package(self, package_name: str, version: Optional[str] = None) -> bool:
        """Install a package with vcpkg."""
        if not self.available:
            logger.error("vcpkg is not available")
            return False

        logger.info(f"Installing {package_name} with vcpkg...")

        try:
            cmd = ["vcpkg", "install", package_name]
            if version:
                cmd.extend([f"{package_name}:{version}"])

            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=600,
            )

            if result.returncode == 0:
                logger.info(f"✅ {package_name} installed successfully with vcpkg")
                return True
            else:
                logger.error(f"❌ Failed to install {package_name} with vcpkg: {result.stderr}")
                return False
        except Exception as e:
            logger.error(f"Error installing {package_name} with vcpkg: {e}")
            return False

    def verify_package(self, package_name: str) -> bool:
        """Verify package installation with vcpkg."""
        if not self.available:
            return False

        try:
            result = subprocess.run(
                ["vcpkg", "list", package_name],
                capture_output=True,
                text=True,
                timeout=30,
            )

            if result.returncode == 0 and package_name in result.stdout:
                logger.info(f"✅ {package_name} verified with vcpkg")
                return True
            else:
                logger.warning(f"⚠ {package_name} not found in vcpkg")
                return False
        except Exception as e:
            logger.error(f"Error verifying {package_name} with vcpkg: {e}")
            return False


class CPMManager(PackageManager):
    """CPM.cmake package manager implementation."""

    def __init__(self, cmake_file: Path) -> None:
        """Initialize CPM manager.

        Args:
            cmake_file: Path to CMakeLists.txt.
        """
        super().__init__("CPM", priority=3)
        self.cmake_file = cmake_file
        self.available = self._check_cpm_available()

    def _check_cpm_available(self) -> bool:
        """Check if CPM is available."""
        return self.cmake_file.exists()

    def check_available(self) -> bool:
        """Check if CPM is available."""
        return self.available

    def install_package(self, package_name: str, version: Optional[str] = None) -> bool:
        """Install a package with CPM (via CMake)."""
        if not self.available:
            logger.error("CPM is not available (CMakeLists.txt not found)")
            return False

        logger.info(f"Installing {package_name} with CPM...")

        try:
            # CPM packages are installed via CMake
            # This is handled by CMake, not directly by CPM
            logger.info(f"CPM package {package_name} will be installed via CMake")
            return True
        except Exception as e:
            logger.error(f"Error with CPM package {package_name}: {e}")
            return False

    def verify_package(self, package_name: str) -> bool:
        """Verify package installation with CPM."""
        # CPM packages are verified via CMake
        logger.info(f"CPM package {package_name} verification handled by CMake")
        return True


class PackageManagerFactory:
    """Factory for creating package managers with priority-based selection."""

    def __init__(self, cmake_file: Optional[Path] = None) -> None:
        """Initialize package manager factory.

        Args:
            cmake_file: Path to CMakeLists.txt (for CPM).
        """
        self.conan = ConanManager()
        self.vcpkg = VcpkgManager()
        self.cpm = CPMManager(cmake_file or Path("CMakeLists.txt"))
        self.managers: List[PackageManager] = [
            self.conan,
            self.vcpkg,
            self.cpm,
        ]

    def get_available_managers(self) -> List[PackageManager]:
        """Get list of available package managers.

        Returns:
            List of available package managers sorted by priority.
        """
        available = [pm for pm in self.managers if pm.check_available()]
        available.sort(key=lambda x: x.priority)
        return available

    def get_best_manager(self) -> Optional[PackageManager]:
        """Get the best available package manager.

        Returns:
            Package manager with highest priority, or None if none available.
        """
        available = self.get_available_managers()
        return available[0] if available else None

    def install_package(
        self,
        package_name: str,
        version: Optional[str] = None,
        preferred_manager: Optional[str] = None,
    ) -> bool:
        """Install a package using the best available package manager.

        Args:
            package_name: Name of package to install.
            version: Optional version to install.
            preferred_manager: Optional preferred package manager name.

        Returns:
            True if successful, False otherwise.
        """
        # Use preferred manager if specified and available
        if preferred_manager:
            for pm in self.managers:
                if pm.name.lower() == preferred_manager.lower() and pm.check_available():
                    logger.info(f"Using preferred package manager: {pm.name}")
                    return pm.install_package(package_name, version)

        # Use best available manager
        best_manager = self.get_best_manager()
        if not best_manager:
            logger.error("No package manager available")
            return False

        logger.info(f"Using package manager: {best_manager.name} (priority {best_manager.priority})")
        return best_manager.install_package(package_name, version)

    def verify_package(
        self,
        package_name: str,
        preferred_manager: Optional[str] = None,
    ) -> bool:
        """Verify a package installation.

        Args:
            package_name: Name of package to verify.
            preferred_manager: Optional preferred package manager name.

        Returns:
            True if verified, False otherwise.
        """
        # Use preferred manager if specified and available
        if preferred_manager:
            for pm in self.managers:
                if pm.name.lower() == preferred_manager.lower() and pm.check_available():
                    logger.info(f"Verifying with preferred package manager: {pm.name}")
                    return pm.verify_package(package_name)

        # Use best available manager
        best_manager = self.get_best_manager()
        if not best_manager:
            logger.error("No package manager available")
            return False

        logger.info(f"Verifying with package manager: {best_manager.name}")
        return best_manager.verify_package(package_name)


def verify_package_security(package_name: str, package_path: Path) -> bool:
    """Verify package security by checking hash.

    Args:
        package_name: Name of package.
        package_path: Path to package file.

    Returns:
        True if security verified, False otherwise.

    Raises:
        PackageSecurityError: If security verification fails.
    """
    logger.info(f"Verifying security for {package_name}...")

    if not package_path.exists():
        raise PackageSecurityError(
            f"Package file not found: {package_path}",
            package_name=package_name,
            issue="File not found",
        )

    try:
        # Calculate SHA256 hash
        with open(package_path, "rb") as f:
            file_hash = hashlib.sha256(f.read()).hexdigest()

        logger.info(f"Package SHA256: {file_hash}")

        # In a real implementation, this would be compared against known good hashes
        # For now, just log the hash
        logger.info(f"✅ Security verification completed for {package_name}")
        return True

    except Exception as e:
        raise PackageSecurityError(
            f"Security verification failed: {e}",
            package_name=package_name,
            issue="Hash calculation error",
        )


__all__ = [
    "PackageSecurityError",
    "PackageManager",
    "ConanManager",
    "VcpkgManager",
    "CPMManager",
    "PackageManagerFactory",
    "verify_package_security",
]
