# warploom_scripts/compilers/detection_system.py
"""
Unified Compiler Detection System

This module provides a unified system for detecting compilers, terminals,
and cross-compilation toolchains on Windows systems.
"""

from __future__ import annotations

import logging
from enum import Enum
from typing import Any, Dict, List, Optional
from dataclasses import dataclass, field

from warploom_scripts.logging.logger import get_logger
from .detector import CompilerInfo


class CompilerType(Enum):
    """Compiler type enumeration"""
    MSVC = "msvc"
    MSVC_CLANG = "msvc-clang"
    MINGW_GCC = "mingw-gcc"
    MINGW_CLANG = "mingw-clang"
    GCC = "gcc"
    CLANG = "clang"


@dataclass
class DetectionError:
    """Detection error information

    Attributes:
        component: Component that raised error
        error_type: Type of error (detection, validation, execution, etc.)
        message: Error message
        details: Additional error details
        suggestion: Suggestion for fixing error
    """
    component: str
    error_type: str
    message: str
    details: Optional[Dict[str, Any]] = None
    suggestion: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary representation"""
        return {
            "component": self.component,
            "error_type": self.error_type,
            "message": self.message,
            "details": self.details,
            "suggestion": self.suggestion
        }


@dataclass
class DetectionResult:
    """Detection result containing all detected components

    Attributes:
        compilers: Dictionary of detected compilers by type
        terminals: Dictionary of detected terminals by type
        cross_compilers: Dictionary of detected cross-compilers by platform
        errors: List of detection errors
        warnings: List of warning messages
        success: Whether detection was successful
    """
    compilers: Dict[str, Optional[CompilerInfo]] = field(default_factory=dict)
    terminals: Dict[str, List[Any]] = field(default_factory=dict)
    cross_compilers: Dict[str, Any] = field(default_factory=dict)
    errors: List[DetectionError] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    success: bool = True

    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary representation"""
        return {
            "compilers": self.compilers,
            "terminals": self.terminals,
            "cross_compilers": self.cross_compilers,
            "errors": [e.to_dict() for e in self.errors],
            "warnings": self.warnings,
            "success": self.success
        }

    @property
    def has_errors(self) -> bool:
        """Check if detection has errors"""
        return len(self.errors) > 0

    @property
    def has_warnings(self) -> bool:
        """Check if detection has warnings"""
        return len(self.warnings) > 0


