# warploom_scripts/compilers/clang.py
"""
Clang compiler detection and validation for Warploom project.

Provides Clang-specific compiler detection, version checking,
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
class ClangCompilerInfo:
    """Detailed information about Clang compiler.
    
    Attributes:
        version: Clang version (e.g., '18.1.3')
        path: Path to clang executable
        clangxx_path: Path to clang++ executable
        supports_cpp23: Whether Clang supports C++23
        target: Clang target triplet (e.g., 'x86_64-unknown-linux-gnu')
        install_prefix: Clang installation prefix
    """
    version: str
    path: Path
    clangxx_path: Path
    supports_cpp23: bool
    target: str
    install_prefix: str


def detect_clang() -> Optional[ClangCompilerInfo]:
    """Detect Clang compiler installation.
    
    This function searches for Clang compiler installation and validates
    compiler availability. It checks for clang and clang++ executables and
    determines Clang version and target.
    
    Returns:
        ClangCompilerInfo object if Clang is found, None otherwise
    """
    logger = get_logger(__name__)
    
    try:
        # Try to find clang in PATH
        clang_path = _find_executable("clang")
        if not clang_path:
            logger.warning("Clang not found in PATH")
            return None
        
        clangxx_path = _find_executable("clang++")
        if not clangxx_path:
            logger.warning("clang++ not found in PATH")
            return None
        
        # Get Clang version
        version = _get_clang_version(clang_path)
        
        # Get Clang target
        target = _get_clang_target(clang_path)
        
        # Get Clang install prefix
        install_prefix = _get_clang_prefix(clang_path)
        
        # Check C++23 support
        supports_cpp23 = _check_cpp23_support(version)
        
        clang_info = ClangCompilerInfo(
            version=version,
            path=clang_path,
            clangxx_path=clangxx_path,
            supports_cpp23=supports_cpp23,
            target=target,
            install_prefix=install_prefix
        )
        
        logger.info(
            f"Found Clang {version} ({target}) at {clang_path}"
        )
        
        return clang_info
        
    except Exception as e:
        logger.error(f"Clang detection failed: {e}")
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


def _get_clang_version(clang_path: Path) -> str:
    """Get Clang version from clang executable.
    
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
            # Parse version from clang output
            # Output format: "clang version 18.1.3"
            match = re.search(r"clang version (\d+\.\d+\.\d+)", result.stdout)
            if match:
                return match.group(1)
        
        logger.warning("Could not determine Clang version, using default")
        return "18.1.0"
        
    except Exception as e:
        logger.warning(f"Failed to get Clang version: {e}, using default")
        return "18.1.0"


def _get_clang_target(clang_path: Path) -> str:
    """Get Clang target triplet.
    
    Args:
        clang_path: Path to clang executable
        
    Returns:
        Clang target triplet string
    """
    logger = get_logger(__name__)
    
    try:
        result = subprocess.run(
            [str(clang_path), "-dumpmachine"],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
        
        return "unknown"
        
    except Exception as e:
        logger.warning(f"Failed to get Clang target: {e}")
        return "unknown"


def _get_clang_prefix(clang_path: Path) -> str:
    """Get Clang installation prefix.
    
    Args:
        clang_path: Path to clang executable
        
    Returns:
        Clang installation prefix string
    """
    logger = get_logger(__name__)
    
    try:
        result = subprocess.run(
            [str(clang_path), "-print-resource-dir"],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode == 0 and result.stdout.strip():
            # Get parent directory of resource dir
            resource_dir = Path(result.stdout.strip())
            if resource_dir.exists():
                return str(resource_dir.parent)
        
        return ""
        
    except Exception as e:
        logger.warning(f"Failed to get Clang prefix: {e}")
        return ""


def _check_cpp23_support(version: str) -> bool:
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


def validate_clang(clang_info: ClangCompilerInfo) -> Dict[str, Any]:
    """Validate Clang installation.
    
    This function validates Clang installation and provides detailed
    validation results.
    
    Args:
        clang_info: ClangCompilerInfo object to validate
        
    Returns:
        Dictionary with validation results
    """
    logger = get_logger(__name__)
    
    results: Dict[str, Any] = {
        "valid": True,
        "warnings": [],
        "errors": []
    }
    
    # Check if clang exists
    if not clang_info.path.exists():
        results["valid"] = False
        results["errors"].append(
            f"clang not found at {clang_info.path}"
        )
    
    # Check if clang++ exists
    if not clang_info.clangxx_path.exists():
        results["valid"] = False
        results["errors"].append(
            f"clang++ not found at {clang_info.clangxx_path}"
        )
    
    # Check C++23 support
    if not clang_info.supports_cpp23:
        results["warnings"].append(
            f"Clang {clang_info.version} does not fully support C++23"
        )
    
    # Log results
    if results["valid"]:
        logger.info(f"Clang validation passed")
    else:
        logger.error(f"Clang validation failed: {results['errors']}")
    
    return results


def test_clang_cpp23(clang_info: ClangCompilerInfo) -> bool:
    """Test Clang C++23 support by compiling a test program.
    
    Args:
        clang_info: ClangCompilerInfo object to test
        
    Returns:
        True if Clang supports C++23, False otherwise
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
            [str(clang_info.clangxx_path), "-std=c++23", "-x", "c++", "-o", "/dev/null", "-"],
            input=test_code,
            capture_output=True,
            text=True,
            timeout=10
        )
        
        success = result.returncode == 0
        
        if success:
            logger.info(f"Clang {clang_info.version} C++23 test passed")
        else:
            logger.warning(f"Clang {clang_info.version} C++23 test failed")
        
        return success
        
    except Exception as e:
        logger.warning(f"Failed to test Clang C++23 support: {e}")
        return False


__all__ = [
    'ClangCompilerInfo',
    'detect_clang',
    'validate_clang',
    'test_clang_cpp23',
]
