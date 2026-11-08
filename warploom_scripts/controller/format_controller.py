"""
Format controller for WarploomController.

This module handles format command, which formats code
with clang-format for C++ and black for Python.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
from typing import List

from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import ControllerError


class FormatController(BaseController):
    """Controller for format command.

    Handles code formatting, including:
    - C++ code formatting with clang-format
    - Python code formatting with black
    - Check mode (verify formatting without modifying)
    - Dry-run mode
    - File and directory selection
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize format controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.files = getattr(args, "files", [])
        self.directories = getattr(args, "directories", [])
        self.check = getattr(args, "check", False)
        self.dry_run = getattr(args, "dry_run", False)
        self.cpp_only = getattr(args, "cpp_only", False)
        self.python_only = getattr(args, "python_only", False)

    def validate_arguments(self) -> None:
        """Validate format command arguments.

        Raises:
            ControllerError: If arguments are invalid.
        """
        # Validate that black is available if Python formatting is requested
        if not self.cpp_only:
            import shutil
            try:
                if shutil.which("black") is None:
                    raise ControllerError(
                        message="black formatter not found. Please install black to format Python files.",
                        command="format",
                        context={"tool": "black"},
                        exit_code=2,
                    )
            except (OSError, AttributeError) as e:
                raise ControllerError(
                    message=f"Failed to check for black formatter: {e}. Please ensure black is installed.",
                    command="format",
                    context={"tool": "black", "error": str(e)},
                    exit_code=2,
                )

        # Validate that cpp_only and python_only are not both specified
        if self.cpp_only and self.python_only:
            raise ControllerError(
                message="Cannot specify both --cpp-only and --python-only",
                command="format",
                context={
                    "cpp_only": self.cpp_only,
                    "python_only": self.python_only,
                },
                exit_code=2,
            )

        # Validate files if specified
        for file_path in self.files:
            path = Path(file_path)
            self.validate_file_path(path, must_exist=True)

        # Validate directories if specified
        for directory in self.directories:
            dir_path = Path(directory)
            self.validate_directory(dir_path, must_exist=True)

    def get_cpp_files(self) -> List[Path]:
        """Get list of C++ files to format.

        Returns:
            List of C++ file paths.
        """
        cpp_extensions = [".cpp", ".hpp", ".h", ".cc", ".cxx", ".hxx", ".c"]
        cpp_files = []

        # Add specified files
        for file_path in self.files:
            path = Path(file_path)
            if path.suffix in cpp_extensions:
                cpp_files.append(path)

        # Add files from specified directories
        for directory in self.directories:
            dir_path = Path(directory)
            for ext in cpp_extensions:
                cpp_files.extend(dir_path.rglob(f"*{ext}"))

        # If no files or directories specified, scan current directory
        if not self.files and not self.directories:
            project_root = self.get_project_root()
            for ext in cpp_extensions:
                cpp_files.extend(project_root.rglob(f"*{ext}"))

        # Filter out files in build directories
        cpp_files = [
            f for f in cpp_files
            if "build" not in str(f) and "packages" not in str(f)
        ]

        return cpp_files

    def get_python_files(self) -> List[Path]:
        """Get list of Python files to format.

        Returns:
            List of Python file paths.
        """
        python_extensions = [".py"]
        python_files = []

        # Add specified files
        for file_path in self.files:
            path = Path(file_path)
            if path.suffix in python_extensions:
                python_files.append(path)

        # Add files from specified directories
        for directory in self.directories:
            dir_path = Path(directory)
            for ext in python_extensions:
                python_files.extend(dir_path.rglob(f"*{ext}"))

        # If no files or directories specified, scan current directory
        if not self.files and not self.directories:
            project_root = self.get_project_root()
            for ext in python_extensions:
                python_files.extend(project_root.rglob(f"*{ext}"))

        # Filter out files in build directories
        python_files = [
            f for f in python_files
            if "build" not in str(f) and "packages" not in str(f)
        ]

        return python_files

    def format_cpp_file(self, file_path: Path) -> int:
        """Format a single C++ file with clang-format.

        Args:
            file_path: Path to C++ file to format.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.debug(f"Formatting C++ file: {file_path}")

        # Check if clang-format exists before execution
        import shutil
        if shutil.which("clang-format") is None:
            self.logger.warning("clang-format not found, skipping C++ formatting")
            return 0

        try:
            # Build clang-format command
            cmd = ["clang-format"]

            # Add check mode flag
            if self.check:
                cmd.append("--dry-run")
                cmd.append("--Werror")

            # Add dry-run flag
            if self.dry_run:
                cmd.append("--dry-run")

            # Add file
            cmd.append(str(file_path))

            # Run command
            result = subprocess.run(cmd, capture_output=True, text=True)

            if result.returncode == 0:
                if not self.check and not self.dry_run:
                    # Write formatted content back to file
                    with open(file_path, "w", encoding="utf-8") as f:
                        f.write(result.stdout)
                    self.logger.debug(f"Formatted C++ file: {file_path}")
                else:
                    self.logger.debug(f"Checked C++ file: {file_path}")
            else:
                self.logger.warning(f"clang-format failed for {file_path}: {result.stderr}")

            return result.returncode
        except FileNotFoundError:
            self.logger.warning("clang-format not found, skipping C++ formatting")
            return 0
        except Exception as e:
            self.logger.error(f"Failed to format C++ file {file_path}: {e}")
            return 1

    def format_python_file(self, file_path: Path) -> int:
        """Format a single Python file with black.

        Args:
            file_path: Path to Python file to format.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.debug(f"Formatting Python file: {file_path}")

        # Check if black exists before execution
        import shutil
        if shutil.which("black") is None:
            self.logger.warning("black not found, skipping Python formatting")
            return 0

        try:
            # Build black command
            cmd = ["black"]

            # Add check mode flag
            if self.check:
                cmd.append("--check")

            # Add dry-run flag
            if self.dry_run:
                cmd.append("--diff")

            # Add file
            cmd.append(str(file_path))

            # Run command
            result = subprocess.run(cmd, capture_output=True, text=True)

            if result.returncode == 0:
                self.logger.debug(f"Formatted Python file: {file_path}")
            else:
                self.logger.warning(f"black failed for {file_path}: {result.stderr}")

            return result.returncode
        except FileNotFoundError:
            self.logger.warning("black not found, skipping Python formatting")
            return 0
        except Exception as e:
            self.logger.error(f"Failed to format Python file {file_path}: {e}")
            return 1

    def format_cpp_files(self) -> int:
        """Format all C++ files.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        if self.python_only:
            self.logger.info("Skipping C++ formatting (--python-only specified)")
            return 0

        cpp_files = self.get_cpp_files()

        if not cpp_files:
            self.logger.info("No C++ files to format")
            return 0

        self.logger.info(f"Formatting {len(cpp_files)} C++ files")

        # Format each file
        failed_files = []
        for file_path in cpp_files:
            result = self.format_cpp_file(file_path)
            if result != 0:
                failed_files.append(file_path)

        if failed_files:
            self.logger.error(f"Failed to format {len(failed_files)} C++ files")
            for file_path in failed_files:
                self.logger.error(f"  - {file_path}")
            return 1

        self.logger.info(f"Successfully formatted {len(cpp_files)} C++ files")
        return 0

    def format_python_files(self) -> int:
        """Format all Python files.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        if self.cpp_only:
            self.logger.info("Skipping Python formatting (--cpp-only specified)")
            return 0

        python_files = self.get_python_files()

        if not python_files:
            self.logger.info("No Python files to format")
            return 0

        self.logger.info(f"Formatting {len(python_files)} Python files")

        # Format each file
        failed_files = []
        for file_path in python_files:
            result = self.format_python_file(file_path)
            if result != 0:
                failed_files.append(file_path)

        if failed_files:
            self.logger.error(f"Failed to format {len(failed_files)} Python files")
            for file_path in failed_files:
                self.logger.error(f"  - {file_path}")
            return 1

        self.logger.info(f"Successfully formatted {len(python_files)} Python files")
        return 0

    def execute(self) -> int:
        """Execute format command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("format")

        try:
            # Validate arguments
            self.validate_arguments()

            # Format C++ files
            result = self.format_cpp_files()
            if result != 0:
                return result

            # Format Python files
            result = self.format_python_files()
            if result != 0:
                return result

            self.log_command_success("format")
            return 0

        except ControllerError as e:
            self.log_command_error("format", e)
            return e.exit_code
        except Exception as e:
            self.logger.exception(f"Unexpected error during format: {e}")
            return 1


def format_code(args: argparse.Namespace) -> int:
    """Format code with clang-format and black.

    This function provides a command-line interface for format command.

    Args:
        args: Parsed command-line arguments containing:
            - files: List of specific files to format (optional)
            - directories: List of directories to scan (optional)
            - check: Only check formatting without modifying (optional)
            - dry_run: Run in dry-run mode (optional)
            - cpp_only: Only format C++ files (optional)
            - python_only: Only format Python files (optional)

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    controller = FormatController(args)
    return controller.execute()


__all__ = [
    "FormatController",
    "format_code",
]