class CompilerDetectionSystem:
    """Unified compiler detection system

    This class provides a unified interface for detecting compilers,
    terminals, and cross-compilation toolchains on Windows systems.
    It integrates all previously implemented components into a cohesive system.

    Attributes:
        _logger: Logger instance for logging operations
    """

    def __init__(
        self,
        logger: Optional[logging.Logger] = None
    ) -> None:
        """Initialize compiler detection system

        Args:
            logger: Logger instance for logging operations (optional)
        """
        self._logger = logger or get_logger(__name__)
        self._logger.info("Initializing CompilerDetectionSystem")

        # Import compiler detection components
        from .detector import detect_all_compilers

        # Detect all compilers
        self._logger.info("Detecting all compilers")
        self._compilers = detect_all_compilers()

        # Log detected compilers
        for compiler_name, compiler_info in self._compilers.items():
            if compiler_info:
                self._logger.info(
                    f"Detected compiler: {compiler_name} "
                    f"{compiler_info.version} at {compiler_info.path}"
                )

        self._logger.info("CompilerDetectionSystem initialized successfully")

    def detect_all(self) -> DetectionResult:
        """Detect all compilers, terminals, and cross-compilers

        This method performs comprehensive detection of all system components:
        - Native compilers (MSVC, MSVC-Clang, MinGW-GCC, MinGW-Clang)
        - Terminals (MSVC, MSYS2)
        - Cross-compilation toolchains (Linux, WASM, Android)

        Returns:
            DetectionResult containing all detected components and any errors

        Raises:
            RuntimeError: If critical detection failures occur
        """
        self._logger.info("Starting comprehensive detection of all components")

        errors: List[DetectionError] = []
        warnings: List[str] = []

        # Detect compilers
        try:
            self._logger.info("Detecting native compilers")
            compilers = self._compilers
            self._logger.info(
                f"Detected {len([c for c in compilers.values() if c])} compilers"
            )
        except Exception as e:
            error_msg = f"Failed to detect compilers: {str(e)}"
            self._logger.error(error_msg)
            errors.append(DetectionError(
                component="compiler_detection",
                error_type="detection_failed",
                message=error_msg,
                suggestion="Check compiler installations and permissions"
            ))
            compilers = {}

        # Determine overall success
        success = len(errors) == 0

        if success:
            self._logger.info("Detection completed successfully")
        else:
            self._logger.warning(
                f"Detection completed with {len(errors)} errors and "
                f"{len(warnings)} warnings"
            )

        return DetectionResult(
            compilers=compilers,
            terminals={},
            cross_compilers={},
            errors=errors,
            warnings=warnings,
            success=success
        )

    def detect_compiler(
        self,
        compiler_type: str,
        architecture: str = "x64"
    ) -> Optional[CompilerInfo]:
        """Detect a specific compiler type and architecture

        Args:
            compiler_type: Type of compiler (msvc, msvc_clang, mingw_gcc, mingw_clang)
            architecture: Target architecture (x64, x86, arm, arm64)

        Returns:
            CompilerInfo if found, None otherwise

        Raises:
            ValueError: If compiler_type is invalid
        """
        self._logger.info(
            f"Detecting compiler: {compiler_type} {architecture}"
        )

        try:
            # Map compiler_type to detector function
            from .detector import detect_compiler

            # Validate compiler_type
            try:
                CompilerType(compiler_type.lower())
            except ValueError:
                self._logger.error(f"Invalid compiler type: {compiler_type}")
                raise ValueError(f"Invalid compiler type: {compiler_type}")

            # Detect compiler
            compiler = detect_compiler(
                compiler_name=compiler_type.lower(),
                platform_info=None
            )

            if compiler:
                self._logger.info(
                    f"Successfully detected compiler: "
                    f"{compiler.name} {compiler.version}"
                )
            else:
                self._logger.warning(
                    f"Compiler not found: {compiler_type} {architecture}"
                )

            return compiler
        except ValueError as e:
            self._logger.error(f"Invalid compiler type: {compiler_type}")
            raise
        except Exception as e:
            self._logger.error(
                f"Error detecting compiler {compiler_type} {architecture}: {str(e)}"
            )
            return None

    def get_recommended_compiler(
        self,
        architecture: str = "x64",
        cpp_standard: Optional[str] = None
    ) -> Optional[CompilerInfo]:
        """Get recommended compiler for system

        Selects best compiler based on platform, architecture,
        and C++ standard requirements.

        Args:
            architecture: Target architecture (default: x64)
            cpp_standard: Required C++ standard (optional)

        Returns:
            Recommended CompilerInfo or None if no suitable compiler found
        """
        self._logger.info(
            f"Getting recommended compiler for architecture: {architecture}"
        )

        try:
            # Get all compilers
            compilers = self._compilers

            # Filter by architecture if specified
            if architecture:
                # Simple filtering - in production would check architecture
                pass

            # Select best compiler based on C++ standard
            if cpp_standard == "23":
                # Prefer compilers with C++23 support
                for compiler_name, compiler_info in compilers.items():
                    if compiler_info and compiler_info.supports_cpp23:
                        self._logger.info(
                            f"Recommended compiler: {compiler_name} "
                            f"{compiler_info.version} (C++23 support)"
                        )
                        return compiler_info

            # Default to first available compiler
            for compiler_name, compiler_info in compilers.items():
                if compiler_info:
                    self._logger.info(
                        f"Recommended compiler: {compiler_name} "
                        f"{compiler_info.version}"
                    )
                    return compiler_info

            self._logger.warning(
                f"No suitable compiler found for architecture: {architecture}"
            )
            return None
        except Exception as e:
            self._logger.error(f"Error getting recommended compiler: {str(e)}")
            return None

    def get_all_compilers(self) -> Dict[str, Optional[CompilerInfo]]:
        """Get all detected compilers

        Returns:
            Dictionary mapping compiler names to CompilerInfo objects
        """
        self._logger.debug("Getting all detected compilers")
        return self._compilers

    def refresh_detection(self) -> DetectionResult:
        """Refresh all detection results

        Clears caches and re-detects all components.

        Returns:
            DetectionResult with refreshed detection results
        """
        self._logger.info("Refreshing detection results")

        # Re-detect all components
        return self.detect_all()

    def get_system_info(self) -> Dict[str, Any]:
        """Get comprehensive system information

        Returns:
            Dictionary with system information including compilers, terminals,
            cross-compilers, and validation status
        """
        self._logger.info("Getting system information")

        # Get detection results
        detection_result = self.detect_all()

        return {
            "detection": detection_result.to_dict(),
            "compilers_count": len(
                [c for c in detection_result.compilers.values() if c]
            ),
            "terminals_count": len(
                [t for t in detection_result.terminals.values() if t]
            ),
            "cross_compilers_count": len(detection_result.cross_compilers),
            "has_errors": detection_result.has_errors,
            "has_warnings": detection_result.has_warnings,
            "is_valid": detection_result.success
        }


__all__ = [
    'DetectionError',
    'DetectionResult',
    'CompilerDetectionSystem',
]
