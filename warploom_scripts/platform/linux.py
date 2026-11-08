# warploom_scripts/platform/linux.py
"""
Linux-specific platform detection for OmniCPP project.

Provides Linux platform detection including GCC and Clang compiler detection,
and terminal environment setup for Linux compilers.
"""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional, Union

from warploom_scripts.logging.logger import get_logger


# Cache for distribution detection results
_distribution_cache: Optional[LinuxDistribution] = None
_package_manager_cache: Optional[PackageManager] = None
_nix_environment_cache: Optional[bool] = None
_nix_info_cache: Optional[NixInfo] = None


@dataclass
class LinuxDistribution:
    """Linux distribution information.

    Attributes:
        name: Distribution name (e.g., "Arch Linux", "Ubuntu 22.04").
        version: Distribution version string.
        family: Distribution family (arch, debian, fedora, suse, unknown).
        package_manager: Package manager name (pacman, apt, dnf, zypper, unknown).
        is_cachyos: True if CachyOS detected.
    """
    name: str
    version: str
    family: str
    package_manager: str
    is_cachyos: bool


@dataclass
class PackageManager:
    """Package manager information.

    Attributes:
        name: Package manager name (pacman, apt, dnf, zypper, unknown).
        command: Primary package manager command.
        family: Package manager family (arch, debian, fedora, suse, unknown).
    """
    name: str
    command: str
    family: str


@dataclass
class CachyOSInfo:
    """CachyOS-specific information.

    Attributes:
        version: CachyOS version (e.g., "2023.12.01", "2024.01.01").
        variant: CachyOS variant (e.g., "kde", "gnome", "base").
        kernel_version: Kernel version string.
        gcc_version: Default GCC version for this CachyOS release.
        supports_lto: Whether this version supports LTO optimizations.
        supports_native_optimizations: Whether this version supports -march=native.
    """
    version: str
    variant: str
    kernel_version: str
    gcc_version: str
    supports_lto: bool
    supports_native_optimizations: bool


@dataclass
class NixInfo:
    """Nix package manager environment information.

    Attributes:
        is_nix_environment: True if running in Nix shell.
        nix_version: Nix version string (e.g., "2.18.1").
        nix_store_path: Path to Nix store directory.
        nix_profiles: List of active Nix profile paths.
        is_flake_environment: True if running in Nix flake environment.
        in_nix_shell: Value of IN_NIX_SHELL environment variable.
    """
    is_nix_environment: bool
    nix_version: str
    nix_store_path: str
    nix_profiles: list[str]
    is_flake_environment: bool
    in_nix_shell: str


