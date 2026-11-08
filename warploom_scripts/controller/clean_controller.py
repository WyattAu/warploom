"""
Clean controller for WarploomController.

This module handles clean command, which removes build artifacts
and temporary files from the project.
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path
from typing import List

from warploom_scripts.build_system.cmake import CMakeWrapper
from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import InvalidTargetError


class CleanController(BaseController):
    """Controller for clean command.

    Handles cleaning build artifacts, including:
    - Target-specific cleaning (engine, game, standalone, all)
    - Build directory removal
    - Temporary file cleanup
    - CMake cache cleanup
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize clean controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.target = getattr(args, "target", "all")

    def validate_arguments(self) -> None:
        """Validate clean command arguments.

        Raises:
            InvalidTargetError: If target is invalid.
        """
        # Validate target
        self.validate_target(self.target)

    def get_clean_directories(self) -> List[Path]:
        """Get list of directories to clean based on target.

        Returns:
            List of directory paths to clean.
        """
        project_root = self.get_project_root()
        directories = []

        # Always clean build directory
        build_dir = project_root / "build"
        if build_dir.exists():
            directories.append(build_dir)

        # Target-specific cleaning
        if self.target == "all":
            # Clean all build artifacts
            directories.extend([
                project_root / "packages",
                project_root / "CPM_modules",
                project_root / ".pytest_cache",
            ])
        elif self.target == "engine":
            # Clean engine-specific artifacts
            engine_build = build_dir / "engine"
            if engine_build.exists():
                directories.append(engine_build)
        elif self.target == "game":
            # Clean game-specific artifacts
            game_build = build_dir / "game"
            if game_build.exists():
                directories.append(game_build)
        elif self.target == "standalone":
            # Clean standalone-specific artifacts
            standalone_build = build_dir / "standalone"
            if standalone_build.exists():
                directories.append(standalone_build)

        return directories

    def get_clean_files(self) -> List[Path]:
        """Get list of files to clean.

        Returns:
            List of file paths to clean.
        """
        project_root = self.get_project_root()
        files: List[Path] = []

        # Common temporary files
        patterns = [
            "*.o",
            "*.obj",
            "*.a",
            "*.lib",
            "*.dll",
            "*.so",
            "*.dylib",
            "*.exe",
            "*.pyc",
            "*.pyo",
            "__pycache__",
            ".DS_Store",
            "*.log",
        ]

        for pattern in patterns:
            files.extend(project_root.rglob(pattern))

        return files

    def clean_directory(self, directory: Path) -> int:
        """Clean a specific directory.

        Args:
            directory: Directory path to clean.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info(f"Cleaning directory: {directory}")

        try:
            if not directory.exists():
                self.logger.info(f"Directory does not exist: {directory}")
                return 0

            if directory.is_file():
                directory.unlink()
                self.logger.info(f"Removed file: {directory}")
            else:
                shutil.rmtree(directory)
                self.logger.info(f"Removed directory: {directory}")

            return 0
        except Exception as e:
            self.logger.error(f"Failed to clean directory {directory}: {e}")
            return 1

    def clean_file(self, file_path: Path) -> int:
        """Clean a specific file.

        Args:
            file_path: File path to clean.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.debug(f"Cleaning file: {file_path}")

        try:
            if not file_path.exists():
                return 0

            if file_path.is_file():
                file_path.unlink()
            elif file_path.is_dir():
                shutil.rmtree(file_path)

            return 0
        except Exception as e:
            self.logger.error(f"Failed to clean file {file_path}: {e}")
            return 1

    def clean_cmake_cache(self) -> int:
        """Clean CMake cache files.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.info("Cleaning CMake cache")

        try:
            cmake_wrapper = CMakeWrapper(
                source_dir=self.get_project_root(),
                build_dir=self.get_project_root() / "build",
            )

            result = cmake_wrapper.clean(target=None)
            return result
        except Exception as e:
            self.logger.error(f"Failed to clean CMake cache: {e}")
            return 1

    def execute(self) -> int:
        """Execute clean command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("clean")

        try:
            # Validate arguments
            self.validate_arguments()

            # Get directories to clean
            directories = self.get_clean_directories()

            # Clean CMake cache FIRST (before deleting build directory)
            result = self.clean_cmake_cache()
            if result != 0:
                self.logger.warning("CMake cache clean returned non-zero, continuing...")

            # Clean directories
            for directory in directories:
                result = self.clean_directory(directory)
                if result != 0:
                    self.logger.error(f"Failed to clean directory: {directory}")
                    # Continue cleaning other directories even if one fails

            # Clean files (optional, based on target)
            if self.target == "all":
                files = self.get_clean_files()
                for file_path in files:
                    result = self.clean_file(file_path)
                    if result != 0:
                        self.logger.error(f"Failed to clean file: {file_path}")
                        # Continue cleaning other files even if one fails

            self.log_command_success("clean")
            return 0

        except InvalidTargetError as e:
            self.log_command_error("clean", e)
            return e.exit_code
        except Exception as e:
            self.logger.exception(f"Unexpected error during clean: {e}")
            return 1


def clean(args: argparse.Namespace) -> int:
    """Clean build artifacts.

    This function provides a command-line interface for clean command.

    Args:
        args: Parsed command-line arguments containing:
            - target: Target to clean (engine, game, standalone, all)

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    controller = CleanController(args)
    return controller.execute()


__all__ = [
    "CleanController",
    "clean",
]
