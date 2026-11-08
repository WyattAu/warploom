# warploom_scripts/platform/windows.py
"""
Windows-specific platform detection for Warploom project.

Provides Windows platform detection including MSVC and MinGW compiler detection,
and terminal environment setup for different compilers.
"""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional

from warploom_scripts.logging.logger import get_logger


@dataclass
class MSVCInfo:
    """Information about MSVC installation.

    Attributes:
        version: MSVC version (e.g., '19.40')
        path: Path to MSVC installation directory
        vcvars_path: Path to vcvars64.bat file
        edition: Visual Studio edition (Community, Professional, Enterprise)
        year: Visual Studio year (e.g., '2022')
    """
    version: str
    path: Path
    vcvars_path: Path
    edition: str
    year: str


@dataclass
class MinGWInfo:
    """Information about MinGW installation.

    Attributes:
        version: MinGW version
        path: Path to MinGW installation directory
        environment: Environment type (UCRT64, MSYS2)
        gcc_path: Path to gcc executable
        gxx_path: Path to g++ executable
    """
    version: str
    path: Path
    environment: str
    gcc_path: Path
    gxx_path: Path


def detect_msvc() -> Optional[MSVCInfo]:
    """Detect MSVC compiler installation on Windows.

    This function searches for Visual Studio installations and validates
    MSVC compiler availability. It checks for vcvars64.bat and determines
    the MSVC version.

    Returns:
        MSVCInfo object if MSVC is found, None otherwise
    """
    logger = get_logger(__name__)

    try:
        # Common Visual Studio installation paths
        vs_paths = [
            Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio",
            Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Microsoft Visual Studio",
        ]

        # Search for Visual Studio 2022, 2019, 2017
        vs_years = ["2022", "2019", "2017"]
        editions = ["Enterprise", "Professional", "Community", "BuildTools"]

        for vs_path in vs_paths:
            if not vs_path.exists():
                continue

            for year in vs_years:
                for edition in editions:
                    vs_install_path = vs_path / year / edition
                    if not vs_install_path.exists():
                        continue

                    # Look for vcvars64.bat
                    vcvars_paths = [
                        vs_install_path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat",
                        vs_install_path / "VC" / "Auxiliary" / "Build" / "vcvarsamd64_arm64.bat",
                    ]

                    for vcvars_path in vcvars_paths:
                        if vcvars_path.exists():
                            # Try to get MSVC version
                            version = _get_msvc_version(vs_install_path)

                            msvc_info = MSVCInfo(
                                version=version,
                                path=vs_install_path,
                                vcvars_path=vcvars_path,
                                edition=edition,
                                year=year
                            )

                            logger.info(
                                f"Found MSVC {version} ({edition} {year}) "
                                f"at {vs_install_path}"
                            )
                            return msvc_info

        logger.warning("MSVC not found")
        return None

    except Exception as e:
        logger.error(f"MSVC detection failed: {e}")
        return None


def _get_msvc_version(vs_path: Path) -> str:
    """Get MSVC version from Visual Studio installation.

    Args:
        vs_path: Path to Visual Studio installation

    Returns:
        MSVC version string
    """
    logger = get_logger(__name__)

    try:
        # Try to find cl.exe and get version
        vc_tools_path = vs_path / "VC" / "Tools" / "MSVC"
        if vc_tools_path.exists():
            # Get the latest version directory
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


def detect_mingw() -> Optional[MinGWInfo]:
    """Detect MinGW compiler installation on Windows.

    This function searches for MinGW-w64 installations in common locations
    and validates compiler availability. It supports both UCRT64 and MSYS2
    environments.

    Returns:
        MinGWInfo object if MinGW is found, None otherwise
    """
    logger = get_logger(__name__)

    try:
        # Common MinGW installation paths
        mingw_paths = [
            Path("C:/msys64"),
            Path("C:/mingw64"),
            Path("C:/mingw"),
            Path(os.environ.get("MSYS2_PATH", "")),
        ]

        # Environment types to check
        environments = ["UCRT64", "MINGW64", "CLANG64", "MSYS2"]

        for mingw_path in mingw_paths:
            if not mingw_path.exists():
                continue

            for env in environments:
                # Check for gcc and g++ in environment
                bin_path = mingw_path / env / "bin"
                if not bin_path.exists():
                    continue

                gcc_path = bin_path / "gcc.exe"
                gxx_path = bin_path / "g++.exe"

                if gcc_path.exists() and gxx_path.exists():
                    # Get GCC version
                    version = _get_gcc_version(gcc_path)

                    mingw_info = MinGWInfo(
                        version=version,
                        path=mingw_path,
                        environment=env,
                        gcc_path=gcc_path,
                        gxx_path=gxx_path
                    )

                    logger.info(
                        f"Found MinGW-GCC {version} ({env}) "
                        f"at {mingw_path}"
                    )
                    return mingw_info

        logger.warning("MinGW not found")
        return None

    except Exception as e:
        logger.error(f"MinGW detection failed: {e}")
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
            # Output format: "gcc.exe (GCC) 13.2.0"
            match = re.search(r"gcc\.exe.*?(\d+\.\d+\.\d+)", result.stdout)
            if match:
                return match.group(1)

        logger.warning("Could not determine GCC version, using default")
        return "13.2.0"

    except Exception as e:
        logger.warning(f"Failed to get GCC version: {e}, using default")
        return "13.2.0"