def detect_linux_distribution() -> LinuxDistribution:
    """Detect Linux distribution and version.

    This function reads /etc/os-release to obtain distribution information.
    It supports detection of major distributions: Arch Linux, Ubuntu, Fedora,
    Debian, CentOS, openSUSE, and CachyOS. The function handles
    missing or malformed /etc/os-release gracefully and caches results
    for performance.

    Returns:
        LinuxDistribution object containing detected distribution information.

    Example:
        >>> distro = detect_linux_distribution()
        >>> print(f"Distribution: {distro.name}, Family: {distro.family}")
    """
    global _distribution_cache

    if _distribution_cache is not None:
        return _distribution_cache

    logger = get_logger(__name__)
    os_release_path = Path("/etc/os-release")

    try:
        if not os_release_path.exists():
            logger.warning("/etc/os-release not found, returning unknown distribution")
            _distribution_cache = _get_unknown_distribution()
            return _distribution_cache

        with open(os_release_path, "r", encoding="utf-8") as f:
            content = f.read()

        distro_id = _parse_os_release_field(content, "ID")
        distro_pretty_name = _parse_os_release_field(content, "PRETTY_NAME")
        distro_name = _parse_os_release_field(content, "NAME")
        distro_version = _parse_os_release_field(content, "VERSION")
        distro_version_id = _parse_os_release_field(content, "VERSION_ID")
        distro_id_like = _parse_os_release_field(content, "ID_LIKE")

        # Check for CachyOS first (CachyOS is Arch-based but special)
        if distro_id == "cachyos":
            logger.info("Detected CachyOS")
            _distribution_cache = LinuxDistribution(
                name=distro_pretty_name or distro_name or "CachyOS",
                version=distro_version or distro_version_id or "",
                family="arch",
                package_manager="pacman",
                is_cachyos=True
            )
            logger.info(f"Distribution: {_distribution_cache.name} {_distribution_cache.version}")
            return _distribution_cache

        # Check for Arch Linux and derivatives
        if distro_id == "arch" or (distro_id_like and "arch" in distro_id_like.lower()):
            logger.info("Detected Arch Linux or derivative")
            _distribution_cache = LinuxDistribution(
                name=distro_pretty_name or distro_name or "Arch Linux",
                version=distro_version or "",
                family="arch",
                package_manager="pacman",
                is_cachyos=False
            )
            logger.info(f"Distribution: {_distribution_cache.name}")
            return _distribution_cache

        # Check for Debian-based distributions
        if distro_id in ("debian", "ubuntu") or (
            distro_id_like and "debian" in distro_id_like.lower()
        ):
            logger.info("Detected Debian-based distribution")
            distro_full_name = distro_pretty_name or distro_name or "Debian"
            # Extract version from PRETTY_NAME for Ubuntu
            if distro_id == "ubuntu" and distro_pretty_name:
                # Extract version from PRETTY_NAME (e.g., "Ubuntu 22.04.3 LTS" -> "22.04.3 LTS")
                distro_version_from_pretty = distro_pretty_name.replace("Ubuntu ", "", 1)
                distro_version = distro_version_from_pretty
            _distribution_cache = LinuxDistribution(
                name=distro_full_name,
                version=distro_version or distro_version_id or "",
                family="debian",
                package_manager="apt",
                is_cachyos=False
            )
            logger.info(f"Distribution: {_distribution_cache.name}")
            return _distribution_cache

        # Check for Fedora-based distributions
        if distro_id in ("fedora", "rhel", "centos") or (
            distro_id_like and "fedora" in distro_id_like.lower()
        ):
            logger.info("Detected Fedora-based distribution")
            distro_full_name = distro_pretty_name or distro_name or "Fedora"
            _distribution_cache = LinuxDistribution(
                name=distro_full_name,
                version=distro_version or distro_version_id or "",
                family="fedora",
                package_manager="dnf",
                is_cachyos=False
            )
            logger.info(f"Distribution: {_distribution_cache.name}")
            return _distribution_cache

        # Check for openSUSE
        if distro_id in ("opensuse", "suse") or (
            distro_id_like and "suse" in distro_id_like.lower()
        ):
            logger.info("Detected openSUSE distribution")
            distro_full_name = distro_pretty_name or distro_name or "openSUSE"
            # Append VERSION_ID to name if not already in PRETTY_NAME
            if distro_version_id and distro_pretty_name and distro_version_id not in distro_pretty_name:
                distro_full_name = f"{distro_full_name} {distro_version_id}"
            _distribution_cache = LinuxDistribution(
                name=distro_full_name,
                version=distro_version_id or distro_version or "",
                family="suse",
                package_manager="zypper",
                is_cachyos=False
            )
            logger.info(f"Distribution: {_distribution_cache.name}")
            return _distribution_cache

        # Unknown distribution
        logger.warning(f"Unknown distribution ID: {distro_id}")
        _distribution_cache = LinuxDistribution(
            name=distro_pretty_name or distro_name or "Unknown",
            version=distro_version or "",
            family="unknown",
            package_manager="unknown",
            is_cachyos=False
        )
        logger.info(f"Distribution: {_distribution_cache.name}")
        return _distribution_cache

    except PermissionError as e:
        logger.error(f"Permission denied reading /etc/os-release: {e}")
        _distribution_cache = _get_unknown_distribution()
        return _distribution_cache
    except Exception as e:
        logger.error(f"Error detecting Linux distribution: {e}")
        _distribution_cache = _get_unknown_distribution()
        return _distribution_cache


def _get_unknown_distribution() -> LinuxDistribution:
    """Get unknown distribution object.

    Returns:
        LinuxDistribution object with unknown values.
    """
    return LinuxDistribution(
        name="Unknown",
        version="",
        family="unknown",
        package_manager="unknown",
        is_cachyos=False
    )


def _parse_os_release_field(content: str, field: str) -> Optional[str]:
    """Parse a field from /etc/os-release content.

    Args:
        content: Content of /etc/os-release file.
        field: Field name to parse (e.g., "ID", "NAME", "VERSION").

    Returns:
        Field value if found, None otherwise.
    """
    # Pattern matches: "quoted" OR 'quoted' OR unquoted (until end of line)
    # [^\n"]* matches anything except newlines and double quotes
    # [^\n']* matches anything except newlines and single quotes
    pattern = rf'^{field}=("[^\n"]*"|\'[^\n\']*\'|[^"\n\']*)$'
    match = re.search(pattern, content, re.MULTILINE)
    if match:
        value = match.group(1)
        # Strip surrounding quotes if present
        if value and len(value) >= 2 and ((value[0] == '"' and value[-1] == '"') or (value[0] == "'" and value[-1] == "'")):
            return value[1:-1]
        return value
    return None


def is_cachyos() -> bool:
    """Check if running on CachyOS.

    This function checks if the current system is CachyOS by examining
    the distribution information. CachyOS is detected before generic Arch
    Linux to ensure proper identification.

    Returns:
        True if CachyOS is detected, False otherwise.

    Example:
        >>> if is_cachyos():
        ...     print("Running on CachyOS")
    """
    distro = detect_linux_distribution()
    return distro.is_cachyos


