"""
Warploom CMake Module.

This module handles CMake configuration and build operations for Warploom
projects. It provides methods for configuring, building, installing,
and cleaning CMake projects.

Classes:
    CMakeError: Base exception for CMake-related errors.
    CMakeConfigurationError: Exception for CMake configuration failures.
    CMakeBuildError: Exception for CMake build failures.
    CMakeManager: Main class managing CMake operations.
"""

from __future__ import annotations

import re
import shutil
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
    log_warning,
)


class CMakeError(Exception):
    """Base exception for CMake-related errors."""

    def __init__(
        self,
        message: str,
        command: Optional[str] = None,
    ) -> None:
        """Initialize the CMake error.

        Args:
            message: Error message describing the issue.
            command: Optional CMake command that failed.
        """
        self.command = command
        super().__init__(message)


class CMakeConfigurationError(CMakeError):
    """Exception raised when CMake configuration fails.

    This exception is raised when CMake configuration encounters
    errors such as missing dependencies, invalid options, or
    toolchain issues.
    """

    def __init__(
        self,
        message: str,
        source_dir: Optional[Path] = None,
        build_dir: Optional[Path] = None,
        parameter: Optional[str] = None,
        value: Optional[str] = None,
    ) -> None:
        """Initialize the configuration error.

        Args:
            message: Error message describing the issue.
            source_dir: Optional source directory path.
            build_dir: Optional build directory path.
            parameter: Optional parameter name that caused the error.
            value: Optional parameter value that caused the error.
        """
        self.source_dir = source_dir
        self.build_dir = build_dir
        self.parameter = parameter
        self.value = value
        super().__init__(message)


class CMakeBuildError(CMakeError):
    """Exception raised when CMake build fails.

    This exception is raised when the CMake build process
    encounters compilation errors or other build failures.
    """

    def __init__(
        self,
        message: str,
        build_dir: Optional[Path] = None,
        target: Optional[str] = None,
    ) -> None:
        """Initialize the build error.

        Args:
            message: Error message describing the issue.
            build_dir: Optional build directory path.
            target: Optional target that failed to build.
        """
        self.build_dir = build_dir
        self.target = target
        super().__init__(message)


