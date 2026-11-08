"""
Install controller for WarploomController.

This module handles install command, which installs dependencies
and built artifacts to the system.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import List

from warploom_scripts.build_system.cmake import CMakeWrapper
from warploom_scripts.build_system.conan import ConanWrapper
from warploom_scripts.build_system.vcpkg import VcpkgWrapper
from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import InvalidTargetError


class InstallController(BaseController):
    """Controller for install command.

    Handles installation of:
    - Dependencies (Conan, vcpkg)
    - Built artifacts (CMake install)
    - Target-specific installation
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize install controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.target = getattr(args, "target", "all")
        self.config = getattr(args, "config", "release")
        self.compiler = getattr(args, "compiler", None)
        self.install_dependencies = getattr(args, "install_dependencies", False)
        self.install_prefix = getattr(args, "prefix", None)

    def validate_arguments(self) -> None:
        """Validate install command arguments.

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

        # Validate prefix if specified
        if self.install_prefix:
            prefix_path = Path(self.install_prefix)
            self.validate_directory(prefix_path, must_exist=False)

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

    def get_install_target(self) -> str:
        """Get CMake install target from target argument.

        Returns:
            CMake install target name.
        """
        target_map = {
            "engine": "install-engine",
            "game": "install-game",
            "standalone": "install-standalone",
            "all": "install",
        }
        return target_map.get(self.target.lower(), "install")

    def install_conan_dependencies(self) -> int:
        """Install Conan dependencies.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Installing Conan dependencies")

        try:
            conan_wrapper = ConanWrapper(project_dir=self.get_project_root())

            # Determine profile based on config
            build_type = self.get_cmake_config()
            profile = f"{build_type.lower()}-profile"

            # Check if profile exists
            if not conan_wrapper.validate_profile(profile):
                self.logger.warning(f"Conan profile not found: {profile}")
                self.logger.info("Skipping Conan installation")
                return 0

            result = conan_wrapper.install(
                profile=profile,
                build_type=build_type,
            )

            if result == 0:
                self.logger.info("Conan dependencies installed successfully")
            else:
                self.logger.error("Conan installation failed")

            return result
        except Exception as e:
            self.logger.error(f"Failed to install Conan dependencies: {e}")
            return 1

    def install_vcpkg_dependencies(self) -> int:
        """Install vcpkg dependencies.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Installing vcpkg dependencies")

        try:
            vcpkg_wrapper = VcpkgWrapper(project_dir=self.get_project_root())

            # Select appropriate triplet
            import platform
            system = platform.system().lower()
            if system == "windows":
                triplet = "x64-windows"
            elif system == "linux":
                triplet = "x64-linux"
            elif system == "darwin":
                triplet = "x64-osx"
            else:
                self.logger.warning(f"Unsupported platform for vcpkg: {system}")
                return 0

            # Read vcpkg.json to get dependencies
            vcpkg_json = self.get_project_root() / "vcpkg.json"
            if not vcpkg_json.exists():
                self.logger.info("No vcpkg.json found, skipping vcpkg installation")
                return 0

            import json
            with open(vcpkg_json, "r", encoding="utf-8") as f:
                vcpkg_data = json.load(f)

            dependencies = vcpkg_data.get("dependencies", [])
            if not dependencies:
                self.logger.info("No vcpkg dependencies found")
                return 0

            result = vcpkg_wrapper.install(
                packages=dependencies,
                triplet=triplet,
            )

            if result == 0:
                self.logger.info("vcpkg dependencies installed successfully")
            else:
                self.logger.error("vcpkg installation failed")

            return result
        except Exception as e:
            self.logger.error(f"Failed to install vcpkg dependencies: {e}")
            return 1

    def install_cmake_target(self) -> int:
        """Install CMake target.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        cmake_config = self.get_cmake_config()
        install_target = self.get_install_target()

        self.logger.info(f"Installing CMake target: {install_target} ({cmake_config})")

        try:
            cmake_wrapper = CMakeWrapper(
                source_dir=self.get_project_root(),
                build_dir=self.get_project_root() / "build",
            )

            # Determine install prefix
            prefix = Path(self.install_prefix) if self.install_prefix else None

            result = cmake_wrapper.install(
                prefix=prefix,
                config=cmake_config,
            )

            if result == 0:
                self.logger.info(f"CMake installation completed successfully: {install_target}")
            else:
                self.logger.error(f"CMake installation failed: {install_target}")

            return result
        except Exception as e:
            self.logger.error(f"Failed to install CMake target: {e}")
            return 1

    def validate_installation(self) -> bool:
        """Validate that installation was successful.

        Returns:
            True if installation is valid, False otherwise.
        """
        self.logger.info("Validating installation")

        # Check if install directory exists
        install_dir = self.get_project_root() / "build" / "install"
        if not install_dir.exists():
            self.logger.warning("Install directory does not exist")
            return False

        # Check for target-specific files
        if self.target == "all":
            # Check for any installed files
            found_files = False
            for pattern in ["*.exe", "*.dll", "*.so", "*.dylib", "*.a", "*.lib", "*.h", "*.hpp"]:
                if list(install_dir.rglob(pattern)):
                    found_files = True
                    break

            if not found_files:
                self.logger.warning("No installed files found")
                return False
        else:
            # Check for specific target files
            target_patterns = {
                "engine": ["WarploomEngine.*"],
                "game": ["WarploomGame.*"],
                "standalone": ["WarploomStandalone.*"],
            }

            patterns = target_patterns.get(self.target, [])
            found_files = False

            for pattern in patterns:
                if list(install_dir.rglob(pattern)):
                    found_files = True
                    break

            if not found_files:
                self.logger.warning(f"No installed files found for target: {self.target}")
                return False

        self.logger.info("Installation validation passed")
        return True

    def execute(self) -> int:
        """Execute install command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("install")

        try:
            # Validate arguments
            self.validate_arguments()

            # Install dependencies if requested
            if self.install_dependencies:
                result = self.install_conan_dependencies()
                if result != 0:
                    return result

                result = self.install_vcpkg_dependencies()
                if result != 0:
                    return result

            # Install CMake target
            result = self.install_cmake_target()
            if result != 0:
                return result

            # Validate installation
            if not self.validate_installation():
                self.logger.error("Installation validation failed")
                return 5

            self.log_command_success("install")
            return 0

        except InvalidTargetError as e:
            self.log_command_error("install", e)
            return e.exit_code
        except Exception as e:
            self.logger.exception(f"Unexpected error during install: {e}")
            return 1


def install(args: argparse.Namespace) -> int:
    """Install dependencies and built artifacts.

    This function provides a command-line interface for install command.

    Args:
        args: Parsed command-line arguments containing:
            - target: Target to install (engine, game, standalone, all)
            - config: Build configuration (debug, release)
            - install_dependencies: Install dependencies (optional)
            - prefix: Installation prefix path (optional)

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    controller = InstallController(args)
    return controller.execute()


__all__ = [
    "InstallController",
    "install",
]