def get_cachyos_info() -> Optional[CachyOSInfo]:
    """Get detailed CachyOS-specific information.

    This function retrieves detailed CachyOS information including version,
    variant, kernel version, and optimization capabilities. It parses
    the CachyOS version string and determines which optimizations
    are supported based on the version.

    Returns:
        CachyOSInfo object if running on CachyOS, None otherwise.

    Example:
        >>> info = get_cachyos_info()
        >>> if info:
        ...     print(f"CachyOS {info.version} ({info.variant})")
    """
    logger = get_logger(__name__)

    distro = detect_linux_distribution()
    if not distro.is_cachyos:
        return None

    try:
        # Parse CachyOS version
        version = distro.version
        variant = _parse_cachyos_variant(version)

        # Get kernel version
        kernel_version = _get_kernel_version()

        # Determine default GCC version based on CachyOS release
        gcc_version = _get_cachyos_default_gcc_version(version)

        # Determine optimization support based on version
        supports_lto, supports_native = _get_cachyos_optimization_support(version)

        cachyos_info = CachyOSInfo(
            version=version,
            variant=variant,
            kernel_version=kernel_version,
            gcc_version=gcc_version,
            supports_lto=supports_lto,
            supports_native_optimizations=supports_native
        )

        logger.info(
            f"CachyOS {version} ({variant}) detected - "
            f"Kernel: {kernel_version}, GCC: {gcc_version}, "
            f"LTO: {supports_lto}, Native: {supports_native}"
        )

        return cachyos_info

    except Exception as e:
        logger.error(f"Failed to get CachyOS info: {e}")
        return None


def _parse_cachyos_variant(version: str) -> str:
    """Parse CachyOS variant from version string.

    Args:
        version: CachyOS version string.

    Returns:
        CachyOS variant (kde, gnome, base, or unknown).
    """
    version_lower = version.lower()

    if "kde" in version_lower:
        return "kde"
    elif "gnome" in version_lower:
        return "gnome"
    elif "base" in version_lower or "minimal" in version_lower:
        return "base"
    else:
        return "unknown"


def _get_kernel_version() -> str:
    """Get the current kernel version.

    Returns:
        Kernel version string.
    """
    logger = get_logger(__name__)

    try:
        result = subprocess.run(
            ["uname", "-r"],
            capture_output=True,
            text=True,
            timeout=5
        )

        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()

        logger.warning("Could not determine kernel version")
        return "unknown"

    except Exception as e:
        logger.warning(f"Failed to get kernel version: {e}")
        return "unknown"


def _get_cachyos_default_gcc_version(version: str) -> str:
    """Get the default GCC version for a CachyOS release.

    CachyOS uses specific GCC versions for different releases:
    - 2024.x.x releases: GCC 14
    - 2023.x.x releases: GCC 13
    - Earlier releases: GCC 12

    Args:
        version: CachyOS version string.

    Returns:
        Default GCC version string.
    """
    try:
        # Extract year from version (e.g., "2023.12.01" -> 2023)
        year_match = re.search(r"(\d{4})", version)
        if year_match:
            year = int(year_match.group(1))

            if year >= 2024:
                return "14.2.1"
            elif year >= 2023:
                return "13.2.1"
            else:
                return "12.2.0"

        return "13.2.1"  # Default to GCC 13

    except Exception:
        return "13.2.1"  # Default to GCC 13 on error


def _get_cachyos_optimization_support(version: str) -> tuple[bool, bool]:
    """Determine optimization support for a CachyOS version.

    Args:
        version: CachyOS version string.

    Returns:
        Tuple of (supports_lto, supports_native_optimizations).
    """
    try:
        # Extract year from version
        year_match = re.search(r"(\d{4})", version)
        if year_match:
            year = int(year_match.group(1))

            # All modern CachyOS versions support LTO and native optimizations
            if year >= 2022:
                return True, True

        return True, True  # Assume support if version parsing fails

    except Exception:
        return True, True  # Assume support on error


