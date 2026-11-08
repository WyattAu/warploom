# warploom_scripts/compilers/msvc.py
"""
MSVC compiler detection and validation for OmniCPP project.

Provides MSVC-specific compiler detection, version checking,
and C++23 support validation.
"""

from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

from warploom_scripts.logging.logger import get_logger


@dataclass
class MSVCCompilerInfo:
    """Detailed information about MSVC compiler.
    
    Attributes:
        version: MSVC version (e.g., '19.40')
        edition: Visual Studio edition (Community, Professional, Enterprise)
        year: Visual Studio year (e.g., '2022')
        install_path: Path to Visual Studio installation
        vcvars_path: Path to vcvars64.bat
        supports_cpp23: Whether MSVC supports C++23
        toolset_version: MSVC toolset version
    """
    version: str
    edition: str
    year: str
    install_path: Path
    vcvars_path: Path
    supports_cpp23: bool
    toolset_version: str


def detect_msvc() -> Optional[MSVCCompilerInfo]:
    """Detect MSVC compiler installation.
    
    This function searches for Visual Studio installations and validates
    MSVC compiler availability. It checks for vcvars64.bat and determines
    MSVC version and edition.
    
    Returns:
        MSVCCompilerInfo object if MSVC is found, None otherwise
    """
    logger = get_logger(__name__)
    
    try:
        # Try to use vswhere to find Visual Studio installations
        vswhere_path = _find_vswhere()
        if vswhere_path:
            installations = _get_vs_installations(vswhere_path)
            if installations:
                # Use the latest installation
                latest = installations[0]
                return _create_msvc_info(latest)
        
        # Fallback: search common installation paths
        installations = _search_common_vs_paths()
        if installations:
            latest = installations[0]
            return _create_msvc_info(latest)
        
        logger.warning("MSVC not found")
        return None
        
    except Exception as e:
        logger.error(f"MSVC detection failed: {e}")
        return None


def _find_vswhere() -> Optional[Path]:
    """Find vswhere executable.
    
    vswhere is a tool from Microsoft to locate Visual Studio installations.
    
    Returns:
        Path to vswhere.exe if found, None otherwise
    """
    logger = get_logger(__name__)
    
    # Common vswhere locations
    vswhere_paths = [
        Path("C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"),
        Path("C:/Program Files/Microsoft Visual Studio/Installer/vswhere.exe"),
    ]
    
    for path in vswhere_paths:
        if path.exists():
            logger.debug(f"Found vswhere at {path}")
            return path
    
    return None


def _get_vs_installations(vswhere_path: Path) -> List[Dict[str, Union[str, Path]]]:
    """Get Visual Studio installations using vswhere.
    
    Args:
        vswhere_path: Path to vswhere.exe
        
    Returns:
        List of installation dictionaries sorted by version (newest first)
    """
    logger = get_logger(__name__)
    
    try:
        result = subprocess.run(
            [
                str(vswhere_path),
                "-latest",
                "-property", "installationPath",
                "-property", "displayName",
                "-property", "installationVersion",
                "-format", "json"
            ],
            capture_output=True,
            text=True,
            timeout=10
        )
        
        if result.returncode != 0:
            logger.warning(f"vswhere failed: {result.stderr}")
            return []
        
        installations = json.loads(result.stdout)
        
        # Parse installations
        parsed: List[Dict[str, Union[str, Path]]] = []
        for inst in installations:
            parsed.append({
                "path": Path(inst.get("installationPath", "")),
                "name": inst.get("displayName", ""),
                "version": inst.get("installationVersion", "")
            })
        
        # Sort by version (newest first)
        parsed.sort(key=lambda x: x["version"], reverse=True)
        
        return parsed
        
    except Exception as e:
        logger.warning(f"Failed to get VS installations: {e}")
        return []


def _search_common_vs_paths() -> List[Dict[str, Union[str, Path]]]:
    """Search common Visual Studio installation paths.
    
    Returns:
        List of installation dictionaries
    """
    installations: List[Dict[str, Union[str, Path]]] = []
    
    # Common Visual Studio installation paths
    vs_paths = [
        Path("C:/Program Files/Microsoft Visual Studio/2022"),
        Path("C:/Program Files (x86)/Microsoft Visual Studio/2022"),
        Path("C:/Program Files/Microsoft Visual Studio/2019"),
        Path("C:/Program Files (x86)/Microsoft Visual Studio/2019"),
    ]
    
    editions = ["Enterprise", "Professional", "Community", "BuildTools"]
    
    for vs_path in vs_paths:
        if not vs_path.exists():
            continue
        
        for edition in editions:
            edition_path = vs_path / edition
            if edition_path.exists():
                # Extract year from path
                year = vs_path.name
                installations.append({
                    "path": edition_path,
                    "name": f"Visual Studio {year} {edition}",
                    "version": year
                })
    
    return installations


