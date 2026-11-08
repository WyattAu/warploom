"""
Warploom Conan Module.

This module handles Conan dependency management for Warploom projects.
It provides methods for installing dependencies and validating
Conan installations.

Classes:
    ConanError: Base exception for Conan-related errors.
    ConanProfileError: Exception for Conan profile issues.
    ConanInstallError: Exception for Conan installation failures.
    ConanManager: Main class managing Conan operations.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import List, Optional

from .utils import (
    CommandExecutionError,
    run_command,
    NotADirectoryError,
    execute_command,
    log_error,
    log_info,
    log_success,
)
from .utils.terminal_utils import TerminalEnvironment


class ConanError(Exception):
    """Base exception for Conan-related errors."""

    def __init__(
        self,
        message: str,
        command: Optional[str] = None,
    ) -> None:
        """Initialize Conan error.

        Args:
            message: Error message describing the issue.
            command: Optional Conan command that failed.
        """
        self.command = command
        super().__init__(message)


class ConanProfileError(ConanError):
    """Exception raised when Conan profile is not found or invalid.

    This exception is raised when a required Conan profile
    cannot be found or contains invalid configuration.
    """

    def __init__(
        self,
        message: str,
        profile: Optional[str] = None,
        profile_path: Optional[Path] = None,
    ) -> None:
        """Initialize profile error.

        Args:
            message: Error message describing the issue.
            profile: Optional profile name.
            profile_path: Optional profile file path.
        """
        self.profile = profile
        self.profile_path = profile_path
        super().__init__(message)


class ConanInstallError(ConanError):
    """Exception raised when Conan installation fails.

    This exception is raised when Conan dependency installation
    encounters errors such as missing packages, network issues,
    or configuration problems.
    """

    def __init__(
        self,
        message: str,
        build_dir: Optional[Path] = None,
        profile: Optional[str] = None,
    ) -> None:
        """Initialize installation error.

        Args:
            message: Error message describing the issue.
            build_dir: Optional build directory path.
            profile: Optional profile name being used.
        """
        self.build_dir = build_dir
        self.profile = profile
        super().__init__(message)


class ConanManager:
    """Manages Conan dependency operations.

    This class provides methods for installing Conan dependencies
    and validating Conan installations.

    Attributes:
        workspace_dir: The root workspace directory.
        conan_dir: The Conan configuration directory.
    """

    def __init__(self, workspace_dir: Optional[Path] = None) -> None:
        """Initialize Conan manager.

        Args:
            workspace_dir: The root workspace directory.

        Raises:
            NotADirectoryError: If workspace_dir is not a valid directory.
        """
        workspace_dir = workspace_dir or Path.cwd()
        if not workspace_dir.is_dir():
            raise NotADirectoryError(
                f"Workspace directory does not exist: {workspace_dir}",
                workspace_dir,
            )

        self.workspace_dir = workspace_dir
        self.conan_dir = workspace_dir / "conan"

    def install(
        self,
        build_dir: Optional[Path] = None,
        profile: str = "default",
        build_type: str = "debug",
        is_cross_compilation: bool = False,
        terminal_env: Optional[TerminalEnvironment] = None,
        source_dir: Optional[Path] = None,
        settings: Optional[dict] = None,
    ) -> None:
        """Install Conan dependencies for specified target.

        This method installs Conan dependencies using the specified
        profile and build configuration. It supports cross-compilation
        scenarios.

        Args:
            build_dir: The build directory path.
            profile: The Conan profile name to use.
            build_type: The build configuration (debug, release).
            is_cross_compilation: Whether cross-compilation is enabled.
            terminal_env: Optional terminal environment for executing commands.
            source_dir: Optional source directory path containing conanfile.

        Raises:
            ConanProfileError: If Conan profile is not found.
            ConanInstallError: If dependency installation fails.
            CommandExecutionError: If Conan command execution fails.
        """
        build_dir = build_dir or (self.workspace_dir / "build")
        log_info(f"Installing Conan dependencies for {build_type} build")

        # Determine profile path
        profile_path: Path = self.conan_dir / "profiles" / profile

        if not profile_path.exists():
            profile_error_msg: str = f"Conan profile not found: {profile_path}"
            log_error(profile_error_msg)
            raise ConanProfileError(
                profile_error_msg,
                profile=profile,
                profile_path=profile_path,
            )

        # Determine conanfile path
        # If source_dir is provided, look for conanfile there
        # Otherwise, use the workspace root conanfile
        if source_dir and source_dir.exists():
            conanfile_path: Path = source_dir / "conanfile.txt"
            if not conanfile_path.exists():
                conanfile_path = source_dir / "conanfile.py"
            if not conanfile_path.exists():
                # Use workspace root conanfile
                conanfile_path = self.workspace_dir / "conan" / "conanfile.py"
        else:
            # Use workspace root conanfile
            conanfile_path = self.workspace_dir / "conan" / "conanfile.py"

        if not conanfile_path.exists():
            error_msg: str = f"Conanfile not found: {conanfile_path}"
            log_error(error_msg)
            raise ConanInstallError(
                error_msg,
                build_dir=build_dir,
                profile=profile,
            )

        log_info(f"Using conanfile: {conanfile_path}")

        # Prepare Conan command
        conan_cmd: List[str] = [
            "conan",
            "install",
            str(conanfile_path),
            "--output-folder",
            str(build_dir),
            "--build=missing",
            "--profile:host",
            str(profile_path),
            "--profile:build",
            str(profile_path),
            "--settings",
            f"build_type={build_type.capitalize()}",
        ]

        # Add deployer for cross-compilation
        if is_cross_compilation:
            conan_cmd.insert(5, "--deployer=full_deploy")

        # Execute Conan install
        try:
            # Detect MSYS2 environment and use terminal_env if available
            import os
            msys2_path = os.environ.get("MSYS2_PATH", "")
            path = os.environ.get("PATH", "")
            is_msys2 = bool(msys2_path or "/msys64" in path or "/ucrt64" in path)

            # Use terminal_env if provided or if MSYS2 is detected
            if terminal_env or is_msys2:
                if terminal_env:
                    terminal_env.execute_in_environment(" ".join(conan_cmd), cwd=str(build_dir))
                else:
                    # Create a temporary terminal_env for MSYS2
                    temp_terminal_env = TerminalEnvironment("msys2")
                    temp_terminal_env.execute_in_environment(" ".join(conan_cmd), cwd=str(build_dir))
            else:
                execute_command(" ".join(conan_cmd))

            # Validate installation by checking for expected artifacts
            # Even if result_code is non-zero, installation may have succeeded
            # (e.g., vcvars.bat warnings don't affect the actual installation)
            # Add a small delay to ensure files are written to disk
            import time
            time.sleep(0.5)

            if self.validate_installation(build_dir):
                log_success("Conan dependencies installed successfully")
            else:
                # Only raise error if validation fails
                raise ConanInstallError(
                    f"Failed to install Conan dependencies: validation failed",
                    build_dir=build_dir,
                    profile=profile,
                )
        except FileNotFoundError as e:
            log_error(f"Conan executable not found: {e}")
            raise ConanInstallError(
                f"Conan executable not found",
                build_dir=build_dir,
                profile=profile,
            ) from e
        except PermissionError as e:
            log_error(f"Permission denied accessing Conan: {e}")
            raise ConanInstallError(
                f"Permission denied accessing Conan",
                build_dir=build_dir,
                profile=profile,
            ) from e
        except CommandExecutionError as e:
            raise ConanInstallError(
                f"Failed to install Conan dependencies: {e}",
                build_dir=build_dir,
                profile=profile,
            ) from e

    def create_profile(self, name: str, settings: Optional[dict] = None) -> bool:
        """Create a simple local profile for compatibility with legacy callers."""
        profile_path = self.conan_dir / "profiles" / name
        profile_path.parent.mkdir(parents=True, exist_ok=True)
        lines = ["[settings]"]
        for key, value in (settings or {}).items():
            lines.append(f"{key}={value}")
        profile_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return True

    def get_profile(
        self,
        compiler: Optional[str],
        build_type: str,
    ) -> str:
        """Get Conan profile name for given compiler and build type.

        This method determines the appropriate Conan profile name
        based on the compiler and build type.

        Args:
            compiler: The compiler name.
            build_type: The build configuration (debug, release).

        Returns:
            The Conan profile name.

        Raises:
            ConanProfileError: If compiler is unknown.
        """
        if not compiler:
            return build_type.lower()

        compiler_lower: str = compiler.lower()

        # Support both old and new naming conventions
        if compiler_lower in ["msvc", "clang-msvc"]:
            return f"{compiler_lower}-{build_type.lower()}"
        elif compiler_lower in ["mingw-clang", "mingw-gcc"]:
            return f"{compiler_lower}-{build_type.lower()}"
        else:
            log_error(f"Unknown compiler requested: {compiler}")
            raise ConanProfileError(
                f"Unknown compiler: {compiler}",
                profile=compiler,
            )

    def validate_installation(self, build_dir: Path) -> bool:
        """Validate that Conan installation was successful.

        This method checks for the presence of expected Conan
        installation artifacts such as toolchain files and
        build scripts.

        Args:
            build_dir: The build directory to validate.

        Returns:
            True if all expected artifacts are present, False otherwise.
        """
        log_info(f"Validating Conan installation in: {build_dir}")
        log_info(f"Build directory exists: {build_dir.exists()}")

        # List all files in build directory
        if build_dir.exists():
            all_files = list(build_dir.glob("*"))
            log_info(f"Files in build directory: {all_files}")
        else:
            log_error(f"Build directory does not exist: {build_dir}")
            return False

        # Check for conan_toolchain.cmake
        toolchain_file: Path = build_dir / "conan_toolchain.cmake"
        log_info(f"Checking for toolchain file: {toolchain_file}")
        log_info(f"Toolchain file exists: {toolchain_file.exists()}")
        if not toolchain_file.exists():
            log_error(f"Conan toolchain file not found: {toolchain_file}")
            return False

        # Check for conanbuild.sh/bat
        if sys.platform == "win32":
            build_file: Path = build_dir / "conanbuild.bat"
        else:
            build_file: Path = build_dir / "conanbuild.sh"

        log_info(f"Checking for build file: {build_file}")
        log_info(f"Build file exists: {build_file.exists()}")
        if not build_file.exists():
            log_error(f"Conan build file not found: {build_file}")
            return False

        # Check for CMakePresets.json (generated by Conan)
        cmake_presets: Path = build_dir / "CMakePresets.json"
        log_info(f"Checking for CMake presets: {cmake_presets}")
        log_info(f"CMake presets exists: {cmake_presets.exists()}")
        if not cmake_presets.exists():
            log_error(f"CMake presets not found: {cmake_presets}")
            return False

        log_success("Conan installation validation passed")
        return True