def get_cachyos_compiler_flags(compiler: str, build_type: str) -> list[str]:
    """Get CachyOS-specific compiler flags.

    This function returns CachyOS-specific compiler flags optimized for
    the CachyOS platform. It includes performance optimizations,
    security hardening, and build-type specific flags.

    Args:
        compiler: Compiler type ('gcc' or 'clang').
        build_type: Build type ('debug' or 'release').

    Returns:
        List of compiler flags.

    Raises:
        ValueError: If compiler or build_type is invalid.

    Example:
        >>> flags = get_cachyos_compiler_flags('gcc', 'release')
        >>> print(' '.join(flags))
    """
    logger = get_logger(__name__)

    # Validate inputs
    compiler = compiler.lower()
    build_type = build_type.lower()

    if compiler not in ('gcc', 'clang'):
        raise ValueError(
            f"Invalid compiler: {compiler}. Must be 'gcc' or 'clang'."
        )

    if build_type not in ('debug', 'release'):
        raise ValueError(
            f"Invalid build type: {build_type}. Must be 'debug' or 'release'."
        )

    flags: list[str] = []

    # Add build-type specific flags
    if build_type == 'release':
        # CachyOS performance optimizations for release builds
        flags.extend([
            '-march=native',  # Use native CPU features
            '-O3',  # Maximum optimization
            '-flto',  # Link-time optimization
            '-DNDEBUG'  # Disable debug assertions
        ])
        logger.debug("Added release build flags: -march=native -O3 -flto -DNDEBUG")
    else:
        # Debug build flags
        flags.extend([
            '-g',  # Generate debug information
            '-O0',  # No optimization
            '-DDEBUG'  # Enable debug macros
        ])
        logger.debug("Added debug build flags: -g -O0 -DDEBUG")

    # Add compiler-specific security flags
    if compiler == 'gcc':
        flags.extend([
            '-fstack-protector-strong',  # Stack protection
            '-D_FORTIFY_SOURCE=2'  # Buffer overflow protection
        ])
        logger.debug("Added GCC security flags: -fstack-protector-strong -D_FORTIFY_SOURCE=2")
    elif compiler == 'clang':
        flags.extend([
            '-fstack-protector-strong',  # Stack protection
            '-D_FORTIFY_SOURCE=2'  # Buffer overflow protection
        ])
        logger.debug("Added Clang security flags: -fstack-protector-strong -D_FORTIFY_SOURCE=2")

    # Add CachyOS-specific warnings
    flags.extend([
        '-Wall',  # Enable all warnings
        '-Wextra',  # Enable extra warnings
        '-Wpedantic'  # Enable pedantic warnings
    ])

    logger.info(f"Returning {len(flags)} compiler flags for {compiler} {build_type} build")
    return flags


def get_cachyos_linker_flags() -> list[str]:
    """Get CachyOS-specific linker flags.

    This function returns CachyOS-specific linker flags optimized for
    the CachyOS platform. These flags improve security and
    performance of linked binaries.

    Returns:
        List of linker flags.

    Example:
        >>> flags = get_cachyos_linker_flags()
        >>> print(' '.join(flags))
    """
    logger = get_logger(__name__)

    flags = [
        '-Wl,--as-needed',  # Only link libraries that are actually used
        '-Wl,--no-undefined',  # Error on undefined symbols
        '-Wl,-z,relro',  # Read-only relocations
        '-Wl,-z,now'  # Immediate binding
    ]

    logger.debug(f"Returning {len(flags)} linker flags")
    return flags


def get_cachyos_package_manager_config() -> dict[str, str]:
    """Get CachyOS-specific package manager configuration.

    This function returns CachyOS-specific package manager configuration
    for pacman, including repository URLs and optimization options.

    Returns:
        Dictionary of package manager configuration.

    Example:
        >>> config = get_cachyos_package_manager_config()
        >>> print(config['pacman_conf'])
    """
    logger = get_logger(__name__)

    config: dict[str, str] = {
        'package_manager': 'pacman',
        'pacman_conf': '/etc/pacman.conf',
        'mirrorlist': '/etc/pacman.d/mirrorlist',
        'cache_dir': '/var/cache/pacman/pkg',
        'database_dir': '/var/lib/pacman',
        'log_file': '/var/log/pacman.log',
        'use_color': 'always',
        'verbose_pkg_lists': 'true',
        'check_space': 'true',
        'parallel_downloads': '5'
    }

    logger.info("Returning CachyOS package manager configuration")
    return config


def is_nix_environment() -> bool:
    """Check if running in a Nix shell environment.

    This function detects if the current process is running within a Nix shell
    by checking for the IN_NIX_SHELL environment variable and other Nix-specific
    environment variables. The result is cached for performance.

    Returns:
        True if running in a Nix shell environment, False otherwise.

    Example:
        >>> if is_nix_environment():
        ...     print("Running in Nix shell")
    """
    global _nix_environment_cache

    if _nix_environment_cache is not None:
        return _nix_environment_cache

    logger = get_logger(__name__)

    # Check for IN_NIX_SHELL environment variable (primary indicator)
    in_nix_shell = os.environ.get('IN_NIX_SHELL')

    # Check for NIX_PATH environment variable (secondary indicator)
    nix_path = os.environ.get('NIX_PATH')

    # Check for NIX_PROFILES environment variable (tertiary indicator)
    nix_profiles = os.environ.get('NIX_PROFILES')

    # Consider it a Nix environment if any indicator is present
    is_nix = in_nix_shell is not None or nix_path is not None or nix_profiles is not None

    _nix_environment_cache = is_nix

    if is_nix:
        logger.info(
            f"Detected Nix environment: IN_NIX_SHELL={in_nix_shell}, "
            f"NIX_PATH={'present' if nix_path else 'absent'}, "
            f"NIX_PROFILES={'present' if nix_profiles else 'absent'}"
        )
    else:
        logger.debug("Not running in Nix environment")

    return is_nix