def detect_mingw_clang() -> Optional[MinGWInfo]:
    """Detect MinGW-Clang compiler installation on Windows.

    This function searches for Clang installations in MinGW environments
    and validates compiler availability.

    Returns:
        MinGWInfo object if MinGW-Clang is found, None otherwise
    """
    logger = get_logger(__name__)

    try:
        # Common MinGW installation paths
        mingw_paths = [
            Path("C:/msys64"),
            Path("C:/mingw64"),
            Path("C:/mingw"),
            Path(os.environ.get("MSYS2_PATH", "")),
        ]

        # Environment types to check for Clang
        environments = ["CLANG64", "UCRT64", "MINGW64"]

        for mingw_path in mingw_paths:
            if not mingw_path.exists():
                continue

            for env in environments:
                # Check for clang and clang++ in environment
                bin_path = mingw_path / env / "bin"
                if not bin_path.exists():
                    continue

                clang_path = bin_path / "clang.exe"
                clangxx_path = bin_path / "clang++.exe"

                if clang_path.exists() and clangxx_path.exists():
                    # Get Clang version
                    version = _get_clang_version(clang_path)

                    mingw_info = MinGWInfo(
                        version=version,
                        path=mingw_path,
                        environment=env,
                        gcc_path=clang_path,
                        gxx_path=clangxx_path
                    )

                    logger.info(
                        f"Found MinGW-Clang {version} ({env}) "
                        f"at {mingw_path}"
                    )
                    return mingw_info

        logger.warning("MinGW-Clang not found")
        return None

    except Exception as e:
        logger.error(f"MinGW-Clang detection failed: {e}")
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


def setup_msvc_environment(arch: str = 'x64') -> Dict[str, str]:
    """Set up Visual Studio Developer Command Prompt environment for MSVC.

    This function locates vcvars64.bat automatically and sets up the required
    environment variables for MSVC compilation. It supports multiple Visual Studio
    editions and versions, and both x64 and ARM64 architectures.

    Args:
        arch: Architecture to set up ('x64' or 'ARM64'). Defaults to 'x64'.

    Returns:
        Dictionary of environment variables set up for MSVC.

    Raises:
        RuntimeError: If vcvars64.bat cannot be found or setup fails.

    Example:
        >>> env = setup_msvc_environment('x64')
        >>> os.environ.update(env)
    """
    logger = get_logger(__name__)

    try:
        # Detect MSVC installation
        msvc_info = detect_msvc()
        if not msvc_info:
            raise RuntimeError(
                "MSVC not found. Please install Visual Studio 2022 with C++ workload."
            )

        # Select appropriate vcvars file based on architecture
        if arch.upper() == 'ARM64':
            vcvars_file = msvc_info.path / "VC" / "Auxiliary" / "Build" / "vcvarsamd64_arm64.bat"
        else:
            vcvars_file = msvc_info.vcvars_path

        if not vcvars_file.exists():
            raise RuntimeError(
                f"vcvars file not found: {vcvars_file}"
            )

        logger.info(f"Setting up MSVC environment for {arch} using {vcvars_file}")

        # Execute vcvars.bat and capture environment variables
        # We use a temporary batch file to capture the environment
        import tempfile
        with tempfile.NamedTemporaryFile(mode='w', suffix='.bat', delete=False) as f:
            temp_bat = f.name
            f.write(f'@echo off\n')
            f.write(f'call "{vcvars_file}"\n')
            f.write(f'set\n')

        try:
            result = subprocess.run(
                temp_bat,
                capture_output=True,
                text=True,
                timeout=30,
                shell=True,
                env=os.environ.copy()  # Ensure we have a clean environment copy
            )

            if result.returncode != 0:
                logger.error(f"vcvars.bat execution failed with return code {result.returncode}")
                logger.error(f"stderr: {result.stderr}")
                raise RuntimeError(
                    f"Failed to execute vcvars.bat: {result.stderr}"
                )

            # Parse environment variables from output
            env_vars = {}
            for line in result.stdout.split('\n'):
                if '=' in line:
                    # Split only on first '=' to handle values containing '='
                    key, value = line.split('=', 1)
                    key = key.strip()
                    value = value.strip()
                    # Only add non-empty keys
                    if key:
                        env_vars[key] = value

            # Ensure PATH is always included (it's critical for MSVC)
            if 'PATH' not in env_vars:
                logger.warning("PATH not found in vcvars output, using current PATH")
                env_vars['PATH'] = os.environ.get('PATH', '')
            else:
                # Ensure PATH includes current PATH to maintain system paths
                current_path = os.environ.get('PATH', '')
                if current_path and current_path not in env_vars['PATH']:
                    env_vars['PATH'] = env_vars['PATH'] + os.pathsep + current_path

            logger.info(f"MSVC environment set up successfully for {arch}")
            logger.debug(f"Environment variables set: {list(env_vars.keys())}")

            return env_vars

        finally:
            # Clean up temporary file
            try:
                os.unlink(temp_bat)
            except Exception:
                pass

    except Exception as e:
        logger.error(f"Failed to set up MSVC environment: {e}")
        raise RuntimeError(
            f"Failed to set up MSVC environment: {e}"
        ) from e


