# warploom_scripts/utils/platform_utils.py
"""
Platform detection utility functions for OmniCPP project.

Provides system detection and platform-specific operations.
"""

from __future__ import annotations

import sys
from pathlib import Path


def get_workspace_dir() -> Path:
    """Get workspace directory.

    The workspace directory is the directory containing this module.

    Returns:
        The absolute path to workspace directory.
    """
    return Path(__file__).parent.resolve()


def get_system_platform() -> str:
    """Get current system platform.

    Returns:
        The platform name (win32, linux, darwin).
    """
    return sys.platform.lower()


def is_windows() -> bool:
    """Check if running on Windows.

    Returns:
        True if running on Windows, False otherwise.
    """
    return sys.platform == "win32"


def is_linux() -> bool:
    """Check if running on Linux.

    Returns:
        True if running on Linux, False otherwise.
    """
    return sys.platform.startswith("linux")


def is_macos() -> bool:
    """Check if running on macOS.

    Returns:
        True if running on macOS, False otherwise.
    """
    return sys.platform == "darwin"


def get_executable_extension() -> str:
    """Get appropriate executable extension for current platform.

    Returns:
        '.exe' on Windows, empty string on other platforms.
    """
    return ".exe" if is_windows() else ""


def get_library_extension() -> str:
    """Get appropriate library extension for current platform.

    Returns:
        '.dll' on Windows, '.so' on Linux, '.dylib' on macOS.
    """
    if is_windows():
        return ".dll"
    elif is_macos():
        return ".dylib"
    else:
        return ".so"


__all__ = [
    'get_workspace_dir',
    'get_system_platform',
    'is_windows',
    'is_linux',
    'is_macos',
    'get_executable_extension',
    'get_library_extension',
]
