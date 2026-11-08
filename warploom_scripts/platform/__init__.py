"""
Platform detection module for OmniCPP project.

Provides platform detection capabilities including OS, architecture,
and platform-specific information.
"""

from .detector import (
    PlatformInfo,
    detect_architecture,
    detect_platform,
    get_platform_info,
)

__all__ = [
    'PlatformInfo',
    'detect_platform',
    'detect_architecture',
    'get_platform_info',
]
