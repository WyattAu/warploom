"""
Base controller class for WarploomController.

This module provides the base controller class with common functionality
for all command handlers, including logging integration, error handling,
and utility methods.
"""

from __future__ import annotations

import argparse
import logging
from pathlib import Path
from typing import Optional

from warploom_scripts.exceptions import (
    ControllerError,
    InvalidTargetError,
)
from warploom_scripts.logging.logger import get_logger, setup_logging


class BaseController:
    """Base controller class with common functionality for all command handlers.

    This class provides shared functionality for command handlers including:
    - Logging integration
    - Error handling and validation
    - Common utility methods
    - Configuration management

    Attributes:
        logger: Logger instance for the controller.
        args: Parsed command-line arguments.
    """

    # Valid targets for build operations
    VALID_TARGETS = ["engine", "game", "standalone", "all"]

    # Valid configurations
    VALID_CONFIGS = ["debug", "release"]

    # Valid compilers
    VALID_COMPILERS = [
        "msvc",
        "clang-msvc",
        "mingw-clang",
        "mingw-gcc",
        "gcc",
        "clang",
    ]

    # Valid build types for CMake
    VALID_BUILD_TYPES = [
        "Debug",
        "Release",
        "RelWithDebInfo",
        "MinSizeRel",
    ]

    def __init__(self, args: argparse.Namespace) -> None:
        """Initialize the base controller.

        Args:
            args: Parsed command-line arguments.
        """
        self.args = args
        self.logger = get_logger(self.__class__.__name__)

    def validate_target(self, target: str) -> None:
        """Validate that a target is valid.

        Args:
            target: The target to validate.

        Raises:
            InvalidTargetError: If the target is not valid.
        """
        if target not in self.VALID_TARGETS:
            raise InvalidTargetError(
                message=f"Invalid target '{target}'. Valid targets are: {', '.join(self.VALID_TARGETS)}",
                command=getattr(self.args, "command", "unknown"),
                target=target,
                valid_targets=self.VALID_TARGETS,
            )

    def validate_config(self, config: str) -> None:
        """Validate that a configuration is valid.

        Args:
            config: The configuration to validate.

        Raises:
            ControllerError: If the configuration is not valid.
        """
        if config.lower() not in self.VALID_CONFIGS:
            raise ControllerError(
                message=f"Invalid configuration '{config}'. Valid configurations are: {', '.join(self.VALID_CONFIGS)}",
                command=getattr(self.args, "command", "unknown"),
                context={"config": config, "valid_configs": self.VALID_CONFIGS},
                exit_code=2,
            )

    def validate_compiler(self, compiler: Optional[str]) -> None:
        """Validate that a compiler is valid.

        Args:
            compiler: The compiler to validate. If None, validation is skipped.

        Raises:
            ControllerError: If the compiler is not valid.
        """
        if compiler is not None and compiler not in self.VALID_COMPILERS:
            raise ControllerError(
                message=f"Invalid compiler '{compiler}'. Valid compilers are: {', '.join(self.VALID_COMPILERS)}",
                command=getattr(self.args, "command", "unknown"),
                context={"compiler": compiler, "valid_compilers": self.VALID_COMPILERS},
                exit_code=2,
            )

    def validate_build_type(self, build_type: str) -> None:
        """Validate that a build type is valid.

        Args:
            build_type: The build type to validate.

        Raises:
            ControllerError: If the build type is not valid.
        """
        if build_type not in self.VALID_BUILD_TYPES:
            raise ControllerError(
                message=f"Invalid build type '{build_type}'. Valid build types are: {', '.join(self.VALID_BUILD_TYPES)}",
                command=getattr(self.args, "command", "unknown"),
                context={"build_type": build_type, "valid_build_types": self.VALID_BUILD_TYPES},
                exit_code=2,
            )

    def validate_file_path(self, file_path: Path, must_exist: bool = True) -> None:
        """Validate that a file path is valid.

        Args:
            file_path: The file path to validate.
            must_exist: If True, the file must exist.

        Raises:
            ControllerError: If the file path is invalid.
        """
        if must_exist and not file_path.exists():
            raise ControllerError(
                message=f"File not found: {file_path}",
                command=getattr(self.args, "command", "unknown"),
                context={"file_path": str(file_path)},
                exit_code=3,
            )

        # Check for directory traversal attempts
        try:
            file_path.resolve()
        except Exception as e:
            raise ControllerError(
                message=f"Invalid file path: {file_path}",
                command=getattr(self.args, "command", "unknown"),
                context={"file_path": str(file_path), "error": str(e)},
                exit_code=2,
            )

    def validate_directory(self, directory: Path, must_exist: bool = True) -> None:
        """Validate that a directory is valid.

        Args:
            directory: The directory to validate.
            must_exist: If True, the directory must exist.

        Raises:
            ControllerError: If the directory is invalid.
        """
        if must_exist and not directory.exists():
            raise ControllerError(
                message=f"Directory not found: {directory}",
                command=getattr(self.args, "command", "unknown"),
                context={"directory": str(directory)},
                exit_code=3,
            )

        if must_exist and not directory.is_dir():
            raise ControllerError(
                message=f"Path is not a directory: {directory}",
                command=getattr(self.args, "command", "unknown"),
                context={"directory": str(directory)},
                exit_code=2,
            )

    def log_command_start(self, command: str) -> None:
        """Log the start of a command execution.

        Args:
            command: The command being executed.
        """
        self.logger.info(f"Starting command: {command}")

    def log_command_success(self, command: str) -> None:
        """Log the successful completion of a command.

        Args:
            command: The command that completed successfully.
        """
        self.logger.info(f"Command completed successfully: {command}")

    def log_command_error(self, command: str, error: Exception) -> None:
        """Log an error during command execution.

        Args:
            command: The command that failed.
            error: The exception that occurred.
        """
        if isinstance(error, ControllerError):
            self.logger.error(
                f"Command failed: {command} - {error.message}",
                extra={"context": error.context},
            )
        else:
            self.logger.error(f"Command failed: {command} - {str(error)}")

    def handle_error(self, error: Exception) -> int:
        """Handle an error during command execution.

        Args:
            error: The exception that occurred.

        Returns:
            The appropriate exit code.
        """
        if isinstance(error, ControllerError):
            self.log_command_error(getattr(self.args, "command", "unknown"), error)
            return error.exit_code
        else:
            self.logger.exception(f"Unexpected error: {str(error)}")
            return 1

    def get_project_root(self) -> Path:
        """Get the project root directory.

        Returns:
            The project root directory as a Path object.
        """
        return Path(__file__).parent.parent.parent

    def get_config_path(self, config_name: str) -> Path:
        """Get the path to a configuration file.

        Args:
            config_name: The name of the configuration file.

        Returns:
            The path to the configuration file.
        """
        return self.get_project_root() / "config" / config_name

    def get_logs_directory(self) -> Path:
        """Get the logs directory.

        Returns:
            The logs directory as a Path object.
        """
        logs_dir = self.get_project_root() / "logs"
        logs_dir.mkdir(parents=True, exist_ok=True)
        return logs_dir

    def execute(self) -> int:
        """Execute the controller command.

        This method should be overridden by subclasses to implement
        specific command logic.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        raise NotImplementedError("Subclasses must implement execute()")


def setup_controller_logging(config_path: Optional[str] = None) -> logging.Logger:
    """Set up logging for the controller.

    Args:
        config_path: Optional path to the logging configuration file.

    Returns:
        The configured logger instance.
    """
    return setup_logging(config_path=config_path)


def create_base_parser() -> argparse.ArgumentParser:
    """Create the base argument parser for the controller.

    Returns:
        The configured argument parser.
    """
    parser = argparse.ArgumentParser(
        prog="WarploomController",
        description="Warploom build system controller",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Configure the build system
  python WarploomController.py configure --preset default

  # Build the engine
  python WarploomController.py build engine default default debug

  # Clean all targets
  python WarploomController.py clean

  # Run tests
  python WarploomController.py test engine debug

  # Format code
  python WarploomController.py format --check

  # Lint code
  python WarploomController.py lint --fix
        """,
    )

    parser.add_argument(
        "--version",
        action="version",
        version="%(prog)s 0.1.1",
        help="Show version information and exit",
    )

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable verbose output",
    )

    return parser


__all__ = [
    "BaseController",
    "setup_controller_logging",
    "create_base_parser",
]