def get_nix_info() -> Optional[NixInfo]:
    """Get detailed Nix environment information.

    This function retrieves comprehensive information about the Nix environment
    including Nix version, store path, active profiles, and whether
    running in a flake environment. The result is cached for performance.

    Returns:
        NixInfo object if running in Nix environment, None otherwise.

    Example:
        >>> info = get_nix_info()
        >>> if info:
        ...     print(f"Nix version: {info.nix_version}")
    """
    global _nix_info_cache

    if _nix_info_cache is not None:
        return _nix_info_cache

    logger = get_logger(__name__)

    if not is_nix_environment():
        logger.debug("Not in Nix environment, returning None")
        return None

    try:
        # Get Nix version
        nix_version = _get_nix_version()

        # Get Nix store path
        nix_store_path = _get_nix_store_path()

        # Get Nix profiles
        nix_profiles = _get_nix_profiles()

        # Check if running in flake environment
        is_flake = _is_nix_flake_environment()

        # Get IN_NIX_SHELL value
        in_nix_shell = os.environ.get('IN_NIX_SHELL', '')

        nix_info = NixInfo(
            is_nix_environment=True,
            nix_version=nix_version,
            nix_store_path=nix_store_path,
            nix_profiles=nix_profiles,
            is_flake_environment=is_flake,
            in_nix_shell=in_nix_shell
        )

        logger.info(
            f"Nix environment info: version={nix_version}, "
            f"store={nix_store_path}, flake={is_flake}"
        )

        _nix_info_cache = nix_info
        return nix_info

    except Exception as e:
        logger.error(f"Failed to get Nix info: {e}")
        return None


def _get_nix_version() -> str:
    """Get Nix version string.

    This function retrieves the Nix version by running the 'nix --version'
    command. It handles command execution failures gracefully.

    Returns:
        Nix version string, or "unknown" if detection fails.

    Example:
        >>> version = _get_nix_version()
        >>> print(f"Nix version: {version}")
    """
    logger = get_logger(__name__)

    try:
        result = subprocess.run(
            ["nix", "--version"],
            capture_output=True,
            text=True,
            timeout=5
        )

        if result.returncode == 0 and result.stdout.strip():
            # Parse version from output (format: "nix (Nix) 2.18.1")
            match = re.search(r'nix.*?(\d+\.\d+\.\d+)', result.stdout)
            if match:
                version = match.group(1)
                logger.debug(f"Detected Nix version: {version}")
                return version

        logger.warning("Could not determine Nix version from output")
        return "unknown"

    except FileNotFoundError:
        logger.debug("nix command not found, assuming unknown version")
        return "unknown"
    except subprocess.TimeoutExpired:
        logger.warning("Nix version command timed out")
        return "unknown"
    except Exception as e:
        logger.warning(f"Failed to get Nix version: {e}")
        return "unknown"


def _get_nix_store_path() -> str:
    """Get the Nix store path.

    This function retrieves the Nix store path by checking the NIX_STORE_DIR
    environment variable or using the default /nix/store path.

    Returns:
        Nix store path string.

    Example:
        >>> store_path = _get_nix_store_path()
        >>> print(f"Nix store: {store_path}")
    """
    logger = get_logger(__name__)

    # Check for NIX_STORE_DIR environment variable
    nix_store_dir = os.environ.get('NIX_STORE_DIR')

    if nix_store_dir:
        logger.debug(f"Using NIX_STORE_DIR: {nix_store_dir}")
        return nix_store_dir

    # Default Nix store path
    default_store = "/nix/store"
    logger.debug(f"Using default Nix store path: {default_store}")
    return default_store


def _get_nix_profiles() -> list[str]:
    """Get list of active Nix profiles.

    This function retrieves the list of active Nix profiles by parsing the
    NIX_PROFILES environment variable.

    Returns:
        List of Nix profile paths.

    Example:
        >>> profiles = _get_nix_profiles()
        >>> print(f"Active profiles: {profiles}")
    """
    logger = get_logger(__name__)

    nix_profiles_env = os.environ.get('NIX_PROFILES', '')

    if not nix_profiles_env:
        logger.debug("No NIX_PROFILES environment variable found")
        return []

    # Parse NIX_PROFILES (colon-separated paths)
    profiles = [p.strip() for p in nix_profiles_env.split(':') if p.strip()]

    logger.debug(f"Detected {len(profiles)} Nix profile(s)")
    return profiles


