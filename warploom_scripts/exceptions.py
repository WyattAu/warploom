# warploom_scripts/exceptions.py
"""
Custom exception hierarchy for the Warploom controller.

Provides a structured exception system for controller-related errors,
including proper context tracking and exit code mapping.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List


class ControllerError(Exception):
    """Base exception for controller-related errors.

    All controller exceptions inherit from this class, providing
    consistent error handling and context tracking across the
    controller subsystem.

    Attributes:
        message: Human-readable error message.
        command: The command that triggered the error.
        context: Additional context information as a dictionary.
        exit_code: The exit code to return when this error occurs.
    """

    def __init__(
        self,
        message: str,
        command: str,
        context: Dict[str, Any] | None = None,
        exit_code: int = 1,
    ) -> None:
        """Initialize the controller error.

        Args:
            message: Human-readable error message describing the issue.
            command: The command that triggered the error.
            context: Optional dictionary with additional context information.
            exit_code: The exit code to return (default: 1 for general error).
        """
        self.message = message
        self.command = command
        self.context = context or {}
        self.exit_code = exit_code
        super().__init__(self.message)

    def __str__(self) -> str:
        """Return string representation of the error."""
        return self.message

    def to_dict(self) -> Dict[str, Any]:
        """Convert exception to dictionary for logging/serialization.

        Returns:
            Dictionary containing all exception attributes.
        """
        return {
            "error_type": self.__class__.__name__,
            "message": self.message,
            "command": self.command,
            "context": self.context,
            "exit_code": self.exit_code,
        }


class InvalidTargetError(ControllerError):
    """Raised when an invalid target is specified.

    This exception is used when a user provides a target that is not
    recognized or supported by the build system.

    Attributes:
        target: The invalid target that was specified.
        valid_targets: List of valid target options.
    """

    def __init__(
        self,
        message: str,
        command: str,
        target: str,
        valid_targets: List[str],
        context: Dict[str, Any] | None = None,
    ) -> None:
        """Initialize the invalid target error.

        Args:
            message: Human-readable error message.
            command: The command that triggered the error.
            target: The invalid target that was specified.
            valid_targets: List of valid target options.
            context: Optional additional context information.
        """
        self.target = target
        self.valid_targets = valid_targets

        # Build context with target information
        error_context = {
            "target": target,
            "valid_targets": valid_targets,
        }
        if context:
            error_context.update(context)

        super().__init__(
            message=message,
            command=command,
            context=error_context,
            exit_code=2,  # Invalid arguments
        )


class InvalidPipelineError(ControllerError):
    """Raised when an invalid pipeline is specified.

    This exception is used when a user provides a pipeline name that
    is not recognized or supported by the build system.

    Attributes:
        pipeline: The invalid pipeline that was specified.
        valid_pipelines: List of valid pipeline options.
    """

    def __init__(
        self,
        message: str,
        command: str,
        pipeline: str,
        valid_pipelines: List[str],
        context: Dict[str, Any] | None = None,
    ) -> None:
        """Initialize the invalid pipeline error.

        Args:
            message: Human-readable error message.
            command: The command that triggered the error.
            pipeline: The invalid pipeline that was specified.
            valid_pipelines: List of valid pipeline options.
            context: Optional additional context information.
        """
        self.pipeline = pipeline
        self.valid_pipelines = valid_pipelines

        # Build context with pipeline information
        error_context = {
            "pipeline": pipeline,
            "valid_pipelines": valid_pipelines,
        }
        if context:
            error_context.update(context)

        super().__init__(
            message=message,
            command=command,
            context=error_context,
            exit_code=2,  # Invalid arguments
        )


class ConfigurationError(ControllerError):
    """Raised when configuration is invalid.

    This exception is used when configuration files are missing,
    malformed, or contain invalid values that prevent the build
    system from functioning correctly.

    Attributes:
        config_file: Path to the configuration file that caused the error.
        validation_errors: List of validation error messages.
    """

    def __init__(
        self,
        message: str,
        command: str,
        config_file: Path,
        validation_errors: List[str],
        context: Dict[str, Any] | None = None,
    ) -> None:
        """Initialize the configuration error.

        Args:
            message: Human-readable error message.
            command: The command that triggered the error.
            config_file: Path to the configuration file that caused the error.
            validation_errors: List of validation error messages.
            context: Optional additional context information.
        """
        self.config_file = config_file
        self.validation_errors = validation_errors

        # Build context with configuration information
        error_context = {
            "config_file": str(config_file),
            "validation_errors": validation_errors,
        }
        if context:
            error_context.update(context)

        super().__init__(
            message=message,
            command=command,
            context=error_context,
            exit_code=3,  # Configuration error
        )


class ToolchainError(ControllerError):
    """Raised when toolchain is not available or invalid.

    This exception is used when a required compiler or toolchain
    cannot be found, is not installed, or does not meet the
    required version or capability requirements.

    Attributes:
        compiler: The compiler that caused the error.
        reason: The reason why the toolchain is unavailable.
        suggestions: List of suggested fixes or alternatives.
    """

    def __init__(
        self,
        message: str,
        command: str,
        compiler: str,
        reason: str,
        suggestions: List[str],
        context: Dict[str, Any] | None = None,
    ) -> None:
        """Initialize the toolchain error.

        Args:
            message: Human-readable error message.
            command: The command that triggered the error.
            compiler: The compiler that caused the error.
            reason: The reason why the toolchain is unavailable.
            suggestions: List of suggested fixes or alternatives.
            context: Optional additional context information.
        """
        self.compiler = compiler
        self.reason = reason
        self.suggestions = suggestions

        # Build context with toolchain information
        error_context = {
            "compiler": compiler,
            "reason": reason,
            "suggestions": suggestions,
        }
        if context:
            error_context.update(context)

        super().__init__(
            message=message,
            command=command,
            context=error_context,
            exit_code=4,  # Toolchain error
        )


__all__ = [
    "ControllerError",
    "InvalidTargetError",
    "InvalidPipelineError",
    "ConfigurationError",
    "ToolchainError",
]
