"""
Configure controller for WarploomController.

This module handles the configure command, which configures the build system
with CMake, including build type, generator, toolchain, and preset options.
It also handles Conan and vcpkg dependency configuration.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional

from warploom_scripts.build_system.cmake import CMakeWrapper
from warploom_scripts.build_system.conan import ConanWrapper
from warploom_scripts.build_system.vcpkg import VcpkgWrapper
from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import ConfigurationError, ToolchainError


class ConfigureController(BaseController):
    """Controller for the configure command.

    Handles CMake configuration with support for:
    - Build type selection (Debug, Release, RelWithDebInfo, MinSizeRel)
    - CMake generator selection
    - Toolchain file specification
    - CMake preset usage
    - Conan dependency configuration
    - vcpkg dependency configuration
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize the configure controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.build_type = getattr(args, "build_type", "Release")
        self.generator = getattr(args, "generator", None)
        self.toolchain = getattr(args, "toolchain", None)
        self.preset = getattr(args, "preset", None)
        self.compiler = getattr(args, "compiler", None)
        self.configure_conan = getattr(args, "configure_conan", False)
        self.configure_vcpkg = getattr(args, "configure_vcpkg", False)

    def validate_arguments(self) -> None:
        """Validate configure command arguments.

        Raises:
            ConfigurationError: If arguments are invalid.
        """
        # Validate build type
        self.validate_build_type(self.build_type)

        # Validate compiler if specified
        self.validate_compiler(self.compiler)

        # Validate that at least one of generator, toolchain, or preset is specified
        if not self.generator and not self.toolchain and not self.preset:
            raise ConfigurationError(
                message="At least one of --generator, --toolchain, or --preset must be specified",
                command="configure",
                config_file=Path(""),
                validation_errors=[
                    "No configuration method specified",
                    "Please provide --generator, --toolchain, or --preset"
                ],
                context={
                    "build_type": self.build_type,
                    "generator": self.generator,
                    "toolchain": self.toolchain,
                    "preset": self.preset,
                },
            )

        # Validate toolchain file if specified
        if self.toolchain:
            toolchain_path = Path(self.toolchain)
            self.validate_file_path(toolchain_path, must_exist=True)

        # Validate preset if specified
        if self.preset:
            cmake_wrapper = CMakeWrapper(source_dir=self.get_project_root())
            preset = cmake_wrapper.get_preset(self.preset)
            if preset is None:
                raise ConfigurationError(
                    message=f"CMake preset not found: {self.preset}",
                    command="configure",
                    config_file=Path("CMakePresets.json"),
                    validation_errors=[f"Preset '{self.preset}' does not exist"],
                    context={"preset": self.preset},
                )

    def configure_cmake(self) -> int:
        """Configure CMake project.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info(f"Configuring CMake with build type: {self.build_type}")

        try:
            cmake_wrapper = CMakeWrapper(
                source_dir=self.get_project_root(),
                build_dir=self.get_project_root() / "build",
            )

            # Convert toolchain to Path if specified
            toolchain_path = Path(self.toolchain) if self.toolchain else None

            result = cmake_wrapper.configure(
                build_type=self.build_type,
                generator=self.generator,
                toolchain=toolchain_path,
                preset=self.preset,
            )

            if result == 0:
                self.logger.info("CMake configuration completed successfully")
            else:
                self.logger.error("CMake configuration failed")

            return result
        except Exception as e:
            self.logger.error(f"Failed to configure CMake: {e}")
            raise ConfigurationError(
                message=f"CMake configuration failed: {e}",
                command="configure",
                config_file=Path("CMakeLists.txt"),
                validation_errors=[str(e)],
            ) from e

    def configure_conan_dependencies(self) -> int:
        """Configure Conan dependencies.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Configuring Conan dependencies")

        try:
            conan_wrapper = ConanWrapper(project_dir=self.get_project_root())

            # Determine profile based on build type
            profile = f"{self.build_type.lower()}-profile"

            # Check if profile exists, create if needed
            if not conan_wrapper.validate_profile(profile):
                self.logger.warning(f"Conan profile not found: {profile}")
                self.logger.info("Skipping Conan configuration")
                return 0

            result = conan_wrapper.install(
                profile=profile,
                build_type=self.build_type,
            )

            if result == 0:
                self.logger.info("Conan configuration completed successfully")
            else:
                self.logger.error("Conan configuration failed")

            return result
        except Exception as e:
            self.logger.error(f"Failed to configure Conan: {e}")
            raise ConfigurationError(
                message=f"Conan configuration failed: {e}",
                command="configure",
                config_file=Path("conan/conanfile.py"),
                validation_errors=[str(e)],
            ) from e

    def configure_vcpkg_dependencies(self) -> int:
        """Configure vcpkg dependencies.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Configuring vcpkg dependencies")

        try:
            vcpkg_wrapper = VcpkgWrapper(project_dir=self.get_project_root())

            # Select appropriate triplet based on platform
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
                self.logger.info("No vcpkg.json found, skipping vcpkg configuration")
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
                self.logger.info("vcpkg configuration completed successfully")
            else:
                self.logger.error("vcpkg configuration failed")

            return result
        except Exception as e:
            self.logger.error(f"Failed to configure vcpkg: {e}")
            raise ConfigurationError(
                message=f"vcpkg configuration failed: {e}",
                command="configure",
                config_file=Path("vcpkg.json"),
                validation_errors=[str(e)],
            ) from e

    def validate_configuration(self) -> bool:
        """Validate that the configuration was successful.

        Returns:
            True if configuration is valid, False otherwise.
        """
        self.logger.info("Validating configuration")

        build_dir = self.get_project_root() / "build"

        # Check if build directory exists
        if not build_dir.exists():
            self.logger.error("Build directory does not exist")
            return False

        # Check for CMakeCache.txt
        cmake_cache = build_dir / "CMakeCache.txt"
        if not cmake_cache.exists():
            self.logger.error("CMakeCache.txt not found")
            return False

        # Check for CMakeFiles directory
        cmake_files = build_dir / "CMakeFiles"
        if not cmake_files.exists():
            self.logger.error("CMakeFiles directory not found")
            return False

        self.logger.info("Configuration validation passed")
        return True

    def execute(self) -> int:
        """Execute the configure command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("configure")

        try:
            # Validate arguments
            self.validate_arguments()

            # Configure CMake
            result = self.configure_cmake()
            if result != 0:
                return result

            # Configure Conan if requested
            if self.configure_conan:
                result = self.configure_conan_dependencies()
                if result != 0:
                    return result

            # Configure vcpkg if requested
            if self.configure_vcpkg:
                result = self.configure_vcpkg_dependencies()
                if result != 0:
                    return result

            # Validate configuration
            if not self.validate_configuration():
                self.logger.error("Configuration validation failed")
                return 3

            self.log_command_success("configure")
            return 0

        except ConfigurationError as e:
            self.log_command_error("configure", e)
            return e.exit_code
        except Exception as e:
            self.logger.exception(f"Unexpected error during configure: {e}")
            return 1


def configure(args: argparse.Namespace) -> int:
    """Configure the build system.

    This function provides the command-line interface for the configure command.

    Args:
        args: Parsed command-line arguments containing:
            - build_type: Build type (Debug, Release, RelWithDebInfo, MinSizeRel)
            - generator: CMake generator name
            - toolchain: Path to toolchain file
            - preset: CMake preset name
            - configure_conan: Configure Conan dependencies
            - configure_vcpkg: Configure vcpkg dependencies

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    controller = ConfigureController(args)
    return controller.execute()


__all__ = [
    "ConfigureController",
    "configure",
]