def _is_nix_flake_environment() -> bool:
    """Check if running in a Nix flake environment.

    This function detects if the current environment was created using Nix flakes
    by checking for flake-specific environment variables and paths.

    Returns:
        True if running in a Nix flake environment, False otherwise.

    Example:
        >>> if _is_nix_flake_environment():
        ...     print("Running in Nix flake environment")
    """
    logger = get_logger(__name__)

    try:
        # Check for flake-specific environment variable
        # When using flakes, Nix sets IN_NIX_SHELL to 'pure' or 'impure'
        in_nix_shell = os.environ.get('IN_NIX_SHELL', '')

        # Check for flake-specific path patterns in PATH
        path = os.environ.get('PATH', '')
        is_flake = '/.nix-profile/' in path or '/nix/store/' in path

        # Additional check: look for .direnv/flake.nix or similar
        current_dir = os.getcwd()
        has_flake_file = (
            os.path.exists(os.path.join(current_dir, 'flake.nix')) or
            os.path.exists(os.path.join(current_dir, '.envrc'))
        )

        # Consider it a flake environment if any indicator is present
        result = is_flake or has_flake_file

        if result:
            logger.debug("Detected Nix flake environment")
        else:
            logger.debug("Not in Nix flake environment")

        return result

    except Exception as e:
        logger.warning(f"Failed to detect Nix flake environment: {e}")
        return False


def detect_package_manager() -> PackageManager:
    """Detect system package manager.

    This function detects the system package manager by checking for
    available package manager executables. It handles multiple package
    managers present by returning the first match and caches results
    for performance.

    Returns:
        PackageManager object containing detected package manager information.

    Example:
        >>> pm = detect_package_manager()
        >>> print(f"Package manager: {pm.name}, Command: {pm.command}")
    """
    global _package_manager_cache

    if _package_manager_cache is not None:
        return _package_manager_cache

    logger = get_logger(__name__)

    # Check for pacman (Arch Linux family)
    pacman_path = _find_executable("pacman")
    if pacman_path:
        logger.info("Detected pacman package manager")
        _package_manager_cache = PackageManager(
            name="pacman",
            command="pacman",
            family="arch"
        )
        return _package_manager_cache

    # Check for apt (Debian family)
    apt_path = _find_executable("apt")
    if apt_path:
        logger.info("Detected apt package manager")
        _package_manager_cache = PackageManager(
            name="apt",
            command="apt",
            family="debian"
        )
        return _package_manager_cache

    # Check for apt-get (Debian family, older systems)
    apt_get_path = _find_executable("apt-get")
    if apt_get_path:
        logger.info("Detected apt-get package manager")
        _package_manager_cache = PackageManager(
            name="apt",
            command="apt-get",
            family="debian"
        )
        return _package_manager_cache

    # Check for dnf (Fedora family)
    dnf_path = _find_executable("dnf")
    if dnf_path:
        logger.info("Detected dnf package manager")
        _package_manager_cache = PackageManager(
            name="dnf",
            command="dnf",
            family="fedora"
        )
        return _package_manager_cache

    # Check for yum (Fedora family, older systems)
    yum_path = _find_executable("yum")
    if yum_path:
        logger.info("Detected yum package manager")
        _package_manager_cache = PackageManager(
            name="dnf",
            command="yum",
            family="fedora"
        )
        return _package_manager_cache

    # Check for zypper (openSUSE family)
    zypper_path = _find_executable("zypper")
    if zypper_path:
        logger.info("Detected zypper package manager")
        _package_manager_cache = PackageManager(
            name="zypper",
            command="zypper",
            family="suse"
        )
        return _package_manager_cache

    # No package manager found
    logger.warning("No package manager detected")
    _package_manager_cache = PackageManager(
        name="unknown",
        command="",
        family="unknown"
    )
    return _package_manager_cache


@dataclass
class GCCInfo:
    """Information about GCC installation.
    
    Attributes:
        version: GCC version (e.g., '13.2.0')
        path: Path to gcc executable
        gxx_path: Path to g++ executable
        supports_cpp23: Whether GCC supports C++23
    """
    version: str
    path: Path
    gxx_path: Path
    supports_cpp23: bool


@dataclass
class ClangInfo:
    """Information about Clang installation.
    
    Attributes:
        version: Clang version (e.g., '18.1.3')
        path: Path to clang executable
        clangxx_path: Path to clang++ executable
        supports_cpp23: Whether Clang supports C++23
    """
    version: str
    path: Path
    clangxx_path: Path
    supports_cpp23: bool