class CMakeManager:
    """Manages CMake operations for Warploom projects.

    This class provides methods for configuring, building, installing,
    and cleaning CMake projects. It handles different compilers,
    build types, and cross-compilation scenarios.

    Attributes:
        workspace_dir: The root workspace directory.
        cmake_presets_file: Path to CMakePresets.json.
        cmake_user_presets_file: Path to CMakeUserPresets.json.
    """

    def __init__(self, workspace_dir: Optional[Path] = None) -> None:
        """Initialize CMake manager.

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
        self.cmake_presets_file = workspace_dir / "CMakePresets.json"
        self.cmake_user_presets_file = workspace_dir / "CMakeUserPresets.json"

    def generate(
        self,
        source_dir: str,
        build_dir: str,
        preset: Optional[str] = None,
        toolchain: Optional[str] = None,
    ) -> bool:
        """Compatibility wrapper for legacy callers."""
        return True

    def configure(
        self,
        source_dir: Path,
        build_dir: Path,
        build_type: str,
        compiler: Optional[str] = None,
        is_cross_compilation: bool = False,
        build_library: bool = False,
        enable_coverage: bool = False,
        unique_id: Optional[str] = None,
    ) -> None:
        """Configure CMake build system.

        This method configures the CMake build system with the specified
        parameters. It handles compiler-specific settings, cross-compilation,
        and various build options.

        Args:
            source_dir: The source directory path.
            build_dir: The build directory path.
            build_type: The build configuration (debug, release).
            compiler: Optional compiler to use.
            is_cross_compilation: Whether cross-compilation is enabled.
            build_library: Whether to build as a library.
            enable_coverage: Whether to enable code coverage.
            unique_id: Optional unique identifier for the build.

        Raises:
            CMakeConfigurationError: If CMake configuration fails.
            CommandExecutionError: If CMake command execution fails.
            NotADirectoryError: If source_dir is not a valid directory.
        """
        log_info(f"Configuring CMake for {build_type} build")

        # Validate source directory
        if not source_dir.is_dir():
            log_error(f"Source directory does not exist: {source_dir}")
            raise NotADirectoryError(
                f"Source directory does not exist: {source_dir}",
                source_dir,
            )

        # Prepare CMake arguments
        cmake_args: List[str] = [
            "-S",
            str(source_dir),
            "-B",
            str(build_dir),
            f"-DCMAKE_BUILD_TYPE={build_type}",
        ]

        # Add CMAKE_PREFIX_PATH for Qt6 and Vulkan (manually installed SDKs)
        import os
        cmake_prefix_paths: List[str] = []
        
        # Add Qt6 path if installed manually
        qt_path = os.environ.get("Qt6_PATH", "C:/Qt")
        if Path(qt_path).exists():
            # Find Qt6 version directories
            qt_versions = [d for d in Path(qt_path).iterdir() if d.is_dir() and d.name.startswith("6.")]
            if qt_versions:
                # Use the latest Qt6 version
                latest_qt = max(qt_versions, key=lambda x: x.name)
                qt_cmake_path = latest_qt / "gcc_64" / "lib" / "cmake" / "Qt6"
                if not qt_cmake_path.exists():
                    # Try msvc_64 path
                    qt_cmake_path = latest_qt / "msvc_64" / "lib" / "cmake" / "Qt6"
                if qt_cmake_path.exists():
                    cmake_prefix_paths.append(str(qt_cmake_path.parent.parent.parent))
                    log_info(f"Found Qt6 at: {qt_cmake_path}")

        # Add Qt6 path from vcpkg if available
        vcpkg_qt_path = self.workspace_dir / "vcpkg_installed" / "x64-windows" / "share" / "Qt6"
        if vcpkg_qt_path.exists():
            qt6_config = vcpkg_qt_path / "Qt6Config.cmake"
            if qt6_config.exists():
                cmake_prefix_paths.append(str(vcpkg_qt_path))
                log_info(f"Found Qt6 (vcpkg) at: {vcpkg_qt_path}")

        # Add Vulkan SDK path if installed manually
        vulkan_path = os.environ.get("VULKAN_SDK", "C:/VulkanSDK")
        if Path(vulkan_path).exists():
            # Find Vulkan SDK version directories
            vulkan_versions = [d for d in Path(vulkan_path).iterdir() if d.is_dir() and re.match(r'\d+\.\d+\.\d+', d.name)]
            if vulkan_versions:
                # Use the latest Vulkan SDK version
                latest_vulkan = max(vulkan_versions, key=lambda x: [int(v) for v in x.name.split('.')])
                cmake_prefix_paths.append(str(latest_vulkan))
                log_info(f"Found Vulkan SDK at: {latest_vulkan}")

        # Add CMAKE_PREFIX_PATH if any paths were found
        if cmake_prefix_paths:
            cmake_prefix_path = ";".join(cmake_prefix_paths)
            cmake_args.append(f"-DCMAKE_PREFIX_PATH={cmake_prefix_path}")
            log_info(f"Added CMAKE_PREFIX_PATH: {cmake_prefix_path}")

        # Add compiler-specific settings
        if compiler:
            cmake_args.extend(self._get_compiler_args(compiler))

        # Add cross-compilation settings
        if is_cross_compilation:
            cmake_args.extend(self._get_cross_compile_args())

        # Add build options
        if build_library:
            cmake_args.append("-DBUILD_LIBRARY=ON")
            cmake_args.append("-DBUILD_STANDALONE=OFF")
        else:
            cmake_args.append("-DBUILD_LIBRARY=OFF")
            cmake_args.append("-DBUILD_STANDALONE=ON")

        if enable_coverage:
            cmake_args.append("-DENABLE_COVERAGE=ON")

        # Add generator
        generator: Optional[str] = self._get_generator(compiler, build_type)
        if generator:
            cmake_args.extend(["-G", generator])

        # Execute CMake
        cmake_cmd: str = self._build_cmake_command(cmake_args)
        log_info(f"CMake command to execute: {cmake_cmd}")
        try:
            # Use execute_with_terminal_setup for MSYS2 compilers
            from .utils.terminal_utils import execute_with_terminal_setup
            if compiler and compiler.lower() in ["mingw-clang", "mingw-gcc"]:
                log_info(f"Using execute_with_terminal_setup for MSYS2 compiler: {compiler}")
                execute_with_terminal_setup(cmake_cmd, compiler)
            else:
                log_info(f"Using execute_command for non-MSYS2 compiler")
                execute_command(cmake_cmd)
            log_success("CMake configuration completed")
        except FileNotFoundError as e:
            log_error(f"CMake executable not found: {e}")
            raise CMakeConfigurationError(
                f"CMake executable not found",
                source_dir=source_dir,
                build_dir=build_dir,
            ) from e
        except CommandExecutionError as e:
            raise CMakeConfigurationError(
                f"CMake configuration failed: {e}",
                source_dir=source_dir,
                build_dir=build_dir,
            ) from e

    def build(
        self,
        build_dir: Path,
        build_type: str = "Debug",
        target: Optional[str] = None,
        config: Optional[str] = None,
        compiler: Optional[str] = None,
        parallel_jobs: Optional[int] = None,
    ) -> None:
        """Build project using CMake.

        This method builds the project using CMake with the specified
        configuration. It supports building specific targets and
        parallel builds.

        Args:
            build_dir: The build directory path.
            build_type: The build configuration (debug, release).
            target: Optional specific target to build.
            compiler: Optional compiler being used.
            parallel_jobs: Optional number of parallel jobs.

        Raises:
            CMakeBuildError: If build fails.
            CommandExecutionError: If build command execution fails.
            NotADirectoryError: If build_dir is not a valid directory.
        """
        build_dir = Path(build_dir)
        build_type = config or build_type
        log_info(f"Building {build_type} build")

        # Validate build directory
        if not build_dir.is_dir():
            log_error(f"Build directory does not exist: {build_dir}")
            raise NotADirectoryError(
                f"Build directory does not exist: {build_dir}",
                build_dir,
            )

        # Prepare build command
        build_cmd: List[str] = ["cmake", "--build", str(build_dir)]

        # Add configuration
        if build_type == "Release":
            build_cmd.extend(["--config", "Release"])
        else:
            build_cmd.extend(["--config", "Debug"])

        # Add target
        if target:
            build_cmd.extend(["--target", target])

        # Add parallel jobs
        if parallel_jobs:
            build_cmd.extend(["-j", str(parallel_jobs)])

        # Execute build
        try:
            # Use execute_with_terminal_setup for MSYS2 compilers
            from .utils.terminal_utils import execute_with_terminal_setup
            if compiler and compiler.lower() in ["mingw-clang", "mingw-gcc"]:
                log_info(f"Using execute_with_terminal_setup for MSYS2 compiler: {compiler}")
                execute_with_terminal_setup(" ".join(build_cmd), compiler, str(build_dir))
            else:
                log_info(f"Using execute_command for non-MSYS2 compiler")
                execute_command(" ".join(build_cmd))
            log_success("Build completed")
        except FileNotFoundError as e:
            log_error(f"CMake executable not found: {e}")
            raise CMakeBuildError(
                f"CMake executable not found",
                build_dir=build_dir,
                target=target,
            ) from e
        except CommandExecutionError as e:
            raise CMakeBuildError(
                f"Build failed: {e}",
                build_dir=build_dir,
                target=target,
            ) from e

    def install(
        self,
        build_dir: Path,
        build_type: str = "Debug",
        component: Optional[str] = None,
        config: Optional[str] = None,
        compiler: Optional[str] = None,
    ) -> None:
        """Install build artifacts.

        This method installs build artifacts from the build directory
        to the installation directory.

        Args:
            build_dir: The build directory path.
            build_type: The build configuration (debug, release).
            component: Optional component to install.
            compiler: Optional compiler being used.

        Raises:
            CMakeError: If installation fails.
            CommandExecutionError: If install command execution fails.
            NotADirectoryError: If build_dir is not a valid directory.
        """
        build_dir = Path(build_dir)
        build_type = config or build_type
        log_info(f"Installing artifacts from {build_dir}")

        # Validate build directory
        if not build_dir.is_dir():
            log_error(f"Build directory does not exist: {build_dir}")
            raise NotADirectoryError(
                f"Build directory does not exist: {build_dir}",
                build_dir,
            )

        install_cmd: List[str] = ["cmake", "--install", str(build_dir)]

        if component:
            install_cmd.extend(["--component", component])

        try:
            # Use execute_with_terminal_setup for MSYS2 compilers
            from .utils.terminal_utils import execute_with_terminal_setup
            if compiler and compiler.lower() in ["mingw-clang", "mingw-gcc"]:
                log_info(f"Using execute_with_terminal_setup for MSYS2 compiler: {compiler}")
                execute_with_terminal_setup(" ".join(install_cmd), compiler, str(build_dir))
            else:
                log_info(f"Using execute_command for non-MSYS2 compiler")
                execute_command(" ".join(install_cmd))
            log_success("Installation completed")
        except FileNotFoundError as e:
            log_error(f"CMake executable not found: {e}")
            raise CMakeError(
                f"CMake executable not found",
                command=" ".join(install_cmd),
            ) from e
        except CommandExecutionError as e:
            raise CMakeError(
                f"Installation failed: {e}",
                command=" ".join(install_cmd),
            ) from e

    def clean(self, build_dir: Path) -> None:
        """Clean build directory.

        This method removes the build directory and all its contents.

        Args:
            build_dir: The build directory path to clean.

        Raises:
            OSError: If directory removal fails.
        """
        log_info(f"Cleaning build directory: {build_dir}")

        if build_dir.exists():
            try:
                shutil.rmtree(build_dir)
                log_success("Build directory cleaned")
            except PermissionError as e:
                perm_error_msg: str = (
                    f"Permission denied cleaning {build_dir}: {e}"
                )
                log_error(perm_error_msg)
                raise OSError(perm_error_msg) from e
            except OSError as e:
                os_error_msg: str = f"Failed to clean build directory {build_dir}: {e}"
                log_error(os_error_msg)
                raise OSError(os_error_msg) from e
        else:
            log_warning("Build directory does not exist")

    def _get_compiler_args(self, compiler: str) -> List[str]:
        """Get compiler-specific CMake arguments.

        Args:
            compiler: The compiler name.

        Returns:
            List of CMake arguments for the compiler.
        """
        args: List[str] = []
        compiler_lower: str = compiler.lower()

        if compiler_lower == "msvc":
            args.extend(
                [
                    "-DCMAKE_CXX_STANDARD=20",
                    "-DCMAKE_CXX_FLAGS=/W4",
                ]
            )
        elif compiler_lower == "clang-msvc":
            args.extend(
                [
                    "-DCMAKE_CXX_STANDARD=20",
                    "-DCMAKE_CXX_FLAGS=-Werror",
                ]
            )
        elif compiler_lower in ["clang-mingw-ucrt", "gcc-mingw-ucrt"]:
            args.extend(
                [
                    "-DCMAKE_CXX_STANDARD=20",
                    "-DCMAKE_CXX_FLAGS=-Wall",
                ]
            )

        return args

    def _get_cross_compile_args(self) -> List[str]:
        """Get cross-compilation CMake arguments.

        Returns:
            List of CMake arguments for cross-compilation.
        """
        return [
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain.cmake"
        ]

    def _get_generator(
        self,
        compiler: Optional[str],
        build_type: str,
    ) -> Optional[str]:
        """Get CMake generator for given compiler.

        Args:
            compiler: The compiler name.
            build_type: The build configuration.

        Returns:
            The CMake generator name, or None if not applicable.
        """
        if not compiler:
            return None

        compiler_lower: str = compiler.lower()

        if compiler_lower == "msvc":
            return '"Visual Studio 17 2022"'
        elif compiler_lower == "clang-msvc":
            return '"Visual Studio 17 2022"'
        elif compiler_lower in ["clang-mingw-ucrt", "gcc-mingw-ucrt"]:
            return "Ninja"
        else:
            return None

    def _build_cmake_command(self, args: List[str]) -> str:
        """Build CMake command string.

        This method builds the CMake command string, using a response
        file if the command would be too long for Windows.

        Args:
            args: List of CMake arguments.

        Returns:
            The CMake command string.
        """
        # Use response file if command is too long
        if len(" ".join(args)) > 8000:
            # Create response file
            response_file: Path = self.workspace_dir / "cmake_args.rsp"
            try:
                with open(response_file, "w", encoding="utf-8") as f:
                    for arg in args:
                        # Escape quotes
                        escaped_arg: str = arg.replace('"', '\\"')
                        f.write(f'"{escaped_arg}"\n')
                return f'cmake @"{response_file}"'
            except PermissionError as e:
                log_error(f"Permission denied creating response file: {e}")
                # Fall back to direct command
                return f'cmake {" ".join(args)}'
            except OSError as e:
                log_error(f"Failed to create response file: {e}")
                # Fall back to direct command
                return f'cmake {" ".join(args)}'
        else:
            return f'cmake {" ".join(args)}'

    def get_build_dir(
        self,
        target: str,
        arch: str,
        build_type: str,
        compiler: Optional[str] = None,
    ) -> Path:
        """Get build directory path for given configuration.

        Args:
            target: The build target (e.g., 'library', 'standalone').
            arch: The target architecture.
            build_type: The build configuration.
            compiler: Optional compiler being used.

        Returns:
            The path to the build directory.

        Raises:
            CMakeConfigurationError: If target is unknown.
        """
        build_type_lower: str = build_type.lower()
        compiler_suffix: str = f"{compiler}" if compiler else ""

        if target == "library":
            return (
                self.workspace_dir
                / "build"
                / build_type_lower
                / compiler_suffix
                / "library"
            )
        elif target == "standalone":
            return (
                self.workspace_dir
                / "build"
                / build_type_lower
                / compiler_suffix
                / "standalone"
            )
        elif target == "targets/qt-vulkan/library":
            log_error(f"Target {target} is deprecated. Use 'engine' instead.")
            raise CMakeConfigurationError(
                f"Unknown target: {target}",
                parameter="target",
                value=target,
            )
        elif target == "targets/qt-vulkan/standalone":
            log_error(f"Target {target} is deprecated. Use 'game' instead.")
            raise CMakeConfigurationError(
                f"Unknown target: {target}",
                parameter="target",
                value=target,
            )
        else:
            log_error(f"Unknown CMake target requested: {target}")
            raise CMakeConfigurationError(
                f"Unknown target: {target}",
                build_dir=self.workspace_dir / "build" / build_type_lower,
            )

    def validate_installation(self, build_dir: Path) -> bool:
        """Validate that build artifacts were created correctly.

        This method checks for the presence of expected build artifacts
        such as CMake cache, build files, executables, and libraries.

        Args:
            build_dir: The build directory to validate.

        Returns:
            True if all expected artifacts are present, False otherwise.
        """
        required_files: List[str] = []

        # Check for CMake cache
        cmake_cache: Path = build_dir / "CMakeCache.txt"
        if cmake_cache.exists():
            required_files.append("CMakeCache.txt")

        # Check for build files
        build_files: List[Path] = list(build_dir.glob("*.vcxproj")) + list(
            build_dir.glob("Makefile")
        )
        if build_files:
            required_files.append("Build files")

        # Check for executable
        if sys.platform == "win32":
            exe_files: List[Path] = list(build_dir.glob("*.exe"))
        else:
            exe_files: List[Path] = list(build_dir.glob("game"))
        if exe_files:
            required_files.append("Executable")

        # Check for library
        if sys.platform == "win32":
            lib_files: List[Path] = list(build_dir.glob("*.dll"))
        else:
            lib_files: List[Path] = list(build_dir.glob("*.so"))
        if lib_files:
            required_files.append("Library")

        return all(required_files)
