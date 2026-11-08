# warploom_scripts/compilers/detector.py
"""
Compiler detection module for Warploom project.

Provides compiler detection capabilities including MSVC, GCC, Clang,
and C++23 support validation.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Union

from warploom_scripts.logging.logger import get_logger
from warploom_scripts.platform.detector import PlatformInfo, detect_platform
from warploom_scripts.platform.linux import detect_clang, detect_gcc
from warploom_scripts.platform.windows import detect_mingw, detect_mingw_clang, detect_msvc


@dataclass
class CompilerInfo:
    """Information about a compiler.
    
    Attributes:
        name: Compiler name (MSVC, MSVC-Clang, MinGW-GCC, MinGW-Clang, GCC, Clang)
        version: Compiler version string
        path: Path to compiler executable
        supports_cpp23: Whether compiler supports C++23
        platform: Platform the compiler is running on
        extra_info: Additional compiler-specific information
    """
    name: str
    version: str
    path: Path
    supports_cpp23: bool
    platform: str
    extra_info: Optional[Dict[str, Union[str, Path]]] = None


@dataclass
class ValidationResult:
    """Result of compiler validation.
    
    Attributes:
        valid: Whether validation passed
        version: Compiler version
        warnings: List of warnings
        errors: List of errors
        fallback: Fallback C++ standard if C++23 not supported
    """
    valid: bool
    version: str
    warnings: List[str]
    errors: List[str]
    fallback: Optional[str] = None


def detect_compiler(
    compiler_name: Optional[str] = None,
    platform_info: Optional[PlatformInfo] = None
) -> Optional[CompilerInfo]:
    """Detect compiler based on platform and optional compiler name.
    
    This function detects the appropriate compiler based on the current
    platform and optionally a specific compiler name. If no compiler name
    is specified, it uses platform defaults.
    
    Args:
        compiler_name: Optional compiler name to detect
        platform_info: Optional platform information (auto-detected if not provided)
        
    Returns:
        CompilerInfo object if compiler is found, None otherwise
    """
    logger = get_logger(__name__)
    
    try:
        # Detect platform if not provided
        if platform_info is None:
            platform_info = detect_platform()
        
        # Detect compiler based on platform
        if platform_info.os == "Windows":
            return _detect_windows_compiler(compiler_name)
        elif platform_info.os == "Linux":
            return _detect_linux_compiler(compiler_name)
        elif platform_info.os == "macOS":
            return _detect_macos_compiler(compiler_name)
        else:
            logger.error(f"Unsupported platform: {platform_info.os}")
            return None
            
    except Exception as e:
        logger.error(f"Compiler detection failed: {e}")
        return None


def _detect_windows_compiler(
    compiler_name: Optional[str] = None
) -> Optional[CompilerInfo]:
    """Detect compiler on Windows.
    
    Args:
        compiler_name: Optional compiler name to detect
        
    Returns:
        CompilerInfo object if compiler is found, None otherwise
    """
    logger = get_logger(__name__)
    
    # If specific compiler requested
    if compiler_name:
        if compiler_name == "msvc":
            return _detect_msvc_compiler()
        elif compiler_name == "msvc-clang":
            return _detect_msvc_clang_compiler()
        elif compiler_name == "mingw-gcc":
            return _detect_mingw_gcc_compiler()
        elif compiler_name == "mingw-clang":
            return _detect_mingw_clang_compiler()
        else:
            logger.warning(f"Unknown compiler: {compiler_name}")
            return None
    
    # Auto-detect: try MSVC first, then MinGW
    msvc_info = _detect_msvc_compiler()
    if msvc_info:
        return msvc_info
    
    mingw_gcc_info = _detect_mingw_gcc_compiler()
    if mingw_gcc_info:
        return mingw_gcc_info
    
    mingw_clang_info = _detect_mingw_clang_compiler()
    if mingw_clang_info:
        return mingw_clang_info
    
    logger.warning("No compiler found on Windows")
    return None


def _detect_linux_compiler(
    compiler_name: Optional[str] = None
) -> Optional[CompilerInfo]:
    """Detect compiler on Linux.
    
    Args:
        compiler_name: Optional compiler name to detect
        
    Returns:
        CompilerInfo object if compiler is found, None otherwise
    """
    logger = get_logger(__name__)
    
    # If specific compiler requested
    if compiler_name:
        if compiler_name == "gcc":
            return _detect_gcc_compiler()
        elif compiler_name == "clang":
            return _detect_clang_compiler()
        else:
            logger.warning(f"Unknown compiler: {compiler_name}")
            return None
    
    # Auto-detect: try GCC first, then Clang
    gcc_info = _detect_gcc_compiler()
    if gcc_info:
        return gcc_info
    
    clang_info = _detect_clang_compiler()
    if clang_info:
        return clang_info
    
    logger.warning("No compiler found on Linux")
    return None


def _detect_macos_compiler(
    compiler_name: Optional[str] = None
) -> Optional[CompilerInfo]:
    """Detect compiler on macOS.
    
    Args:
        compiler_name: Optional compiler name to detect
        
    Returns:
        CompilerInfo object if compiler is found, None otherwise
    """
    logger = get_logger(__name__)
    
    # macOS uses Clang by default
    clang_info = _detect_clang_compiler()
    if clang_info:
        return clang_info
    
    logger.warning("No compiler found on macOS")
    return None


def _detect_msvc_compiler() -> Optional[CompilerInfo]:
    """Detect MSVC compiler.
    
    Returns:
        CompilerInfo object if MSVC is found, None otherwise
    """
    msvc_info = detect_msvc()
    if not msvc_info:
        return None
    
    # Check C++23 support
    supports_cpp23 = _check_msvc_cpp23_support(msvc_info.version)
    
    return CompilerInfo(
        name="MSVC",
        version=msvc_info.version,
        path=msvc_info.vcvars_path,
        supports_cpp23=supports_cpp23,
        platform="Windows",
        extra_info={
            "edition": msvc_info.edition,
            "year": msvc_info.year,
            "install_path": msvc_info.path
        }
    )


def _detect_msvc_clang_compiler() -> Optional[CompilerInfo]:
    """Detect MSVC-Clang compiler.
    
    Returns:
        CompilerInfo object if MSVC-Clang is found, None otherwise
    """
    logger = get_logger(__name__)
    
    # MSVC-Clang is clang-cl.exe in MSVC installation
    msvc_info = detect_msvc()
    if not msvc_info:
        return None
    
    # Look for clang-cl.exe
    clang_cl_path = msvc_info.path / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "clang-cl.exe"
    if not clang_cl_path.exists():
        logger.warning("clang-cl.exe not found in MSVC installation")
        return None
    
    # Get Clang version
    version = _get_clang_version_from_executable(clang_cl_path)
    
    # Check C++23 support
    supports_cpp23 = _check_clang_cpp23_support(version)
    
    return CompilerInfo(
        name="MSVC-Clang",
        version=version,
        path=clang_cl_path,
        supports_cpp23=supports_cpp23,
        platform="Windows",
        extra_info={
            "edition": msvc_info.edition,
            "year": msvc_info.year,
            "install_path": msvc_info.path
        }
    )


def _detect_mingw_gcc_compiler() -> Optional[CompilerInfo]:
    """Detect MinGW-GCC compiler.
    
    Returns:
        CompilerInfo object if MinGW-GCC is found, None otherwise
    """
    mingw_info = detect_mingw()
    if not mingw_info:
        return None
    
    # Check C++23 support
    supports_cpp23 = _check_gcc_cpp23_support(mingw_info.version)
    
    return CompilerInfo(
        name="MinGW-GCC",
        version=mingw_info.version,
        path=mingw_info.gcc_path,
        supports_cpp23=supports_cpp23,
        platform="Windows",
        extra_info={
            "environment": mingw_info.environment,
            "install_path": mingw_info.path,
            "gxx_path": mingw_info.gxx_path
        }
    )


def _detect_mingw_clang_compiler() -> Optional[CompilerInfo]:
    """Detect MinGW-Clang compiler.
    
    Returns:
        CompilerInfo object if MinGW-Clang is found, None otherwise
    """
    mingw_info = detect_mingw_clang()
    if not mingw_info:
        return None
    
    # Check C++23 support
    supports_cpp23 = _check_clang_cpp23_support(mingw_info.version)
    
    return CompilerInfo(
        name="MinGW-Clang",
        version=mingw_info.version,
        path=mingw_info.gcc_path,
        supports_cpp23=supports_cpp23,
        platform="Windows",
        extra_info={
            "environment": mingw_info.environment,
            "install_path": mingw_info.path,
            "clangxx_path": mingw_info.gxx_path
        }
    )


def _detect_gcc_compiler() -> Optional[CompilerInfo]:
    """Detect GCC compiler.
    
    Returns:
        CompilerInfo object if GCC is found, None otherwise
    """
    gcc_info = detect_gcc()
    if not gcc_info:
        return None
    
    return CompilerInfo(
        name="GCC",
        version=gcc_info.version,
        path=gcc_info.path,
        supports_cpp23=gcc_info.supports_cpp23,
        platform="Linux",
        extra_info={
            "gxx_path": gcc_info.gxx_path
        }
    )


def _detect_clang_compiler() -> Optional[CompilerInfo]:
    """Detect Clang compiler.
    
    Returns:
        CompilerInfo object if Clang is found, None otherwise
    """
    clang_info = detect_clang()
    if not clang_info:
        return None
    
    return CompilerInfo(
        name="Clang",
        version=clang_info.version,
        path=clang_info.path,
        supports_cpp23=clang_info.supports_cpp23,
        platform="Linux",
        extra_info={
            "clangxx_path": clang_info.clangxx_path
        }
    )


def validate_cpp23_support(compiler_info: CompilerInfo) -> ValidationResult:
    """Validate compiler C++23 support.
    
    This function validates whether a compiler supports C++23 and provides
    detailed validation results including warnings and errors.
    
    Args:
        compiler_info: CompilerInfo object to validate
        
    Returns:
        ValidationResult object with validation details
    """
    logger = get_logger(__name__)
    
    warnings: List[str] = []
    errors: List[str] = []
    
    # Check if compiler supports C++23
    if compiler_info.supports_cpp23:
        logger.info(f"{compiler_info.name} {compiler_info.version} supports C++23")
        return ValidationResult(
            valid=True,
            version=compiler_info.version,
            warnings=warnings,
            errors=errors,
            fallback=None
        )
    
    # Compiler doesn't support C++23
    warnings.append(f"{compiler_info.name} {compiler_info.version} does not fully support C++23")
    
    # Determine fallback
    fallback = "C++20"
    warnings.append(f"Falling back to {fallback}")
    logger.warning(
        f"{compiler_info.name} {compiler_info.version} does not support C++23, "
        f"falling back to {fallback}"
    )
    
    return ValidationResult(
        valid=False,
        version=compiler_info.version,
        warnings=warnings,
        errors=errors,
        fallback=fallback
    )


def _check_msvc_cpp23_support(version: str) -> bool:
    """Check if MSVC version supports C++23.
    
    Args:
        version: MSVC version string (e.g., '19.40')
        
    Returns:
        True if MSVC supports C++23, False otherwise
    """
    try:
        # MSVC 19.35+ (Visual Studio 2022 17.5+) has good C++23 support
        major, minor = map(int, version.split("."))
        if major > 19 or (major == 19 and minor >= 35):
            return True
        return False
    except Exception:
        return False


def _check_gcc_cpp23_support(version: str) -> bool:
    """Check if GCC version supports C++23.
    
    Args:
        version: GCC version string (e.g., '13.2.0')
        
    Returns:
        True if GCC supports C++23, False otherwise
    """
    try:
        # GCC 13+ has good C++23 support
        major = int(version.split(".")[0])
        return major >= 13
    except Exception:
        return False


def _check_clang_cpp23_support(version: str) -> bool:
    """Check if Clang version supports C++23.
    
    Args:
        version: Clang version string (e.g., '18.1.3')
        
    Returns:
        True if Clang supports C++23, False otherwise
    """
    try:
        # Clang 16+ has good C++23 support
        major = int(version.split(".")[0])
        return major >= 16
    except Exception:
        return False


def _get_clang_version_from_executable(clang_path: Path) -> str:
    """Get Clang version from executable.
    
    Args:
        clang_path: Path to clang executable
        
    Returns:
        Clang version string
    """
    logger = get_logger(__name__)
    
    try:
        result = subprocess.run(
            [str(clang_path), "--version"],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.stdout:
            match = re.search(r"clang version (\d+\.\d+\.\d+)", result.stdout)
            if match:
                return match.group(1)
        
        logger.warning("Could not determine Clang version, using default")
        return "18.1.0"
        
    except Exception as e:
        logger.warning(f"Failed to get Clang version: {e}, using default")
        return "18.1.0"


def detect_all_compilers(
    platform_info: Optional[PlatformInfo] = None
) -> Dict[str, Optional[CompilerInfo]]:
    """Detect all available compilers on the platform.
    
    This function detects all available compilers and returns
    information about each one.
    
    Args:
        platform_info: Optional platform information (auto-detected if not provided)
        
    Returns:
        Dictionary mapping compiler names to CompilerInfo objects
    """
    logger = get_logger(__name__)
    
    if platform_info is None:
        platform_info = detect_platform()
    
    compilers: Dict[str, Optional[CompilerInfo]] = {}
    
    if platform_info.os == "Windows":
        compilers["msvc"] = _detect_msvc_compiler()
        compilers["msvc-clang"] = _detect_msvc_clang_compiler()
        compilers["mingw-gcc"] = _detect_mingw_gcc_compiler()
        compilers["mingw-clang"] = _detect_mingw_clang_compiler()
    elif platform_info.os == "Linux":
        compilers["gcc"] = _detect_gcc_compiler()
        compilers["clang"] = _detect_clang_compiler()
    elif platform_info.os == "macOS":
        compilers["clang"] = _detect_clang_compiler()
    
    # Log summary
    available = [name for name, info in compilers.items() if info is not None]
    if available:
        logger.info(f"Available compilers: {', '.join(available)}")
    else:
        logger.warning("No compilers found")
    
    return compilers


__all__ = [
    'CompilerInfo',
    'ValidationResult',
    'detect_compiler',
    'validate_cpp23_support',
    'detect_all_compilers',
]