def detect_gcc() -> Optional[GCCInfo]:
    """Detect GCC compiler installation on Linux.
    
    This function searches for GCC compiler installation and validates
    compiler availability. It checks for gcc and g++ executables and
    determines the GCC version.
    
    Returns:
        GCCInfo object if GCC is found, None otherwise
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
        
        # Check C++23 support
        supports_cpp23 = _check_gcc_cpp23_support(gcc_path, version)
        
        gcc_info = GCCInfo(
            version=version,
            path=gcc_path,
            gxx_path=gxx_path,
            supports_cpp23=supports_cpp23
        )
        
        logger.info(
            f"Found GCC {version} at {gcc_path} "
            f"(C++23: {'Yes' if supports_cpp23 else 'No'})"
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


def _check_gcc_cpp23_support(gcc_path: Path, version: str) -> bool:
    """Check if GCC supports C++23.
    
    Args:
        gcc_path: Path to gcc executable
        version: GCC version string
        
    Returns:
        True if GCC supports C++23, False otherwise
    """
    logger = get_logger(__name__)
    
    try:
        # GCC 13+ has good C++23 support
        major_version = int(version.split(".")[0])
        if major_version >= 13:
            return True
        
        # Try to compile a simple C++23 test
        test_code = """
        #include <version>
        #if __cpp_modules >= 202207L
        int main() { return 0; }
        #else
        #error "C++23 not supported"
        #endif
        """
        
        result = subprocess.run(
            [str(gcc_path), "-std=c++23", "-x", "c++", "-o", "/dev/null", "-"],
            input=test_code,
            capture_output=True,
            text=True,
            timeout=10
        )
        
        return result.returncode == 0
        
    except Exception as e:
        logger.warning(f"Failed to check GCC C++23 support: {e}")
        return False


def detect_clang() -> Optional[ClangInfo]:
    """Detect Clang compiler installation on Linux.
    
    This function searches for Clang compiler installation and validates
    compiler availability. It checks for clang and clang++ executables and
    determines the Clang version.
    
    Returns:
        ClangInfo object if Clang is found, None otherwise
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
        
        # Check C++23 support
        supports_cpp23 = _check_clang_cpp23_support(clang_path, version)
        
        clang_info = ClangInfo(
            version=version,
            path=clang_path,
            clangxx_path=clangxx_path,
            supports_cpp23=supports_cpp23
        )
        
        logger.info(
            f"Found Clang {version} at {clang_path} "
            f"(C++23: {'Yes' if supports_cpp23 else 'No'})"
        )
        
        return clang_info
        
    except Exception as e:
        logger.error(f"Clang detection failed: {e}")
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


def _check_clang_cpp23_support(clang_path: Path, version: str) -> bool:
    """Check if Clang supports C++23.
    
    Args:
        clang_path: Path to clang executable
        version: Clang version string
        
    Returns:
        True if Clang supports C++23, False otherwise
    """
    logger = get_logger(__name__)
    
    try:
        # Clang 16+ has good C++23 support
        major_version = int(version.split(".")[0])
        if major_version >= 16:
            return True
        
        # Try to compile a simple C++23 test
        test_code = """
        #include <version>
        #if __cpp_modules >= 202207L
        int main() { return 0; }
        #else
        #error "C++23 not supported"
        #endif
        """
        
        result = subprocess.run(
            [str(clang_path), "-std=c++23", "-x", "c++", "-o", "/dev/null", "-"],
            input=test_code,
            capture_output=True,
            text=True,
            timeout=10
        )
        
        return result.returncode == 0
        
    except Exception as e:
        logger.warning(f"Failed to check Clang C++23 support: {e}")
        return False


def detect_system_compilers() -> Dict[str, Optional[Union[GCCInfo, ClangInfo]]]:
    """Detect all available compilers on Linux.
    
    This function detects GCC and Clang compilers and returns
    information about available compilers.
    
    Returns:
        Dictionary with compiler information
    """
    logger = get_logger(__name__)
    
    compilers: Dict[str, Optional[Union[GCCInfo, ClangInfo]]] = {
        "gcc": None,
        "clang": None
    }
    
    # Detect GCC
    gcc_info = detect_gcc()
    if gcc_info:
        compilers["gcc"] = gcc_info
    
    # Detect Clang
    clang_info = detect_clang()
    if clang_info:
        compilers["clang"] = clang_info
    
    # Log summary
    available = [name for name, info in compilers.items() if info is not None]
    if available:
        logger.info(f"Available compilers: {', '.join(available)}")
    else:
        logger.warning("No compilers found")
    
    return compilers


