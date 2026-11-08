"""
Conan package manager integration module for Warploom build system.

Provides Conan package installation, profile management,
and Conan integration with CMake.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional

from warploom_scripts.logging.logger import get_logger
from warploom_scripts.utils.command_utils import execute_command


logger = get_logger(__name__)


class ConanError(Exception):
    """Base exception for Conan-related errors."""

    def __init__(self, message: str, command: str, exit_code: int = 1) -> None:
        """Initialize Conan error.

        Args:
            message: Human-readable error message.
            command: The Conan command that failed.
            exit_code: The exit code to return.
        """
        self.message = message
        self.command = command
        self.exit_code = exit_code
        super().__init__(self.message)


class ConanWrapper:
    """Wrapper class for Conan operations.

    Provides high-level interface for Conan package management,
    profile creation, and CMake integration.
    """

    def __init__(
        self,
        conan_path: Optional[Path] = None,
        project_dir: Optional[Path] = None,
    ) -> None:
        """Initialize Conan wrapper.

        Args:
            conan_path: Path to Conan executable (default: conan from PATH).
            project_dir: Path to project directory (default: current directory).
        """
        self.conan_path = conan_path or Path("conan")
        self.project_dir = project_dir or Path.cwd()
        self.conan_dir = self.project_dir / "conan"

        logger.debug(
            f"ConanWrapper initialized: conan={self.conan_path}, "
            f"project={self.project_dir}"
        )

    def install(
        self,
        profile: str,
        build_type: str = "Release",
        conanfile_path: Optional[Path] = None,
        extra_args: Optional[List[str]] = None,
    ) -> int:
        """Install Conan dependencies.

        Args:
            profile: Conan profile to use.
            build_type: Build type (Debug, Release, RelWithDebInfo, MinSizeRel).
            conanfile_path: Path to conanfile.txt or conanfile.py.
            extra_args: Additional Conan arguments.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            ConanError: If Conan install fails.
        """
        # Check for system-wide Vulkan SDK installation
        # If VULKAN_SDK is set, use system SDK instead of Conan dependencies
        use_system_vulkan = os.environ.get("VULKAN_SDK") is not None
        if use_system_vulkan:
            logger.info("VULKAN_SDK environment variable detected.")
            logger.info("Using system-wide Vulkan SDK. Conan will skip Vulkan dependencies.")
            # Add option to disable Vulkan in Conan
            if extra_args is None:
                extra_args = []
            extra_args.extend(["-o", "warploom-template/*:with_vulkan=False"])
        else:
            logger.info("VULKAN_SDK not set. Conan will provide Vulkan SDK.")

        logger.info(f"Installing Conan dependencies with profile: {profile}")

        # Build Conan command
        cmd_parts = [str(self.conan_path), "install"]

        # Add conanfile path
        if conanfile_path:
            if not conanfile_path.exists():
                raise ConanError(
                    f"Conanfile not found: {conanfile_path}",
                    f"conan install {conanfile_path}",
                )
            cmd_parts.append(str(conanfile_path))
        else:
            # Use project directory
            cmd_parts.append(str(self.project_dir))

        # Add profile
        cmd_parts.extend(["--profile:build", profile])
        cmd_parts.extend(["--profile:host", profile])

        # Add build type
        cmd_parts.extend(["--settings", f"build_type={build_type}"])

        # Add output folder
        cmd_parts.extend(["--output-folder", str(self.conan_dir)])

        # Add extra arguments
        if extra_args:
            cmd_parts.extend(extra_args)

        cmd = " ".join(cmd_parts)
        logger.debug(f"Conan install command: {cmd}")
        try:
            execute_command(cmd, timeout=600)
            logger.info("Conan install completed successfully")
            return 0
        except Exception as e:
            error_msg = f"Conan install failed: {e}"
            logger.error(error_msg)
            raise ConanError(error_msg, cmd) from e

    def create_profile(
        self,
        profile_name: str,
        compiler: str,
        compiler_version: str,
        arch: str = "x86_64",
        build_type: str = "Release",
        os_name: Optional[str] = None,
        extra_settings: Optional[Dict[str, str]] = None,
    ) -> Path:
        """Create Conan profile.

        Args:
            profile_name: Name of the profile to create.
            compiler: Compiler name (gcc, clang, msvc, etc.).
            compiler_version: Compiler version.
            arch: Target architecture (x86_64, ARM64, etc.).
            build_type: Build type (Debug, Release, etc.).
            os_name: Operating system name (default: auto-detect).
            extra_settings: Additional Conan settings.

        Returns:
            Path to created profile file.

        Raises:
            ConanError: If profile creation fails.
        """
        logger.info(f"Creating Conan profile: {profile_name}")

        # Auto-detect OS if not specified
        if os_name is None:
            import platform
            system = platform.system().lower()
            if system == "windows":
                os_name = "Windows"
            elif system == "linux":
                os_name = "Linux"
            elif system == "darwin":
                os_name = "Macos"
            else:
                os_name = system

        # Build profile content
        profile_content = {
            "settings": {
                "os": os_name,
                "arch": arch,
                "compiler": compiler,
                "compiler.version": compiler_version,
                "build_type": build_type,
            },
            "buildenv": {},
            "conf": {},
        }

        # Add extra settings
        if extra_settings:
            profile_content["settings"].update(extra_settings)

        # Create profiles directory
        profiles_dir = self.conan_dir / "profiles"
        profiles_dir.mkdir(parents=True, exist_ok=True)

        # Write profile file
        profile_path = profiles_dir / profile_name
        try:
            with open(profile_path, "w", encoding="utf-8") as f:
                # Write in INI format
                f.write(f"[settings]\n")
                for key, value in profile_content["settings"].items():
                    f.write(f"{key}={value}\n")

                if profile_content["buildenv"]:
                    f.write(f"\n[buildenv]\n")
                    for key, value in profile_content["buildenv"].items():
                        f.write(f"{key}={value}\n")

                if profile_content["conf"]:
                    f.write(f"\n[conf]\n")
                    for key, value in profile_content["conf"].items():
                        f.write(f"{key}={value}\n")

            logger.info(f"Created Conan profile: {profile_path}")
            return profile_path
        except Exception as e:
            error_msg = f"Failed to create Conan profile: {e}"
            logger.error(error_msg)
            raise ConanError(error_msg, f"create_profile {profile_name}") from e

    def list_profiles(self) -> List[str]:
        """List available Conan profiles.

        Returns:
            List of profile names.
        """
        logger.info("Listing Conan profiles")

        profiles = []

        # Check project profiles directory
        profiles_dir = self.conan_dir / "profiles"
        if profiles_dir.exists():
            for profile_file in profiles_dir.glob("*"):
                if profile_file.is_file():
                    profiles.append(profile_file.name)

        # Check global Conan profiles
        try:
            result = subprocess.run(
                [str(self.conan_path), "profile", "list"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode == 0:
                # Parse output to extract profile names
                for line in result.stdout.split("\n"):
                    line = line.strip()
                    if line and not line.startswith("="):
                        profiles.append(line)
        except Exception as e:
            logger.warning(f"Failed to list global Conan profiles: {e}")

        # Remove duplicates
        profiles = list(set(profiles))
        logger.info(f"Found {len(profiles)} Conan profiles")
        return profiles

    def get_profile(self, profile_name: str) -> Optional[Dict[str, Any]]:
        """Get Conan profile by name.

        Args:
            profile_name: Name of the profile to retrieve.

        Returns:
            Profile dictionary if found, None otherwise.
        """
        logger.debug(f"Getting Conan profile: {profile_name}")

        # Check project profiles directory
        profiles_dir = self.conan_dir / "profiles"
        profile_path = profiles_dir / profile_name

        if profile_path.exists():
            try:
                with open(profile_path, "r", encoding="utf-8") as f:
                    content = f.read()
                    # Parse INI format
                    profile: Dict[str, Dict[str, str]] = {"settings": {}, "buildenv": {}, "conf": {}}
                    current_section = "settings"

                    for line in content.split("\n"):
                        line = line.strip()
                        if not line or line.startswith("#"):
                            continue

                        if line.startswith("["):
                            current_section = line[1:-1]
                            continue

                        if "=" in line:
                            key, value = line.split("=", 1)
                            profile[current_section][key.strip()] = value.strip()

                    return profile
            except Exception as e:
                logger.warning(f"Failed to read profile {profile_name}: {e}")

        return None

    def integrate_cmake(self) -> int:
        """Integrate Conan with CMake.

        Generates CMake toolchain and build information files.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            ConanError: If integration fails.
        """
        logger.info("Integrating Conan with CMake")

        # Build Conan command
        cmd_parts = [
            str(self.conan_path),
            "install",
            str(self.project_dir),
            "--output-folder",
            str(self.conan_dir),
            "--generator",
            "CMakeDeps",
            "--generator",
            "CMakeToolchain",
        ]

        cmd = " ".join(cmd_parts)
        logger.debug(f"Conan CMake integration command: {cmd}")

        try:
            execute_command(cmd, timeout=600)
            logger.info("Conan CMake integration completed successfully")
            return 0
        except Exception as e:
            error_msg = f"Conan CMake integration failed: {e}"
            logger.error(error_msg)
            raise ConanError(error_msg, cmd) from e

    def search(self, query: str) -> List[Dict[str, str]]:
        """Search for Conan packages.

        Args:
            query: Search query string.

        Returns:
            List of package dictionaries containing name and version.
        """
        logger.info(f"Searching Conan packages: {query}")

        packages = []

        try:
            result = subprocess.run(
                [str(self.conan_path), "search", query, "--remote"],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if result.returncode == 0:
                # Parse output to extract package information
                for line in result.stdout.split("\n"):
                    line = line.strip()
                    if "/" in line:
                        parts = line.split("/")
                        if len(parts) >= 2:
                            name = parts[0]
                            version = parts[1].split()[0]
                            packages.append({"name": name, "version": version})

            logger.info(f"Found {len(packages)} packages")
        except Exception as e:
            logger.warning(f"Failed to search Conan packages: {e}")

        return packages

    def remove_profile(self, profile_name: str) -> int:
        """Remove Conan profile.

        Args:
            profile_name: Name of the profile to remove.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        logger.info(f"Removing Conan profile: {profile_name}")

        # Check project profiles directory
        profiles_dir = self.conan_dir / "profiles"
        profile_path = profiles_dir / profile_name

        if profile_path.exists():
            try:
                profile_path.unlink()
                logger.info(f"Removed Conan profile: {profile_path}")
                return 0
            except Exception as e:
                error_msg = f"Failed to remove profile: {e}"
                logger.error(error_msg)
                return 1

        logger.warning(f"Profile not found: {profile_name}")
        return 1

    def get_version(self) -> Optional[str]:
        """Get Conan version.

        Returns:
            Conan version string if available, None otherwise.
        """
        try:
            result = subprocess.run(
                [str(self.conan_path), "--version"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode == 0:
                # Parse version from output (e.g., "Conan version 2.0.0")
                version_line = result.stdout.split("\n")[0]
                version = version_line.split()[-1]
                logger.debug(f"Conan version: {version}")
                return version
        except Exception as e:
            logger.warning(f"Failed to get Conan version: {e}")

        return None

    def validate_profile(self, profile_name: str) -> bool:
        """Validate Conan profile.

        Args:
            profile_name: Name of the profile to validate.

        Returns:
            True if profile is valid, False otherwise.
        """
        logger.debug(f"Validating Conan profile: {profile_name}")

        profile = self.get_profile(profile_name)
        if profile is None:
            logger.error(f"Profile not found: {profile_name}")
            return False

        # Check required settings
        required_settings = ["os", "arch", "compiler", "compiler.version", "build_type"]
        for setting in required_settings:
            if setting not in profile.get("settings", {}):
                logger.error(f"Profile missing required setting: {setting}")
                return False

        logger.debug(f"Profile validation passed: {profile_name}")
        return True


def conan_install(args: Dict[str, Any]) -> int:
    """Install Conan dependencies with given arguments.

    This function provides a command-line interface for Conan installation.

    Args:
        args: Dictionary containing installation arguments:
            - profile: Conan profile to use
            - build_type: Build type
            - conanfile_path: Path to conanfile
            - project_dir: Project directory path

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    try:
        wrapper = ConanWrapper(
            project_dir=Path(args.get("project_dir", ".")),
        )

        return wrapper.install(
            profile=args["profile"],
            build_type=args.get("build_type", "Release"),
            conanfile_path=Path(args["conanfile_path"]) if args.get("conanfile_path") else None,
        )
    except ConanError as e:
        logger.error(f"Conan install failed: {e.message}")
        return e.exit_code
    except Exception as e:
        logger.error(f"Unexpected error during Conan install: {e}")
        return 1


def conan_create_profile(args: Dict[str, Any]) -> Path:
    """Create Conan profile with given arguments.

    Args:
        args: Dictionary containing profile creation arguments:
            - profile_name: Name of the profile
            - compiler: Compiler name
            - compiler_version: Compiler version
            - arch: Target architecture
            - build_type: Build type
            - os_name: Operating system name

    Returns:
        Path to created profile file.
    """
    try:
        wrapper = ConanWrapper(
            project_dir=Path(args.get("project_dir", ".")),
        )

        return wrapper.create_profile(
            profile_name=args["profile_name"],
            compiler=args["compiler"],
            compiler_version=args["compiler_version"],
            arch=args.get("arch", "x86_64"),
            build_type=args.get("build_type", "Release"),
            os_name=args.get("os_name"),
        )
    except ConanError as e:
        logger.error(f"Conan profile creation failed: {e.message}")
        raise
    except Exception as e:
        logger.error(f"Unexpected error during Conan profile creation: {e}")
        raise


__all__ = [
    "ConanError",
    "ConanWrapper",
    "conan_install",
    "conan_create_profile",
]
