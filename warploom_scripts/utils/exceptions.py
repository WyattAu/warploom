# warploom_scripts/utils/exceptions.py
"""
Custom exception classes for Warploom project.

Provides exception classes for error handling across all modules.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional


class NotADirectoryError(Exception):
    """Raised when a path exists but is not a directory."""

    def __init__(self, message: str, path: Optional[Path] = None) -> None:
        """Initialize exception.

        Args:
            message: Error message describing issue.
            path: Optional path that caused error.
        """
        self.path = path
        super().__init__(message)


class CommandExecutionError(Exception):
    """Raised when a command execution fails."""

    def __init__(
        self,
        message: str,
        command: str,
        return_code: Optional[int] = None,
        output: Optional[str] = None,
    ) -> None:
        """Initialize exception.

        Args:
            message: Error message describing the issue.
            command: The command that failed.
            return_code: The return code from the command.
            output: The output from the command.
        """
        self.command = command
        self.return_code = return_code
        self.output = output
        super().__init__(message)


class PathValidationError(Exception):
    """Raised when path validation fails."""

    def __init__(
        self,
        message: str,
        path: Path,
        must_exist: bool,
    ) -> None:
        """Initialize exception.

        Args:
            message: Error message describing the issue.
            path: The path that failed validation.
            must_exist: Whether the path was required to exist.
        """
        self.path = path
        self.must_exist = must_exist
        super().__init__(message)


class ConfigurationError(Exception):
    """Raised when configuration operations fail."""

    def __init__(
        self,
        message: str,
        context: Optional[dict[str, Any]] = None,
        exit_code: int = 1
    ) -> None:
        """Initialize configuration error.

        Args:
            message: Error message describing issue.
            context: Optional context dictionary with additional information.
            exit_code: Exit code for the error.
        """
        self.message = message
        self.context = context or {}
        self.exit_code = exit_code
        super().__init__(self.message)


__all__ = [
    'NotADirectoryError',
    'CommandExecutionError',
    'PathValidationError',
    'ConfigurationError',
]
