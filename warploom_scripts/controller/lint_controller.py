"""
Lint controller for WarploomController.

This module handles lint command, which performs static analysis
with clang-tidy for C++ and pylint for Python.
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
from typing import List

from warploom_scripts.controller.base import BaseController
from warploom_scripts.exceptions import ControllerError


class LintController(BaseController):
    """Controller for lint command.

    Handles static analysis, including:
    - C++ code linting with clang-tidy
    - Python code linting with pylint
    - Fix mode (apply automatic fixes)
    - File and directory selection
    """

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize lint controller.

        Args:
            args: Parsed command-line arguments.
        """
        super().__init__(args)
        self.files = getattr(args, "files", [])
        self.directories = getattr(args, "directories", [])
        self.fix = getattr(args, "fix", False)
        self.cpp_only = getattr(args, "cpp_only", False)
        self.python_only = getattr(args, "python_only", False)

    def validate_arguments(self) -> None:
        """Validate lint command arguments.

        Raises:
            ControllerError: If arguments are invalid.
        """
        # Validate that pylint is available if Python linting is requested
        if not self.cpp_only:
            import shutil
            try:
                if shutil.which("pylint") is None:
                    raise ControllerError(
                        message="pylint linter not found. Please install pylint to lint Python files.",
                        command="lint",
                        context={"tool": "pylint"},
                        exit_code=2,
                    )
            except (OSError, AttributeError) as e:
                raise ControllerError(
                    message=f"Failed to check for pylint linter: {e}. Please ensure pylint is installed.",
                    command="lint",
                    context={"tool": "pylint", "error": str(e)},
                    exit_code=2,
                )

        # Validate that cpp_only and python_only are not both specified
        if self.cpp_only and self.python_only:
            raise ControllerError(
                message="Cannot specify both --cpp-only and --python-only",
                command="lint",
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
        """Get list of C++ files to lint.

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
        """Get list of Python files to lint.

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

    def lint_cpp_file(self, file_path: Path) -> int:
        """Lint a single C++ file with clang-tidy.

        Args:
            file_path: Path to C++ file to lint.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.debug(f"Linting C++ file: {file_path}")

        # Check if clang-tidy exists before execution
        import shutil
        if shutil.which("clang-tidy") is None:
            self.logger.warning("clang-tidy not found, skipping C++ linting")
            return 0

        try:
            # Build clang-tidy command
            cmd = ["clang-tidy"]

            # Add fix mode flag
            if self.fix:
                cmd.append("--fix")
                cmd.append("--fix-errors")

            # Add file
            cmd.append(str(file_path))

            # Add compilation database if available
            compile_commands = self.get_project_root() / "build" / "compile_commands.json"
            if compile_commands.exists():
                cmd.extend(["-p", str(compile_commands.parent)])

            # Run command
            result = subprocess.run(cmd, capture_output=True, text=True)

            if result.returncode == 0:
                self.logger.debug(f"No issues found in {file_path}")
            else:
                self.logger.warning(f"clang-tidy found issues in {file_path}")
                if result.stdout:
                    self.logger.debug(f"clang-tidy output:\n{result.stdout}")

            return result.returncode
        except FileNotFoundError:
            self.logger.warning("clang-tidy not found, skipping C++ linting")
            return 0
        except Exception as e:
            self.logger.error(f"Failed to lint C++ file {file_path}: {e}")
            return 1

    def lint_python_file(self, file_path: Path) -> int:
        """Lint a single Python file with pylint.

        Args:
            file_path: Path to Python file to lint.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.logger.debug(f"Linting Python file: {file_path}")

        # Check if pylint exists before execution
        import shutil
        if shutil.which("pylint") is None:
            self.logger.warning("pylint not found, skipping Python linting")
            return 0

        try:
            # Build pylint command
            cmd = ["pylint"]

            # Add fix mode flag
            if self.fix:
                cmd.append("--fix")

            # Add output format
            cmd.extend(["--output-format", "text"])

            # Add file
            cmd.append(str(file_path))

            # Run command
            result = subprocess.run(cmd, capture_output=True, text=True)

            if result.returncode == 0:
                self.logger.debug(f"No issues found in {file_path}")
            else:
                self.logger.warning(f"pylint found issues in {file_path}")
                if result.stdout:
                    self.logger.debug(f"pylint output:\n{result.stdout}")

            return result.returncode
        except FileNotFoundError:
            self.logger.warning("pylint not found, skipping Python linting")
            return 0
        except Exception as e:
            self.logger.error(f"Failed to lint Python file {file_path}: {e}")
            return 1

    def lint_cpp_files(self) -> int:
        """Lint all C++ files.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        if self.python_only:
            self.logger.info("Skipping C++ linting (--python-only specified)")
            return 0

        cpp_files = self.get_cpp_files()

        if not cpp_files:
            self.logger.info("No C++ files to lint")
            return 0

        self.logger.info(f"Linting {len(cpp_files)} C++ files")

        # Lint each file
        failed_files = []
        for file_path in cpp_files:
            result = self.lint_cpp_file(file_path)
            if result != 0:
                failed_files.append(file_path)

        if failed_files:
            self.logger.error(f"Failed to lint {len(failed_files)} C++ files")
            for file_path in failed_files:
                self.logger.error(f"  - {file_path}")
            return 1

        self.logger.info(f"Successfully linted {len(cpp_files)} C++ files")
        return 0

    def lint_python_files(self) -> int:
        """Lint all Python files.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        if self.cpp_only:
            self.logger.info("Skipping Python linting (--cpp-only specified)")
            return 0

        python_files = self.get_python_files()

        if not python_files:
            self.logger.info("No Python files to lint")
            return 0

        self.logger.info(f"Linting {len(python_files)} Python files")

        # Lint each file
        failed_files = []
        for file_path in python_files:
            result = self.lint_python_file(file_path)
            if result != 0:
                failed_files.append(file_path)

        if failed_files:
            self.logger.error(f"Failed to lint {len(failed_files)} Python files")
            for file_path in failed_files:
                self.logger.error(f"  - {file_path}")
            return 1

        self.logger.info(f"Successfully linted {len(python_files)} Python files")
        return 0

    def execute(self) -> int:
        """Execute lint command.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        self.log_command_start("lint")

        try:
            # Validate arguments
            self.validate_arguments()

            # Lint C++ files
            result = self.lint_cpp_files()
            if result != 0:
                return result

            # Lint Python files
            result = self.lint_python_files()
            if result != 0:
                return result

            self.log_command_success("lint")
            return 0

        except ControllerError as e:
            self.log_command_error("lint", e)
            return e.exit_code
        except Exception as e:
            self.logger.exception(f"Unexpected error during lint: {e}")
            return 1


def lint(args: argparse.Namespace) -> int:
    """Lint code with clang-tidy and pylint.

    This function provides a command-line interface for lint command.

    Args:
        args: Parsed command-line arguments containing:
            - files: List of specific files to lint (optional)
            - directories: List of directories to scan (optional)
            - fix: Apply automatic fixes (optional)
            - cpp_only: Only lint C++ files (optional)
            - python_only: Only lint Python files (optional)

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    controller = LintController(args)
    return controller.execute()


__all__ = [
    "LintController",
    "lint",
]
