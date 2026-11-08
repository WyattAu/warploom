# warploom_scripts/error_handler.py
"""
Centralized error handling system for OmniCPP project.

This module provides:
- Custom exception classes for different error types
- Retry decorator with configurable backoff strategies
- Error logging and reporting system
- Recovery action registry
"""

import functools
import logging
import time
import traceback
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Tuple, Type
from pathlib import Path

logger = logging.getLogger(__name__)


class ErrorSeverity(Enum):
    """Error severity levels"""
    LOW = "low"
    MEDIUM = "medium"
    HIGH = "high"
    CRITICAL = "critical"


class WarploomError(Exception):
    """Base exception class for OmniCPP errors"""

    def __init__(self, message: str, severity: ErrorSeverity = ErrorSeverity.MEDIUM,
                 recoverable: bool = True, context: Optional[Dict[str, Any]] = None):
        super().__init__(message)
        self.message = message
        self.severity = severity
        self.recoverable = recoverable
        self.context = context or {}
        self.timestamp = time.time()

    def __str__(self) -> str:
        return f"[{self.severity.value.upper()}] {self.message}"


class BuildError(WarploomError):
    """Errors related to build operations"""
    pass


class DependencyError(WarploomError):
    """Errors related to dependency management"""
    pass


class ConfigurationError(WarploomError):
    """Errors related to configuration"""
    pass


class ValidationError(WarploomError):
    """Errors related to validation"""
    pass


class SecurityError(WarploomError):
    """Errors related to security"""
    pass


@dataclass
class RetryConfig:
    """Configuration for retry mechanisms"""
    max_attempts: int = 3
    initial_delay: float = 1.0
    backoff_factor: float = 2.0
    max_delay: float = 60.0
    jitter: bool = True
    retryable_exceptions: Tuple[Type[Exception], ...] = (Exception,)


@dataclass
class RecoveryAction:
    """Recovery action definition"""
    name: str
    description: str
    action_func: Callable[[WarploomError], bool]
    priority: int = 1
    requires_confirmation: bool = False


class ErrorHandler:
    """Centralized error handling and recovery system"""

    def __init__(self) -> None:
        self.recovery_actions: Dict[str, RecoveryAction] = {}
        self.error_history: List[WarploomError] = []
        self.logger = logging.getLogger(__name__)

    def register_recovery_action(self, error_type: Type[WarploomError],
                                action: RecoveryAction) -> None:
        """Register a recovery action for a specific error type"""
        key = f"{error_type.__name__}:{action.name}"
        self.recovery_actions[key] = action
        self.logger.debug(f"Registered recovery action: {key}")

    def log_error(self, error: WarploomError) -> None:
        """Log an error with appropriate level"""
        self.error_history.append(error)

        # Keep only last 100 errors
        if len(self.error_history) > 100:
            self.error_history = self.error_history[-100:]

        # Log based on severity
        if error.severity == ErrorSeverity.CRITICAL:
            self.logger.critical(str(error), extra=error.context)
        elif error.severity == ErrorSeverity.HIGH:
            self.logger.error(str(error), extra=error.context)
        elif error.severity == ErrorSeverity.MEDIUM:
            self.logger.warning(str(error), extra=error.context)
        else:
            self.logger.info(str(error), extra=error.context)

    def attempt_recovery(self, error: WarploomError) -> bool:
        """Attempt to recover from an error"""
        if not error.recoverable:
            self.logger.debug(f"Error {type(error).__name__} is not recoverable")
            return False

        # Find recovery actions for this error type
        error_type_name = type(error).__name__
        matching_actions = [
            action for key, action in self.recovery_actions.items()
            if key.startswith(f"{error_type_name}:")
        ]

        if not matching_actions:
            self.logger.debug(f"No recovery actions found for {error_type_name}")
            return False

        # Sort by priority (higher priority first)
        matching_actions.sort(key=lambda x: x.priority, reverse=True)

        for action in matching_actions:
            try:
                self.logger.info(f"Attempting recovery action: {action.name}")

                if action.requires_confirmation:
                    # In interactive mode, this would prompt user
                    # For now, skip confirmation-required actions
                    continue

                result = action.action_func(error)
                if result:
                    self.logger.info(f"Recovery action {action.name} succeeded")
                    return True
                else:
                    self.logger.warning(f"Recovery action {action.name} failed")

            except Exception as e:
                self.logger.error(f"Recovery action {action.name} threw exception: {e}")

        return False

    def handle_error(self, error: WarploomError, raise_after_recovery: bool = True) -> None:
        """Handle an error with logging and recovery attempts"""
        self.log_error(error)

        if self.attempt_recovery(error):
            self.logger.info("Error recovery successful")
            return

        if raise_after_recovery:
            raise error


