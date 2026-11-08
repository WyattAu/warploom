"""
Package controller for WarploomController.

This module handles package command, which creates distribution
packages using CPack with support for different formats.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
from typing import Any, Dict, List

from warploom_scripts.build_system.cmake import CMakeWrapper
from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import ControllerError, InvalidTargetError


class PackageController(BaseController):
    """Controller for package command.

    Handles packaging with CPack, including:
    - Target-specific packaging (engine, game, standalone, all)
    - Package format selection (ZIP, TGZ, DEB, RPM, NSIS, etc.)
    - Build configuration selection
    - Package validation
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize package controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.target = getattr(args, "target", "all")
        self.config = getattr(args, "config", "release")
        self.compiler = getattr(args, "compiler", None)
        self.format = getattr(args, "format", None)
        self.output_dir = getattr(args, "output_dir", None)

    def validate_arguments(self) -> None:
        """Validate package command arguments.

        Raises:
            InvalidTargetError: If target is invalid.
            ControllerError: If other arguments are invalid.
        """
        # Validate target
        self.validate_target(self.target)

        # Validate config
        self.validate_config(self.config)

        # Validate compiler if specified
        self.validate_compiler(self.compiler)

        # Validate format if specified
        if self.format:
            valid_formats = [
                "ZIP",
                "TGZ",
                "7Z",
                "DEB",
                "RPM",
                "NSIS",
                "WIX",
                "CYGWIN",
                "PACKAGEMAKER",
                "OSXX11",
                "NUGET",
                "FREEBSD",
            ]
            if self.format.upper() not in valid_formats:
                raise ControllerError(
                    message=f"Invalid package format '{self.format}'. Valid formats are: {', '.join(valid_formats)}",
                    command="package",
                    context={"format": self.format, "valid_formats": valid_formats},
                    exit_code=2,
                )

        # Validate output directory if specified
        if self.output_dir:
            output_path = Path(self.output_dir)
            self.validate_directory(output_path, must_exist=False)

    def get_cmake_config(self) -> str:
        """Get CMake configuration name from config argument.

        Returns:
            CMake configuration name (Debug, Release, etc.).
        """
        config_map = {
            "debug": "Debug",
            "release": "Release",
        }
        return config_map.get(self.config.lower(), "Release")

    def get_package_target(self) -> str:
        """Get CPack package target from target argument.

        Returns:
            CPack package target name.
        """
        target_map = {
            "engine": "package-engine",
            "game": "package-game",
            "standalone": "package-standalone",
            "all": "package",
        }
        return target_map.get(self.target.lower(), "package")

    def get_default_package_format(self) -> str:
        """Get default package format based on platform.

        Returns:
            Default package format for current platform.
        """
        import platform
        system = platform.system().lower()

        format_map = {
            "windows": "ZIP",
            "linux": "TGZ",
            "darwin": "TGZ",
        }

        return format_map.get(system, "TGZ")

    def run_cpack(self) -> int:
        """Run CPack to create packages.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        cmake_config = self.get_cmake_config()
        package_format = self.format or self.get_default_package_format()

        self.logger.info(f"Creating package: {self.target} ({cmake_config}, {package_format})")

        try:
            # Build CPack command
            cmd_parts = ["cpack"]

            # Add configuration
            cmd_parts.extend(["--config", str(self.get_project_root() / "build" / "CPackConfig.cmake")])

            # Add package format
            cmd_parts.extend(["-G", package_format])

            # Add output directory if specified
            if self.output_dir:
                cmd_parts.extend(["-D", f"CPACK_OUTPUT_DIRECTORY={self.output_dir}"])

            # Add configuration type
            cmd_parts.extend(["-D", f"CMAKE_BUILD_TYPE={cmake_config}"])

            cmd = " ".join(cmd_parts)
            self.logger.debug(f"CPack command: {cmd}")

            result = subprocess.run(cmd, shell=True, cwd=self.get_project_root())

            if result.returncode == 0:
                self.logger.info(f"Package created successfully: {self.target}")
            else:
                self.logger.error(f"Package creation failed: {self.target}")

            return result.returncode
        except Exception as e:
            self.logger.error(f"Failed to run CPack: {e}")
            return 1

    def validate_package(self) -> bool:
        """Validate that package was created successfully.

        Returns:
            True if package is valid, False otherwise.
        """
        self.logger.info("Validating package")

        # Determine output directory
        output_dir = Path(self.output_dir) if self.output_dir else self.get_project_root() / "build"

        # Check for package files
        package_extensions = {
            "ZIP": [".zip"],
            "TGZ": [".tar.gz", ".tgz"],
            "7Z": [".7z"],
            "DEB": [".deb"],
            "RPM": [".rpm"],
            "NSIS": [".exe"],
            "WIX": [".msi"],
            "CYGWIN": [".tar.bz2"],
            "PACKAGEMAKER": [".dmg"],
            "OSXX11": [".dmg"],
            "NUGET": [".nupkg"],
            "FREEBSD": [".txz"],
        }

        package_format = self.format or self.get_default_package_format()
        extensions = package_extensions.get(package_format.upper(), [".zip", ".tar.gz"])

        found_package = False
        for ext in extensions:
            package_files = list(output_dir.glob(f"*{ext}"))
            if package_files:
                found_package = True
                self.logger.info(f"Found package: {package_files[0]}")
                break

        if not found_package:
            self.logger.error("No package file found")
            return False

        # Validate package size
        for ext in extensions:
            package_files = list(output_dir.glob(f"*{ext}"))
            for package_file in package_files:
                size_mb = package_file.stat().st_size / (1024 * 1024)
                self.logger.info(f"Package size: {size_mb:.2f} MB")

                if size_mb < 0.1:
                    self.logger.warning("Package size is very small, may be incomplete")

        return True

    def get_package_info(self) -> Dict[str, Any]:
        """Get information about created packages.

        Returns:
            Dictionary containing package information.
        """
        output_dir = Path(self.output_dir) if self.output_dir else self.get_project_root() / "build"

        package_info: Dict[str, Any] = {
            "target": self.target,
            "config": self.config,
            "format": self.format or self.get_default_package_format(),
            "packages": [],
        }

        # Find package files
        package_extensions = [".zip", ".tar.gz", ".tgz", ".7z", ".deb", ".rpm", ".exe", ".msi", ".dmg", ".nupkg", ".txz"]

        for ext in package_extensions:
            package_files = list(output_dir.glob(f"*{ext}"))
            for package_file in package_files:
                if isinstance(package_info["packages"], list):
                    package_info["packages"].append({
                        "path": str(package_file),
                        "name": package_file.name,
                        "size_mb": package_file.stat().st_size / (1024 * 1024),
                    })

        return package_info

    def execute(self) -> int:
        """Execute package command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("package")

        try:
            # Validate arguments
            self.validate_arguments()

            # Run CPack
            result = self.run_cpack()
            if result != 0:
                return result

            # Validate package
            if not self.validate_package():
                self.logger.error("Package validation failed")
                return 5

            # Log package information
            package_info = self.get_package_info()
            self.logger.info(f"Package information: {package_info}")

            self.log_command_success("package")
            return 0

        except InvalidTargetError as e:
            self.log_command_error("package", e)
            return e.exit_code
        except Exception as e:
            self.logger.exception(f"Unexpected error during package: {e}")
            return 1


def package(args: argparse.Namespace) -> int:
    """Create distribution packages.

    This function provides a command-line interface for package command.

    Args:
        args: Parsed command-line arguments containing:
            - target: Target to package (engine, game, standalone, all)
            - config: Build configuration (debug, release)
            - format: Package format (optional)
            - output_dir: Output directory for packages (optional)

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    controller = PackageController(args)
    return controller.execute()


__all__ = [
    "PackageController",
    "package",
]
