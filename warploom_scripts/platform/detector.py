# warploom_scripts/platform/detector.py
"""
Platform detection module for Warploom project.

Provides platform detection capabilities including OS, architecture,
and platform-specific information.
"""

from __future__ import annotations

import platform
from dataclasses import dataclass

from warploom_scripts.logging.logger import get_logger
from warploom_scripts.utils.platform_utils import is_linux, is_macos, is_windows


@dataclass
class PlatformInfo:
    """Information about the current platform.

    Attributes:
        os: Operating system name (Windows, Linux, macOS)
        architecture: System architecture (x86_64, ARM64, etc.)
        is_64bit: Whether the system is 64-bit
        platform_string: Full platform string from sys.platform
    """
    os: str
    architecture: str
    is_64bit: bool
    platform_string: str


def detect_platform() -> PlatformInfo:
    """Detect the current platform information.

    This function detects the operating system, architecture, and other
    platform-specific information. It uses platform-specific APIs to
    gather accurate information.

    Returns:
        PlatformInfo object containing detected platform information

    Raises:
        RuntimeError: If platform detection fails
    """
    logger = get_logger(__name__)

    try:
        # Detect operating system
        if is_windows():
            os_name = "Windows"
        elif is_linux():
            os_name = "Linux"
        elif is_macos():
            os_name = "macOS"
        else:
            os_name = "Unknown"
            logger.warning(f"Unknown operating system: {platform.system()}")

        # Detect architecture
        machine = platform.machine().lower()
        if machine in ("x86_64", "amd64", "x64"):
            arch = "x86_64"
            is_64 = True
        elif machine in ("arm64", "aarch64"):
            arch = "ARM64"
            is_64 = True
        elif machine in ("i386", "i686", "x86"):
            arch = "x86"
            is_64 = False
        else:
            arch = machine
            is_64 = platform.architecture()[0] == "64bit"
            logger.warning(f"Unknown architecture: {machine}")

        # Get platform string (use sys.platform for consistency with tests)
        import sys
        platform_str = sys.platform

        platform_info = PlatformInfo(
            os=os_name,
            architecture=arch,
            is_64bit=is_64,
            platform_string=platform_str
        )

        logger.info(
            f"Detected platform: {os_name} {arch} "
            f"({'64-bit' if is_64 else '32-bit'})"
        )

        return platform_info

    except Exception as e:
        logger.error(f"Platform detection failed: {e}")
        raise RuntimeError(f"Failed to detect platform: {e}") from e


def detect_architecture() -> str:
    """Detect the system architecture.

    This function detects the CPU architecture of the system.

    Returns:
        Architecture string (e.g., 'x86_64', 'ARM64')

    Raises:
        RuntimeError: If architecture detection fails
    """
    logger = get_logger(__name__)

    try:
        machine = platform.machine().lower()

        if machine in ("x86_64", "amd64", "x64"):
            arch = "x86_64"
        elif machine in ("arm64", "aarch64"):
            arch = "ARM64"
        elif machine in ("i386", "i686", "x86"):
            arch = "x86"
        else:
            arch = machine
            logger.warning(f"Unknown architecture: {machine}")

        logger.info(f"Detected architecture: {arch}")
        return arch

    except Exception as e:
        logger.error(f"Architecture detection failed: {e}")
        raise RuntimeError(f"Failed to detect architecture: {e}") from e


def get_platform_info() -> PlatformInfo:
    """Get platform information (alias for detect_platform).

    This function is an alias for detect_platform() for convenience.

    Returns:
        PlatformInfo object containing detected platform information
    """
    return detect_platform()


__all__ = [
    'PlatformInfo',
    'detect_platform',
    'detect_architecture',
    'get_platform_info',
]