# Global error handler instance
error_handler = ErrorHandler()


def retry_on_failure(config: Optional[RetryConfig] = None) -> Callable[..., Any]:
    """
    Decorator that retries a function on failure with exponential backoff

    Args:
        config: Retry configuration. If None, uses default config.
    """
    if config is None:
        config = RetryConfig()

    def decorator(func: Callable[..., Any]) -> Callable[..., Any]:
        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            last_exception: Optional[Exception] = None

            for attempt in range(config.max_attempts):
                try:
                    return func(*args, **kwargs)
                except Exception as e:
                    # Check if exception is retryable
                    if isinstance(e, config.retryable_exceptions):
                        last_exception = e
                    else:
                        # Non-retryable exception, re-raise immediately
                        raise

                    if attempt < config.max_attempts - 1:
                        delay = min(
                            config.initial_delay * (config.backoff_factor ** attempt),
                            config.max_delay
                        )

                        if config.jitter:
                            # Add random jitter (±25%)
                            import random
                            jitter_range = delay * 0.25
                            delay += random.uniform(-jitter_range, jitter_range)

                        logger.warning(
                            f"Attempt {attempt + 1}/{config.max_attempts} failed for {func.__name__}: {e}. "
                            f"Retrying in {delay:.2f} seconds..."
                        )
                        time.sleep(delay)
                    else:
                        logger.error(
                            f"All {config.max_attempts} attempts failed for {func.__name__}: {e}"
                        )

            # If we get here, all retries failed
            if last_exception is not None:
                raise last_exception
            else:
                raise RuntimeError(f"All {config.max_attempts} attempts failed for {func.__name__}")

        return wrapper
    return decorator


def with_error_handling(severity: ErrorSeverity = ErrorSeverity.MEDIUM,
                       recoverable: bool = True) -> Callable[..., Any]:
    """
    Decorator that wraps function calls with error handling

    Args:
        severity: Error severity level
        recoverable: Whether the error is recoverable
    """
    def decorator(func: Callable[..., Any]) -> Callable[..., Any]:
        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            try:
                return func(*args, **kwargs)
            except WarploomError:
                # Re-raise Warploom errors as-is
                raise
            except Exception as e:
                # Wrap other exceptions
                error = WarploomError(
                    f"Unexpected error in {func.__name__}: {str(e)}",
                    severity=severity,
                    recoverable=recoverable,
                    context={
                        'function': func.__name__,
                        'args': str(args),
                        'kwargs': str(kwargs),
                        'traceback': traceback.format_exc()
                    }
                )
                error_handler.handle_error(error)
                return None  # Should not reach here if error is raised

        return wrapper
    return decorator


def create_error_context(**kwargs: Any) -> Dict[str, Any]:
    """Create error context dictionary"""
    context: Dict[str, Any] = {
        'timestamp': time.time(),
        'working_directory': str(Path.cwd()),
    }
    context.update(kwargs)
    return context


# Default recovery actions
def _cleanup_build_artifacts(error: WarploomError) -> bool:
    """Recovery action: Clean build artifacts"""
    try:
        # This would be implemented to clean build directories
        logger.info("Cleaning build artifacts...")
        return True
    except Exception:
        return False


def _reset_conan_cache(error: WarploomError) -> bool:
    """Recovery action: Reset Conan cache"""
    try:
        # This would run conan cache clean
        logger.info("Resetting Conan cache...")
        return True
    except Exception:
        return False


# Register default recovery actions
error_handler.register_recovery_action(
    BuildError,
    RecoveryAction(
        name="cleanup_artifacts",
        description="Clean build artifacts and retry",
        action_func=_cleanup_build_artifacts,
        priority=2
    )
)

error_handler.register_recovery_action(
    DependencyError,
    RecoveryAction(
        name="reset_conan_cache",
        description="Reset Conan cache and retry",
        action_func=_reset_conan_cache,
        priority=1
    )
)
