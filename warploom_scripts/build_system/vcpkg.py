"""
vcpkg package manager integration module for OmniCPP build system.

Provides vcpkg package installation, triplet management,
and vcpkg integration with CMake.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional

from warploom_scripts.logging.logger import get_logger
from warploom_scripts.utils.command_utils import execute_command


logger = get_logger(__name__)


class VcpkgError(Exception):
    """Base exception for vcpkg-related errors."""

    def __init__(self, message: str, command: str, exit_code: int = 1) -> None:
        """Initialize vcpkg error.

        Args:
            message: Human-readable error message.
            command: The vcpkg command that failed.
            exit_code: The exit code to return.
        """
        self.message = message
        self.command = command
        self.exit_code = exit_code
        super().__init__(self.message)


class VcpkgWrapper:
    """Wrapper class for vcpkg operations.

    Provides high-level interface for vcpkg package management,
    triplet selection, and CMake integration.
    """

    def __init__(
        self,
        vcpkg_path: Optional[Path] = None,
        project_dir: Optional[Path] = None,
    ) -> None:
        """Initialize vcpkg wrapper.

        Args:
            vcpkg_path: Path to vcpkg executable or root directory.
            project_dir: Path to project directory (default: current directory).
        """
        self.project_dir = project_dir or Path.cwd()

        # Determine vcpkg executable path
        if vcpkg_path:
            if vcpkg_path.is_dir():
                # vcpkg root directory provided
                self.vcpkg_root: Optional[Path] = vcpkg_path
                self.vcpkg_exe = vcpkg_path / "vcpkg.exe" if self._is_windows() else vcpkg_path / "vcpkg"
            else:
                # vcpkg executable path provided
                self.vcpkg_exe = vcpkg_path
                self.vcpkg_root = vcpkg_path.parent
        else:
            # Try to find vcpkg in PATH
            self.vcpkg_exe = Path("vcpkg")
            self.vcpkg_root = None

        logger.debug(
            f"VcpkgWrapper initialized: vcpkg={self.vcpkg_exe}, "
            f"root={self.vcpkg_root}, project={self.project_dir}"
        )

    @staticmethod
    def _is_windows() -> bool:
        """Check if running on Windows."""
        import platform
        return platform.system().lower() == "windows"

    def install(
        self,
        packages: List[str],
        triplet: str,
        extra_args: Optional[List[str]] = None,
    ) -> int:
        """Install vcpkg packages.

        Args:
            packages: List of package names to install.
            triplet: vcpkg triplet (e.g., x64-windows, x64-linux).
            extra_args: Additional vcpkg arguments.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            VcpkgError: If vcpkg install fails.
        """
        logger.info(f"Installing vcpkg packages: {packages} with triplet: {triplet}")

        # Build vcpkg command
        cmd_parts = [str(self.vcpkg_exe), "install"]

        # Add packages
        cmd_parts.extend(packages)

        # Add triplet
        cmd_parts.extend(["--triplet", triplet])

        # Add extra arguments
        if extra_args:
            cmd_parts.extend(extra_args)

        cmd = " ".join(cmd_parts)
        logger.debug(f"vcpkg install command: {cmd}")

        try:
            execute_command(cmd, timeout=3600)
            logger.info("vcpkg install completed successfully")
            return 0
        except Exception as e:
            error_msg = f"vcpkg install failed: {e}"
            logger.error(error_msg)
            raise VcpkgError(error_msg, cmd) from e

    def remove(
        self,
        packages: List[str],
        triplet: str,
        extra_args: Optional[List[str]] = None,
    ) -> int:
        """Remove vcpkg packages.

        Args:
            packages: List of package names to remove.
            triplet: vcpkg triplet (e.g., x64-windows, x64-linux).
            extra_args: Additional vcpkg arguments.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            VcpkgError: If vcpkg remove fails.
        """
        logger.info(f"Removing vcpkg packages: {packages} with triplet: {triplet}")

        # Build vcpkg command
        cmd_parts = [str(self.vcpkg_exe), "remove"]

        # Add packages
        cmd_parts.extend(packages)

        # Add triplet
        cmd_parts.extend(["--triplet", triplet])

        # Add extra arguments
        if extra_args:
            cmd_parts.extend(extra_args)

        cmd = " ".join(cmd_parts)
        logger.debug(f"vcpkg remove command: {cmd}")

        try:
            execute_command(cmd, timeout=600)
            logger.info("vcpkg remove completed successfully")
            return 0
        except Exception as e:
            error_msg = f"vcpkg remove failed: {e}"
            logger.error(error_msg)
            raise VcpkgError(error_msg, cmd) from e

    def integrate(self) -> int:
        """Integrate vcpkg with CMake.

        Generates CMake toolchain and build information files.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            VcpkgError: If integration fails.
        """
        logger.info("Integrating vcpkg with CMake")

        # Build vcpkg command
        cmd_parts = [str(self.vcpkg_exe), "integrate", "install"]

        cmd = " ".join(cmd_parts)
        logger.debug(f"vcpkg integrate command: {cmd}")

        try:
            execute_command(cmd, timeout=300)
            logger.info("vcpkg integration completed successfully")
            return 0
        except Exception as e:
            error_msg = f"vcpkg integration failed: {e}"
            logger.error(error_msg)
            raise VcpkgError(error_msg, cmd) from e

    def list_packages(self) -> List[Dict[str, str]]:
        """List installed vcpkg packages.

        Returns:
            List of package dictionaries containing name and version.
        """
        logger.info("Listing installed vcpkg packages")

        packages = []

        try:
            result = subprocess.run(
                [str(self.vcpkg_exe), "list"],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if result.returncode == 0:
                # Parse output to extract package information
                for line in result.stdout.split("\n"):
                    line = line.strip()
                    if ":" in line:
                        parts = line.split(":")
                        if len(parts) >= 2:
                            name = parts[0].strip()
                            version = parts[1].strip()
                            packages.append({"name": name, "version": version})

            logger.info(f"Found {len(packages)} installed packages")
        except Exception as e:
            logger.warning(f"Failed to list vcpkg packages: {e}")

        return packages

    def search(self, query: str) -> List[Dict[str, str]]:
        """Search for vcpkg packages.

        Args:
            query: Search query string.

        Returns:
            List of package dictionaries containing name and description.
        """
        logger.info(f"Searching vcpkg packages: {query}")

        packages = []

        try:
            result = subprocess.run(
                [str(self.vcpkg_exe), "search", query],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if result.returncode == 0:
                # Parse output to extract package information
                for line in result.stdout.split("\n"):
                    line = line.strip()
                    if line and not line.startswith("Found"):
                        # Extract package name and description
                        parts = line.split(maxsplit=1)
                        if parts:
                            name = parts[0]
                            description = parts[1] if len(parts) > 1 else ""
                            packages.append({"name": name, "description": description})

            logger.info(f"Found {len(packages)} packages")
        except Exception as e:
            logger.warning(f"Failed to search vcpkg packages: {e}")

        return packages

    def list_triplets(self) -> List[str]:
        """List available vcpkg triplets.

        Returns:
            List of triplet names.
        """
        logger.info("Listing vcpkg triplets")

        triplets = []

        # Check vcpkg root directory
        if self.vcpkg_root:
            triplets_dir = self.vcpkg_root / "triplets"
            if triplets_dir.exists():
                for triplet_file in triplets_dir.glob("*.cmake"):
                    triplets.append(triplet_file.stem)

        # Also check community triplets
        if self.vcpkg_root:
            community_triplets_dir = self.vcpkg_root / "triplets" / "community"
            if community_triplets_dir.exists():
                for triplet_file in community_triplets_dir.glob("*.cmake"):
                    triplets.append(triplet_file.stem)

        # Remove duplicates and sort
        triplets = sorted(list(set(triplets)))
        logger.info(f"Found {len(triplets)} vcpkg triplets")
        return triplets

    def get_triplet(self, triplet_name: str) -> Optional[Dict[str, str]]:
        """Get vcpkg triplet by name.

        Args:
            triplet_name: Name of triplet to retrieve.

        Returns:
            Triplet dictionary if found, None otherwise.
        """
        logger.debug(f"Getting vcpkg triplet: {triplet_name}")

        if not self.vcpkg_root:
            return None

        # Check triplets directory
        triplets_dir = self.vcpkg_root / "triplets"
        triplet_file = triplets_dir / f"{triplet_name}.cmake"

        if not triplet_file.exists():
            # Check community triplets
            community_triplets_dir = self.vcpkg_root / "triplets" / "community"
            triplet_file = community_triplets_dir / f"{triplet_name}.cmake"

        if triplet_file.exists():
            try:
                content = triplet_file.read_text(encoding="utf-8")
                # Parse CMake triplet file
                triplet = {}
                for line in content.split("\n"):
                    line = line.strip()
                    if line.startswith("set("):
                        # Extract variable assignments
                        parts = line[4:].split()
                        if len(parts) >= 2:
                            var_name = parts[0]
                            var_value = " ".join(parts[1:-1])  # Remove trailing )
                            triplet[var_name] = var_value
                return triplet
            except Exception as e:
                logger.warning(f"Failed to read triplet {triplet_name}: {e}")

        return None

    def select_triplet(
        self,
        target_platform: str,
        target_arch: str,
        build_type: str = "Release",
    ) -> str:
        """Select appropriate vcpkg triplet for target.

        Args:
            target_platform: Target platform (windows, linux, macos).
            target_arch: Target architecture (x86, x64, arm64).
            build_type: Build type (Debug, Release).

        Returns:
            Triplet name.

        Raises:
            VcpkgError: If no suitable triplet is found.
        """
        logger.info(f"Selecting vcpkg triplet for {target_platform}-{target_arch}")

        # Build triplet name based on target
        # Common vcpkg triplet naming convention: <arch>-<platform>
        triplet_map = {
            ("windows", "x86"): "x86-windows",
            ("windows", "x64"): "x64-windows",
            ("windows", "arm64"): "arm64-windows",
            ("linux", "x86"): "x86-linux",
            ("linux", "x64"): "x64-linux",
            ("linux", "arm64"): "arm64-linux",
            ("macos", "x64"): "x64-osx",
            ("macos", "arm64"): "arm64-osx",
        }

        key = (target_platform.lower(), target_arch.lower())
        if key not in triplet_map:
            raise VcpkgError(
                f"No vcpkg triplet found for {target_platform}-{target_arch}",
                "select_triplet",
            )

        triplet = triplet_map[key]
        logger.info(f"Selected vcpkg triplet: {triplet}")
        return triplet

    def export(self, packages: List[str], output_dir: Path) -> int:
        """Export vcpkg packages.

        Args:
            packages: List of package names to export.
            output_dir: Directory to export packages to.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            VcpkgError: If export fails.
        """
        logger.info(f"Exporting vcpkg packages: {packages}")

        # Build vcpkg command
        cmd_parts = [str(self.vcpkg_exe), "export"]

        # Add packages
        cmd_parts.extend(packages)

        # Add output directory
        cmd_parts.extend(["--output", str(output_dir)])

        cmd = " ".join(cmd_parts)
        logger.debug(f"vcpkg export command: {cmd}")

        try:
            execute_command(cmd, timeout=600)
            logger.info("vcpkg export completed successfully")
            return 0
        except Exception as e:
            error_msg = f"vcpkg export failed: {e}"
            logger.error(error_msg)
            raise VcpkgError(error_msg, cmd) from e

    def get_version(self) -> Optional[str]:
        """Get vcpkg version.

        Returns:
            vcpkg version string if available, None otherwise.
        """
        try:
            result = subprocess.run(
                [str(self.vcpkg_exe), "version"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode == 0:
                # Parse version from output (e.g., "vcpkg package management program version 2023-12-12-hash")
                version_line = result.stdout.split("\n")[0]
                version = version_line.split()[-1]
                logger.debug(f"vcpkg version: {version}")
                return version
        except Exception as e:
            logger.warning(f"Failed to get vcpkg version: {e}")

        return None

    def validate_triplet(self, triplet_name: str) -> bool:
        """Validate vcpkg triplet.

        Args:
            triplet_name: Name of triplet to validate.

        Returns:
            True if triplet is valid, False otherwise.
        """
        logger.debug(f"Validating vcpkg triplet: {triplet_name}")

        triplet = self.get_triplet(triplet_name)
        if triplet is None:
            logger.error(f"Triplet not found: {triplet_name}")
            return False

        # Check for required variables
        required_vars = ["VCPKG_TARGET_ARCHITECTURE", "VCPKG_CRT_LINKAGE"]
        for var in required_vars:
            if var not in triplet:
                logger.warning(f"Triplet may be missing required variable: {var}")

        logger.debug(f"Triplet validation passed: {triplet_name}")
        return True


def vcpkg_install(args: Dict[str, Any]) -> int:
    """Install vcpkg packages with given arguments.

    This function provides a command-line interface for vcpkg installation.

    Args:
        args: Dictionary containing installation arguments:
            - packages: List of package names
            - triplet: vcpkg triplet
            - vcpkg_path: Path to vcpkg executable
            - project_dir: Project directory path

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    try:
        wrapper = VcpkgWrapper(
            vcpkg_path=Path(args["vcpkg_path"]) if args.get("vcpkg_path") else None,
            project_dir=Path(args.get("project_dir", ".")),
        )

        return wrapper.install(
            packages=args["packages"],
            triplet=args["triplet"],
        )
    except VcpkgError as e:
        logger.error(f"vcpkg install failed: {e.message}")
        return e.exit_code
    except Exception as e:
        logger.error(f"Unexpected error during vcpkg install: {e}")
        return 1


def vcpkg_integrate(args: Dict[str, Any]) -> int:
    """Integrate vcpkg with CMake.

    Args:
        args: Dictionary containing integration arguments:
            - vcpkg_path: Path to vcpkg executable
            - project_dir: Project directory path

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    try:
        wrapper = VcpkgWrapper(
            vcpkg_path=Path(args["vcpkg_path"]) if args.get("vcpkg_path") else None,
            project_dir=Path(args.get("project_dir", ".")),
        )

        return wrapper.integrate()
    except VcpkgError as e:
        logger.error(f"vcpkg integration failed: {e.message}")
        return e.exit_code
    except Exception as e:
        logger.error(f"Unexpected error during vcpkg integration: {e}")
        return 1


__all__ = [
    "VcpkgError",
    "VcpkgWrapper",
    "vcpkg_install",
    "vcpkg_integrate",
]