def _create_msvc_info(installation: Dict[str, Union[str, Path]]) -> Optional[MSVCCompilerInfo]:
    """Create MSVCCompilerInfo from installation dictionary.
    
    Args:
        installation: Installation dictionary with path, name, version
        
    Returns:
        MSVCCompilerInfo object if valid, None otherwise
    """
    logger = get_logger(__name__)
    
    try:
        install_path = Path(installation["path"])
        name = str(installation["name"])
        version_str = str(installation["version"])
        
        # Extract edition from name
        edition = "Community"
        if "Enterprise" in name:
            edition = "Enterprise"
        elif "Professional" in name:
            edition = "Professional"
        elif "BuildTools" in name:
            edition = "BuildTools"
        
        # Extract year from version or name
        year = "2022"
        if "2019" in version_str or "2019" in name:
            year = "2019"
        elif "2017" in version_str or "2017" in name:
            year = "2017"
        
        # Find vcvars64.bat
        vcvars_path = _find_vcvars(Path(install_path))
        if not vcvars_path:
            logger.warning(f"vcvars64.bat not found in {install_path}")
            return None
        
        # Get MSVC version
        msvc_version = _get_msvc_version(Path(install_path))
        
        # Check C++23 support
        supports_cpp23 = _check_cpp23_support(msvc_version)
        
        # Get toolset version
        toolset_version = _get_toolset_version(Path(install_path))
        
        msvc_info = MSVCCompilerInfo(
            version=msvc_version,
            edition=edition,
            year=year,
            install_path=install_path,
            vcvars_path=vcvars_path,
            supports_cpp23=supports_cpp23,
            toolset_version=toolset_version
        )
        
        logger.info(
            f"Found MSVC {msvc_version} ({edition} {year}) "
            f"at {install_path}"
        )
        
        return msvc_info
        
    except Exception as e:
        logger.error(f"Failed to create MSVC info: {e}")
        return None


def _find_vcvars(install_path: Path) -> Optional[Path]:
    """Find vcvars64.bat in Visual Studio installation.
    
    Args:
        install_path: Path to Visual Studio installation
        
    Returns:
        Path to vcvars64.bat if found, None otherwise
    """
    # Common vcvars64.bat locations
    vcvars_paths = [
        install_path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat",
        install_path / "VC" / "Auxiliary" / "Build" / "vcvarsamd64_arm64.bat",
    ]
    
    for path in vcvars_paths:
        if path.exists():
            return path
    
    return None


def _get_msvc_version(install_path: Path) -> str:
    """Get MSVC version from installation.
    
    Args:
        install_path: Path to Visual Studio installation
        
    Returns:
        MSVC version string
    """
    logger = get_logger(__name__)
    
    try:
        # Try to find cl.exe and get version
        vc_tools_path = install_path / "VC" / "Tools" / "MSVC"
        if vc_tools_path.exists():
            # Get latest version directory
            versions = sorted(vc_tools_path.iterdir(), reverse=True)
            if versions:
                # Extract version from directory name (e.g., 14.40.33807)
                version_dir = versions[0].name
                # Convert to MSVC version format (e.g., 14.40 -> 19.40)
                match = re.match(r"14\.(\d+)\.\d+", version_dir)
                if match:
                    return f"19.{match.group(1)}"
        
        # Fallback: try to run cl.exe to get version
        try:
            result = subprocess.run(
                ["cl.exe"],
                capture_output=True,
                text=True,
                timeout=5,
                shell=True
            )
            if result.stderr:
                # Parse version from cl.exe output
                match = re.search(r"Compiler Version (\d+\.\d+)", result.stderr)
                if match:
                    return match.group(1)
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
        
        # Default version if detection fails
        logger.warning("Could not determine MSVC version, using default")
        return "19.40"
        
    except Exception as e:
        logger.warning(f"Failed to get MSVC version: {e}, using default")
        return "19.40"


def _get_toolset_version(install_path: Path) -> str:
    """Get MSVC toolset version.
    
    Args:
        install_path: Path to Visual Studio installation
        
    Returns:
        Toolset version string
    """
    logger = get_logger(__name__)
    
    try:
        vc_tools_path = install_path / "VC" / "Tools" / "MSVC"
        if vc_tools_path.exists():
            versions = sorted(vc_tools_path.iterdir(), reverse=True)
            if versions:
                return versions[0].name
        
        return "14.40.33807"
        
    except Exception as e:
        logger.warning(f"Failed to get toolset version: {e}, using default")
        return "14.40.33807"


def _check_cpp23_support(version: str) -> bool:
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


def validate_msvc(msvc_info: MSVCCompilerInfo) -> Dict[str, Any]:
    """Validate MSVC installation.
    
    This function validates MSVC installation and provides detailed
    validation results.
    
    Args:
        msvc_info: MSVCCompilerInfo object to validate
        
    Returns:
        Dictionary with validation results
    """
    logger = get_logger(__name__)
    
    results: Dict[str, Any] = {
        "valid": True,
        "warnings": [],
        "errors": []
    }
    
    # Check if vcvars64.bat exists
    if not msvc_info.vcvars_path.exists():
        results["valid"] = False
        results["errors"].append(
            f"vcvars64.bat not found at {msvc_info.vcvars_path}"
        )
    
    # Check C++23 support
    if not msvc_info.supports_cpp23:
        results["warnings"].append(
            f"MSVC {msvc_info.version} does not fully support C++23"
        )
    
    # Log results
    if results["valid"]:
        logger.info(f"MSVC validation passed")
    else:
        logger.error(f"MSVC validation failed: {results['errors']}")
    
    return results


__all__ = [
    'MSVCCompilerInfo',
    'detect_msvc',
    'validate_msvc',
]