def setup_mingw_environment(environment: str = 'UCRT64') -> Dict[str, str]:
    """Set up MSYS2 UCRT64 environment for MinGW-GCC and MinGW-Clang.

    This function locates MSYS2 installation automatically and sets up the
    required environment variables for MinGW compilation. It supports both
    UCRT64 and MSYS2 environments, and converts Windows paths to MSYS2 format.

    Args:
        environment: Environment type ('UCRT64' or 'MSYS2'). Defaults to 'UCRT64'.

    Returns:
        Dictionary of environment variables set up for MinGW.

    Raises:
        RuntimeError: If MSYS2 cannot be found or setup fails.

    Example:
        >>> env = setup_mingw_environment('UCRT64')
        >>> os.environ.update(env)
    """
    logger = get_logger(__name__)

    try:
        # Detect MinGW installation
        mingw_info = detect_mingw()
        if not mingw_info:
            raise RuntimeError(
                "MinGW not found. Please install MSYS2 from https://www.msys2.org/"
            )

        # Validate environment type
        environment = environment.upper()
        if environment not in ['UCRT64', 'MSYS2', 'MINGW64', 'CLANG64']:
            raise ValueError(
                f"Invalid environment type: {environment}. "
                f"Must be one of: UCRT64, MSYS2, MINGW64, CLANG64"
            )

        msys2_path = mingw_info.path
        logger.info(f"Setting up MinGW environment for {environment} using {msys2_path}")

        # Set up MSYS2 environment variables
        env_vars = {
            'MSYSTEM': environment,
            'MSYSTEM_PREFIX': str(msys2_path / environment.lower()),
            'MSYSTEM_CARCH': 'x86_64',
            'MINGW_PREFIX': str(msys2_path / environment.lower()),
            'MINGW_CHOST': 'x86_64-w64-mingw32',
        }

        # Build PATH for MinGW tools
        bin_paths = [
            msys2_path / environment.lower() / 'bin',
            msys2_path / 'usr' / 'bin',
            msys2_path / 'usr' / 'local' / 'bin',
        ]

        # Convert paths to Windows format and add to PATH
        path_entries = [str(p) for p in bin_paths if p.exists()]
        current_path = os.environ.get('PATH', '')
        env_vars['PATH'] = os.pathsep.join(path_entries + [current_path])

        logger.info(f"MinGW environment set up successfully for {environment}")
        logger.debug(f"Environment variables set: {list(env_vars.keys())}")

        return env_vars

    except Exception as e:
        logger.error(f"Failed to set up MinGW environment: {e}")
        raise RuntimeError(
            f"Failed to set up MinGW environment: {e}"
        ) from e


__all__ = [
    'MSVCInfo',
    'MinGWInfo',
    'detect_msvc',
    'detect_mingw',
    'detect_mingw_clang',
    'setup_msvc_environment',
    'setup_mingw_environment',
]
