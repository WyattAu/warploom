# warploom_scripts/utils/logging_utils.py
"""
Logging utility functions for Warploom project.

Provides logging functions with timestamps.
"""

from __future__ import annotations

import sys
from datetime import datetime


def log_info(message: str) -> None:
    """Log an informational message with timestamp.

    Args:
        message: The message to log.
    """
    print(f"[INFO] {datetime.now().isoformat()} - {message}")


def log_warning(message: str) -> None:
    """Log a warning message with timestamp.

    Args:
        message: The warning message to log.
    """
    print(f"[WARNING] {datetime.now().isoformat()} - {message}")


def log_error(message: str) -> None:
    """Log an error message with timestamp to stderr.

    Args:
        message: The error message to log.
    """
    print(f"[ERROR] {datetime.now().isoformat()} - {message}", file=sys.stderr)


def log_success(message: str) -> None:
    """Log a success message with timestamp.

    Args:
        message: The success message to log.
    """
    print(f"[SUCCESS] {datetime.now().isoformat()} - {message}")


__all__ = [
    'log_info',
    'log_warning',
    'log_error',
    'log_success',
]
