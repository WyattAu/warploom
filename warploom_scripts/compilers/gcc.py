# warploom_scripts/compilers/gcc.py
"""
GCC compiler detection and validation for OmniCPP project.

Provides GCC-specific compiler detection, version checking,
and C++23 support validation.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional

from warploom_scripts.logging.logger import get_logger


@dataclass
class GCCCompilerInfo:
    """Detailed information about GCC compiler.
    
    Attributes:
        version: GCC version (e.g., '13.2.0')
        path: Path to gcc executable
        gxx_path: Path to g++ executable
        supports_cpp23: Whether GCC supports C++23
        target: GCC target triplet (e.g., 'x86_64-linux-gnu')
        install_prefix: GCC installation prefix
    """
    version: str
    path: Path
    gxx_path: Path
    supports_cpp23: bool
    target: str
    install_prefix: str


def detect_gcc() -> Optional[GCCCompilerInfo]:
    """Detect GCC compiler installation.
    
    This function searches for GCC compiler installation and validates
    compiler availability. It checks for gcc and g++ executables and
    determines GCC version and target.
    
    Returns:
        GCCCompilerInfo object if GCC is found, None otherwise
    """
    logger = get_logger(__name__)
    
    try:
        # Try to find gcc in PATH
        gcc_path = _find_executable("gcc")
        if not gcc_path:
            logger.warning("GCC not found in PATH")
            return None
        
        gxx_path = _find_executable("g++")
        if not gxx_path:
            logger.warning("g++ not found in PATH")
            return None
        
        # Get GCC version
        version = _get_gcc_version(gcc_path)
        
        # Get GCC target
        target = _get_gcc_target(gcc_path)
        
        # Get GCC install prefix
        install_prefix = _get_gcc_prefix(gcc_path)
        
        # Check C++23 support
        supports_cpp23 = _check_cpp23_support(version)
        
        gcc_info = GCCCompilerInfo(
            version=version,
            path=gcc_path,
            gxx_path=gxx_path,
            supports_cpp23=supports_cpp23,
            target=target,
            install_prefix=install_prefix
        )
        
        logger.info(
            f"Found GCC {version} ({target}) at {gcc_path}"
        )
        
        return gcc_info
        
    except Exception as e:
        logger.error(f"GCC detection failed: {e}")
        return None


def _find_executable(name: str) -> Optional[Path]:
    """Find an executable in PATH.
    
    Args:
        name: Name of the executable to find
        
    Returns:
        Path to the executable if found, None otherwise
    """
    try:
        result = subprocess.run(
            ["which", name],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode == 0 and result.stdout.strip():
            return Path(result.stdout.strip())
        
        return None
        
    except Exception:
        return None


def _get_gcc_version(gcc_path: Path) -> str:
    """Get GCC version from gcc executable.
    
    Args:
        gcc_path: Path to gcc executable
        
    Returns:
        GCC version string
    """
    logger = get_logger(__name__)
    
    try:
        result = subprocess.run(
            [str(gcc_path), "--version"],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.stdout:
            # Parse version from gcc output
            # Output format: "gcc (GCC) 13.2.0"
            match = re.search(r"gcc.*?(\d+\.\d+\.\d+)", result.stdout)
            if match:
                return match.group(1)
        
        logger.warning("Could not determine GCC version, using default")
        return "13.2.0"
        
    except Exception as e:
        logger.warning(f"Failed to get GCC version: {e}, using default")
        return "13.2.0"


def _get_gcc_target(gcc_path: Path) -> str:
    """Get GCC target triplet.
    
    Args:
        gcc_path: Path to gcc executable
        
    Returns:
        GCC target triplet string
    """
    logger = get_logger(__name__)
    
    try:
        result = subprocess.run(
            [str(gcc_path), "-dumpmachine"],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
        
        return "unknown"
        
    except Exception as e:
        logger.warning(f"Failed to get GCC target: {e}")
        return "unknown"


def _get_gcc_prefix(gcc_path: Path) -> str:
    """Get GCC installation prefix.
    
    Args:
        gcc_path: Path to gcc executable
        
    Returns:
        GCC installation prefix string
    """
    logger = get_logger(__name__)
    
    try:
        result = subprocess.run(
            [str(gcc_path), "-print-sysroot"],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
        
        return ""
        
    except Exception as e:
        logger.warning(f"Failed to get GCC prefix: {e}")
        return ""


def _check_cpp23_support(version: str) -> bool:
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


def validate_gcc(gcc_info: GCCCompilerInfo) -> Dict[str, Any]:
    """Validate GCC installation.
    
    This function validates GCC installation and provides detailed
    validation results.
    
    Args:
        gcc_info: GCCCompilerInfo object to validate
        
    Returns:
        Dictionary with validation results
    """
    logger = get_logger(__name__)
    
    results: Dict[str, Any] = {
        "valid": True,
        "warnings": [],
        "errors": []
    }
    
    # Check if gcc exists
    if not gcc_info.path.exists():
        results["valid"] = False
        results["errors"].append(
            f"gcc not found at {gcc_info.path}"
        )
    
    # Check if g++ exists
    if not gcc_info.gxx_path.exists():
        results["valid"] = False
        results["errors"].append(
            f"g++ not found at {gcc_info.gxx_path}"
        )
    
    # Check C++23 support
    if not gcc_info.supports_cpp23:
        results["warnings"].append(
            f"GCC {gcc_info.version} does not fully support C++23"
        )
    
    # Log results
    if results["valid"]:
        logger.info(f"GCC validation passed")
    else:
        logger.error(f"GCC validation failed: {results['errors']}")
    
    return results


def test_gcc_cpp23(gcc_info: GCCCompilerInfo) -> bool:
    """Test GCC C++23 support by compiling a test program.
    
    Args:
        gcc_info: GCCCompilerInfo object to test
        
    Returns:
        True if GCC supports C++23, False otherwise
    """
    logger = get_logger(__name__)
    
    try:
        # Create a simple C++23 test program
        test_code = """
        #include <version>
        #if __cpp_modules >= 202207L
        int main() { return 0; }
        #else
        #error "C++23 not supported"
        #endif
        """
        
        result = subprocess.run(
            [str(gcc_info.gxx_path), "-std=c++23", "-x", "c++", "-o", "/dev/null", "-"],
            input=test_code,
            capture_output=True,
            text=True,
            timeout=10
        )
        
        success = result.returncode == 0
        
        if success:
            logger.info(f"GCC {gcc_info.version} C++23 test passed")
        else:
            logger.warning(f"GCC {gcc_info.version} C++23 test failed")
        
        return success
        
    except Exception as e:
        logger.warning(f"Failed to test GCC C++23 support: {e}")
        return False


__all__ = [
    'GCCCompilerInfo',
    'detect_gcc',
    'validate_gcc',
    'test_gcc_cpp23',
]
