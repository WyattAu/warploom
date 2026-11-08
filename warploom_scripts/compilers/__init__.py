"""
Compiler detection module for OmniCPP project.

Provides compiler detection capabilities including MSVC, GCC, Clang,
and C++23 support validation.
"""

from .detector import (
    CompilerInfo,
    ValidationResult,
    detect_all_compilers,
    detect_compiler,
    validate_cpp23_support,
)

__all__ = [
    'CompilerInfo',
    'ValidationResult',
    'detect_compiler',
    'validate_cpp23_support',
    'detect_all_compilers',
]