def setup_linux_environment(compiler: str = 'gcc') -> Dict[str, str]:
    """Set up appropriate environment for Linux compilers.
    
    This function sets up the environment for Linux compilers (GCC or Clang).
    It preserves existing environment variables, adds compiler-specific paths
    to PATH if needed, supports custom environment variable overrides, and
    validates environment setup before proceeding.
    
    Args:
        compiler: The compiler to set up ('gcc' or 'clang'). Defaults to 'gcc'.
        
    Returns:
        Dictionary of environment variables set up for the specified compiler.
        
    Raises:
        RuntimeError: If the specified compiler is not found or setup fails.
        ValueError: If an invalid compiler is specified.
        
    Example:
        >>> env = setup_linux_environment('gcc')
        >>> os.environ.update(env)
    """
    logger = get_logger(__name__)
    
    try:
        # Validate compiler type
        compiler = compiler.lower()
        if compiler not in ['gcc', 'clang']:
            raise ValueError(
                f"Invalid compiler: {compiler}. Must be 'gcc' or 'clang'."
            )
        
        logger.info(f"Setting up Linux environment for {compiler}")
        
        # Start with current environment variables
        env_vars = os.environ.copy()
        
        # Detect compiler and validate availability
        if compiler == 'gcc':
            gcc_info = detect_gcc()
            if not gcc_info:
                raise RuntimeError(
                    "GCC not found. Please install GCC using your package manager."
                )
            
            # Set CC and CXX environment variables
            env_vars['CC'] = str(gcc_info.path)
            env_vars['CXX'] = str(gcc_info.gxx_path)
            
            logger.info(f"GCC {gcc_info.version} found at {gcc_info.path}")
            
        elif compiler == 'clang':
            clang_info = detect_clang()
            if not clang_info:
                raise RuntimeError(
                    "Clang not found. Please install Clang using your package manager."
                )
            
            # Set CC and CXX environment variables
            env_vars['CC'] = str(clang_info.path)
            env_vars['CXX'] = str(clang_info.clangxx_path)
            
            logger.info(f"Clang {clang_info.version} found at {clang_info.path}")
        
        # Validate environment setup
        _validate_linux_environment(env_vars, compiler)
        
        logger.info(f"Linux environment set up successfully for {compiler}")
        logger.debug(f"Environment variables set: CC={env_vars.get('CC')}, CXX={env_vars.get('CXX')}")
        
        return env_vars
        
    except Exception as e:
        logger.error(f"Failed to set up Linux environment: {e}")
        raise RuntimeError(
            f"Failed to set up Linux environment: {e}"
        ) from e


def _validate_linux_environment(env_vars: Dict[str, str], compiler: str) -> None:
    """Validate Linux environment setup before proceeding.
    
    This function verifies that required environment variables are set,
    checks that compiler executables are accessible, validates PATH configuration,
    and tests compiler invocation with a simple command.
    
    Args:
        env_vars: Dictionary of environment variables to validate.
        compiler: The compiler being validated ('gcc' or 'clang').
        
    Raises:
        RuntimeError: If validation fails.
    """
    logger = get_logger(__name__)
    
    try:
        # Check required environment variables
        required_vars = ['CC', 'CXX']
        missing_vars = [var for var in required_vars if var not in env_vars]
        
        if missing_vars:
            raise RuntimeError(
                f"Missing required environment variables: {', '.join(missing_vars)}"
            )
        
        # Check compiler executables are accessible
        cc_path = Path(env_vars['CC'])
        cxx_path = Path(env_vars['CXX'])
        
        if not cc_path.exists():
            raise RuntimeError(
                f"CC compiler not found: {cc_path}"
            )
        
        if not cxx_path.exists():
            raise RuntimeError(
                f"CXX compiler not found: {cxx_path}"
            )
        
        # Validate PATH configuration
        path = env_vars.get('PATH', '')
        if not path:
            raise RuntimeError(
                "PATH environment variable is not set"
            )
        
        # Test compiler invocation with simple command
        try:
            result = subprocess.run(
                [str(cc_path), '--version'],
                capture_output=True,
                text=True,
                timeout=5,
                env=env_vars
            )
            
            if result.returncode != 0:
                raise RuntimeError(
                    f"Compiler invocation failed: {result.stderr}"
                )
            
            logger.debug(f"Compiler validation successful: {result.stdout.split()[0:3]}")
            
        except subprocess.TimeoutExpired:
            raise RuntimeError(
                f"Compiler invocation timed out"
            )
        except FileNotFoundError:
            raise RuntimeError(
                f"Compiler executable not found: {cc_path}"
            )
        
        logger.info(f"Linux environment validation successful for {compiler}")
        
    except Exception as e:
        logger.error(f"Linux environment validation failed: {e}")
        raise RuntimeError(
            f"Linux environment validation failed: {e}"
        ) from e


__all__ = [
    'LinuxDistribution',
    'PackageManager',
    'CachyOSInfo',
    'NixInfo',
    'GCCInfo',
    'ClangInfo',
    'detect_linux_distribution',
    'is_cachyos',
    'get_cachyos_info',
    'detect_package_manager',
    'detect_gcc',
    'detect_clang',
    'detect_system_compilers',
    'setup_linux_environment',
    'get_cachyos_compiler_flags',
    'get_cachyos_linker_flags',
    'get_cachyos_package_manager_config',
    'is_nix_environment',
    'get_nix_info',
]
