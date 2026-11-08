"""
Build controller for WarploomController.

This module handles the build command, which builds project targets
with CMake, including support for clean builds and parallel builds.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional

from warploom_scripts.build_system.cmake import CMakeWrapper
from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import InvalidTargetError


class BuildController(BaseController):
    """Controller for build command.

    Handles building project targets with CMake, including:
    - Target selection (engine, game, standalone, all)
    - Build configuration (debug, release)
    - Compiler selection
    - Clean builds
    - Parallel builds
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize build controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.target = getattr(args, "target", "all")
        self.pipeline = getattr(args, "pipeline", "default")
        self.preset = getattr(args, "preset", "default")
        self.config = getattr(args, "config", "release")
        self.compiler = getattr(args, "compiler", None)
        self.clean = getattr(args, "clean", False)
        self.parallel = getattr(args, "parallel", None)

    def validate_arguments(self) -> None:
        """Validate build command arguments.

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

        # Validate preset exists
        cmake_wrapper = CMakeWrapper(source_dir=self.get_project_root())
        preset = cmake_wrapper.get_preset(self.preset)
        if preset is None:
            self.logger.warning(f"CMake preset not found: {self.preset}")
            self.logger.info("Using default preset")

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

    def get_build_target(self) -> str:
        """Get CMake build target from target argument.

        Returns:
            CMake target name.
        """
        target_map = {
            "engine": "warploom_engine",
            "game": "warploom_pong",
            "standalone": "warploom_standalone",
            "all": "all",
        }
        return target_map.get(self.target.lower(), "all")

    def clean_before_build(self) -> int:
        """Clean build artifacts before building.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Cleaning before build")

        try:
            cmake_wrapper = CMakeWrapper(
                source_dir=self.get_project_root(),
                build_dir=self.get_project_root() / "build",
            )

            result = cmake_wrapper.clean(target=None)
            return result
        except Exception as e:
            self.logger.error(f"Failed to clean before build: {e}")
            return 1

    def build_target(self) -> int:
        """Build the specified target.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        cmake_config = self.get_cmake_config()
        build_target = self.get_build_target()

        self.logger.info(f"Building target: {build_target} ({cmake_config})")

        try:
            cmake_wrapper = CMakeWrapper(
                source_dir=self.get_project_root(),
                build_dir=self.get_project_root() / "build",
            )

            # Determine parallel jobs
            parallel_jobs = self.parallel
            if parallel_jobs is None:
                # Auto-detect number of CPU cores
                import os
                parallel_jobs = os.cpu_count() or 1

            result = cmake_wrapper.build(
                target=build_target,
                config=cmake_config,
                parallel=parallel_jobs,
                clean=False,  # Clean is handled separately
            )

            if result == 0:
                self.logger.info(f"Build completed successfully: {build_target}")
            else:
                self.logger.error(f"Build failed: {build_target}")

            return result
        except Exception as e:
            self.logger.error(f"Failed to build target: {e}")
            return 1

    def validate_build_output(self) -> bool:
        """Validate that build output exists.

        Returns:
            True if build output is valid, False otherwise.
        """
        self.logger.info("Validating build output")

        build_dir = self.get_project_root() / "build"

        # Check if build directory exists
        if not build_dir.exists():
            self.logger.error("Build directory does not exist")
            return False

        # Check for target-specific output
        cmake_config = self.get_cmake_config()
        build_target = self.get_build_target()

        if build_target == "all":
            # Check for any executable or library files
            found_output = False
            for pattern in ["*.exe", "*.dll", "*.so", "*.dylib", "*.a", "*.lib"]:
                if list(build_dir.rglob(pattern)):
                    found_output = True
                    break

            if not found_output:
                self.logger.warning("No build output files found")
                return False
        else:
            # Check for specific target output
            output_patterns = {
                "WarploomEngine": ["WarploomEngine.exe", "libWarploomEngine.*"],
                "WarploomGame": ["WarploomGame.exe", "libWarploomGame.*"],
                "WarploomStandalone": ["WarploomStandalone.exe", "WarploomStandalone"],
            }

            patterns = output_patterns.get(build_target, [])
            found_output = False

            for pattern in patterns:
                if list(build_dir.rglob(pattern)):
                    found_output = True
                    break

            if not found_output:
                self.logger.warning(f"No build output found for target: {build_target}")
                return False

        self.logger.info("Build output validation passed")
        return True

    def execute(self) -> int:
        """Execute build command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("build")

        try:
            # Validate arguments
            self.validate_arguments()

            # Clean before build if requested
            if self.clean:
                result = self.clean_before_build()
                if result != 0:
                    return result

            # Build target
            result = self.build_target()
            if result != 0:
                return result

            # Validate build output
            if not self.validate_build_output():
                self.logger.error("Build output validation failed")
                return 5

            self.log_command_success("build")
            return 0

        except InvalidTargetError as e:
            self.log_command_error("build", e)
            return e.exit_code
        except Exception as e:
            self.logger.exception(f"Unexpected error during build: {e}")
            return 1


def build(args: argparse.Namespace) -> int:
    """Build project targets.

    This function provides a command-line interface for build command.

    Args:
        args: Parsed command-line arguments containing:
            - target: Target to build (engine, game, standalone, all)
            - pipeline: Build pipeline name
            - preset: CMake preset name
            - config: Build configuration (debug, release)
            - compiler: Compiler to use (optional)
            - clean: Clean before building (optional)
            - parallel: Number of parallel jobs (optional)

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    controller = BuildController(args)
    return controller.execute()


__all__ = [
    "BuildController",
    "build",
]
