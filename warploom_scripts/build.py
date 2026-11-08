"""
Warploom Build Module.

This module handles build operations for Warploom projects, including
dependency installation, build system configuration, project building,
and artifact installation. It provides proper error handling and type hints.

Classes:
    BuildContext: Context dataclass for build operations.
    BuildError: Base exception for build-related errors.
    ConfigurationError: Exception for invalid build configurations.
    ToolchainError: Exception for toolchain issues.
    DependencyError: Exception for dependency resolution failures.
    BuildManager: Main class managing build operations.
"""

from __future__ import annotations

import shutil
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

from .cmake import CMakeManager
from .conan import ConanManager
from .utils import (
    CommandExecutionError,
    run_command,
    NotADirectoryError,
    log_error,
    log_info,
    log_success,
)


@dataclass(init=False)
class BuildContext:
    """Context for build operations.

    This dataclass encapsulates all information needed for a build operation,
    including target product, task type, architecture, build configuration,
    and various build flags.

    Attributes:
        product: The product being built (e.g., 'engine', 'game', 'all').
        task: The task being performed (e.g., 'build', 'clean', 'install').
        arch: Target architecture (e.g., 'x64', 'x86').
        build_type: Build configuration (e.g., 'debug', 'release').
        compiler: Compiler to use (e.g., 'msvc', 'gcc', 'clang').
        is_cross_compilation: Whether cross-compilation is enabled.
        lib_flag: Whether to build library components.
        st_flag: Whether to build standalone components.
        qt_vulkan_lib_flag: Whether to build Qt Vulkan library.
        qt_vulkan_st_flag: Whether to build Qt Vulkan standalone.
        unique_id: Unique identifier for this build context.
    """

    product: str = "standalone"
    task: str = "build"
    arch: str = "x64"
    build_type: str = "debug"
    compiler: Optional[str] = None
    is_cross_compilation: bool = False
    lib_flag: bool = False
    st_flag: bool = False
    qt_vulkan_lib_flag: bool = False
    qt_vulkan_st_flag: bool = False
    unique_id: str = field(default_factory=lambda: str(uuid.uuid4()))

    def __init__(self, product: str = "standalone", task: str = "build", arch: str = "x64", build_type: str = "debug", compiler: Optional[str] = None, is_cross_compilation: bool = False, lib_flag: bool = False, st_flag: bool = False, qt_vulkan_lib_flag: bool = False, qt_vulkan_st_flag: bool = False, unique_id: Optional[str] = None, target: Optional[str] = None, preset: Optional[str] = None) -> None:
        self.product = target or product
        self.task = preset or task
        self.arch = arch
        self.build_type = build_type
        self.compiler = compiler
        self.is_cross_compilation = is_cross_compilation
        self.lib_flag = lib_flag
        self.st_flag = st_flag
        self.qt_vulkan_lib_flag = qt_vulkan_lib_flag
        self.qt_vulkan_st_flag = qt_vulkan_st_flag
        self.unique_id = unique_id or str(uuid.uuid4())

    @property
    def target(self) -> str:
        return self.product

    @property
    def preset(self) -> str:
        return self.task

    def to_dict(self) -> Dict[str, Any]:
        result = self.__dict__.copy()
        result["target"] = self.product
        result["preset"] = self.task
        return result

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "BuildContext":
        values = dict(data)
        if "target" in values and "product" not in values:
            values["product"] = values.pop("target")
        values.pop("preset", None)
        values.setdefault("task", "build")
        values.setdefault("arch", "x64")
        return cls(**values)


class BuildError(Exception):
    """Base exception for build-related errors.

    This exception is raised for general build errors and includes
    context information for debugging.
    """

    def __init__(
        self,
        message: str,
        context: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize build error.

        Args:
            message: Error message describing the issue.
            context: Optional dictionary with additional context information.
        """
        self.message = message
        self.context = context or {}
        super().__init__(self.message)

    def __str__(self) -> str:
        """Return string representation of error."""
        context_str = (
            f" Context: {self.context}" if self.context else ""
        )
        return f"{self.message}{context_str}"


class ConfigurationError(BuildError):
    """Exception raised when build configuration is invalid.

    This exception is raised when build configuration contains
    invalid or inconsistent parameters.
    """

    def __init__(
        self,
        message: str,
        parameter: Optional[str] = None,
        value: Optional[Any] = None,
    ) -> None:
        """Initialize configuration error.

        Args:
            message: Error message describing the issue.
            parameter: Optional name of the invalid parameter.
            value: Optional invalid value.
        """
        context: Dict[str, Any] = {}
        if parameter:
            context["parameter"] = parameter
        if value is not None:
            context["value"] = value
        super().__init__(message, context)


class ToolchainError(BuildError):
    """Exception raised when toolchain is not available or misconfigured.

    This exception is raised when required compiler or toolchain
    cannot be found or is not properly configured.
    """

    def __init__(
        self,
        message: str,
        toolchain: Optional[str] = None,
    ) -> None:
        """Initialize toolchain error.

        Args:
            message: Error message describing the issue.
            toolchain: Optional name of the problematic toolchain.
        """
        context: Dict[str, Any] = {}
        if toolchain:
            context["toolchain"] = toolchain
        super().__init__(message, context)


class DependencyError(BuildError):
    """Exception raised when dependency resolution fails.

    This exception is raised when required dependencies cannot be
    installed or resolved.
    """

    def __init__(
        self,
        message: str,
        dependency: Optional[str] = None,
        context: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize dependency error.

        Args:
            message: Error message describing the issue.
            dependency: Optional name of the problematic dependency.
            context: Optional dictionary with additional context information.
        """
        merged_context: Dict[str, Any] = context or {}
        if dependency:
            merged_context["dependency"] = dependency
        super().__init__(message, merged_context)


class BuildManager:
    """Manages build operations for Warploom projects.

    This class provides methods for cleaning build directories,
    installing dependencies, configuring the build system,
    building projects, and installing artifacts.

    Attributes:
        workspace_dir: The root workspace directory.
        cmake_manager: The CMake manager instance.
        conan_manager: The Conan manager instance.
    """

    def __init__(self, workspace_dir: Optional[Path] = None) -> None:
        """Initialize build manager.

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
        self.cmake_manager = CMakeManager(workspace_dir)
        self.conan_manager = ConanManager(workspace_dir)

    def clean(self, context: BuildContext) -> bool:
        self.clean_build_directories(context)
        return True

    def configure(self, context: BuildContext) -> bool:
        self.configure_build_system(context)
        return True

    def build(self, context: BuildContext) -> bool:
        self.build_project(context)
        return True

    def clean_build_directories(self, context: BuildContext) -> None:
        """Clean build directories for specified targets.

        This method removes build directories for all targets specified
        in the build context.

        Args:
            context: The build context containing target information.

        Raises:
            BuildError: If cleaning any build directory fails.
            OSError: If directory removal fails.
        """
        log_info(f"Cleaning build directories for {context.product}")
        log_info(f"lib_flag: {context.lib_flag}, st_flag: {context.st_flag}")

        targets_to_clean: List[str] = []
        if context.lib_flag:
            targets_to_clean.append("engine")
        if context.st_flag:
            targets_to_clean.append("game")

        log_info(f"Targets to clean: {targets_to_clean}")

        cleaned_count: int = 0
        for target in targets_to_clean:
            build_dir: Optional[Path] = self._get_build_dir(
                target, context.arch, context.build_type, context.compiler
            )
            log_info(f"Build directory for {target}: {build_dir}")
            if build_dir and build_dir.exists():
                log_info(f"Removing build directory: {build_dir}")
                try:
                    shutil.rmtree(build_dir)
                    cleaned_count += 1
                except PermissionError as e:
                    perm_error_msg: str = (
                        f"Permission denied removing {build_dir}: {e}"
                    )
                    log_error(perm_error_msg)
                    raise BuildError(
                        f"Failed to clean build directory: {build_dir}",
                        context={
                            "target": target,
                            "error": "Permission denied",
                            "details": str(e),
                        },
                    ) from e
                except OSError as e:
                    os_error_msg: str = f"Failed to remove {build_dir}: {e}"
                    log_error(os_error_msg)
                    raise BuildError(
                        f"Failed to clean build directory: {build_dir}",
                        context={"target": target, "error": str(e)},
                    ) from e

        log_success(
            f"Build directories cleaned successfully ({cleaned_count} directories)"
        )

    def install_dependencies(self, context: BuildContext, terminal_env: Optional[Any] = None) -> None:
        """Install Conan dependencies for specified targets.

        This method installs all required Conan dependencies for the
        targets specified in the build context.

        Args:
            context: The build context containing target information.

        Raises:
            DependencyError: If dependency installation fails.
            ToolchainError: If Conan profile is not found.
        """
        log_info(f"Installing dependencies for {context.product}")

        targets_to_install: List[str] = []
        if context.lib_flag:
            targets_to_install.append("engine")
        if context.st_flag:
            targets_to_install.append("game")

        for target in targets_to_install:
            build_dir: Optional[Path] = self._get_build_dir(
                target, context.arch, context.build_type, context.compiler
            )
            source_dir: Path = self._get_source_dir(target)
            profile: str = self._get_conan_profile(
                context.compiler, context.build_type
            )

            log_info(f"Installing dependencies for {target} in {build_dir}")

            # Check if build_dir is None
            if build_dir is None:
                raise BuildError(
                    f"Build directory is None for target: {target}",
                    context={"target": target, "error": "Build directory is None"},
                )

            try:
                self.conan_manager.install(
                    build_dir,
                    profile,
                    context.build_type,
                    context.is_cross_compilation,
                    terminal_env,
                    source_dir,
                )
            except FileNotFoundError as e:
                raise ToolchainError(
                    f"Conan profile not found: {profile}",
                    toolchain=profile,
                ) from e
            except PermissionError as e:
                raise DependencyError(
                    f"Permission denied installing dependencies for {target}",
                    dependency=target,
                ) from e
            except Exception as e:
                raise DependencyError(
                    f"Failed to install dependencies for {target}",
                    dependency=target,
                ) from e

        log_success("Dependencies installed successfully")

    def configure_build_system(self, context: BuildContext) -> None:
        """Configure CMake build system for specified targets.

        This method configures the CMake build system for all targets
        specified in the build context.

        Args:
            context: The build context containing target information.

        Raises:
            ConfigurationError: If CMake configuration fails.
            BuildError: If source or build directories are invalid.
        """
        log_info(f"Configuring build system for {context.product}")

        targets_to_configure: List[str] = []
        if context.lib_flag:
            targets_to_configure.append("engine")
        if context.st_flag:
            targets_to_configure.append("game")

        for target in targets_to_configure:
            build_dir: Optional[Path] = self._get_build_dir(
                target, context.arch, context.build_type, context.compiler
            )
            source_dir: Path = self._get_source_dir(target)

            # Determine if building as library or standalone
            build_library: bool = (target == "engine")

            log_info(f"Configuring {target} in {build_dir}")

            # Check if build_dir is None
            if build_dir is None:
                raise BuildError(
                    f"Build directory is None for target: {target}",
                    context={"target": target, "error": "Build directory is None"},
                )

            try:
                self.cmake_manager.configure(
                    source_dir=source_dir,
                    build_dir=build_dir,
                    build_type=context.build_type,
                    compiler=context.compiler,
                    is_cross_compilation=context.is_cross_compilation,
                    build_library=build_library,
                )
            except FileNotFoundError as e:
                raise ConfigurationError(
                    f"Source directory not found for {target}",
                    parameter="source_dir",
                    value=str(source_dir),
                ) from e
            except Exception as e:
                raise ConfigurationError(
                    f"Failed to configure {target}",
                    parameter="target",
                    value=target,
                ) from e

        log_success("Build system configured successfully")

    def build_project(self, context: BuildContext) -> None:
        """Build project for specified targets.

        This method builds all targets specified in the build context.

        Args:
            context: The build context containing target information.

        Raises:
            BuildError: If building any target fails.
            CommandExecutionError: If build command execution fails.
        """
        log_info(f"Building {context.product}")

        targets_to_build: List[str] = []
        if context.lib_flag:
            targets_to_build.append("engine")
        if context.st_flag:
            targets_to_build.append("game")

        for target in targets_to_build:
            build_dir: Optional[Path] = self._get_build_dir(
                target, context.arch, context.build_type, context.compiler
            )

            log_info(f"Building {target} in {build_dir}")

            # Check if build_dir is None
            if build_dir is None:
                raise BuildError(
                    f"Build directory is None for target: {target}",
                    context={"target": target, "error": "Build directory is None"},
                )

            # Get CMake target name
            cmake_target: str = self._get_cmake_target_name(target)
            log_info(f"Building CMake target: {cmake_target}")

            try:
                self.cmake_manager.build(
                    build_dir=build_dir,
                    build_type=context.build_type,
                    target=cmake_target,
                    compiler=context.compiler,
                )
            except FileNotFoundError as e:
                raise BuildError(
                    f"Build directory not found for {target}",
                    context={"target": target, "error": "Directory not found"},
                ) from e
            except CommandExecutionError as e:
                raise BuildError(
                    f"Failed to build {target}",
                    context={"target": target, "error": str(e)},
                ) from e
            except Exception as e:
                raise BuildError(
                    f"Unexpected error building {target}",
                    context={"target": target, "error": str(e)},
                ) from e

        log_success("Build completed successfully")

    def install_artifacts(self, context: BuildContext) -> None:
        """Install build artifacts to installation directory.

        This method installs build artifacts for all standalone targets
        specified in the build context.

        Args:
            context: The build context containing target information.

        Raises:
            BuildError: If artifact installation fails.
            CommandExecutionError: If install command execution fails.
        """
        log_info(f"Installing artifacts for {context.product}")

        targets_to_install: List[str] = []
        if context.st_flag:
            targets_to_install.append("game")

        for target in targets_to_install:
            build_dir: Optional[Path] = self._get_build_dir(
                target, context.arch, context.build_type, context.compiler
            )

            log_info(f"Installing {target} from {build_dir}")

            # Check if build_dir is None
            if build_dir is None:
                raise BuildError(
                    f"Build directory is None for target: {target}",
                    context={"target": target, "error": "Build directory is None"},
                )

            try:
                self.cmake_manager.install(
                    build_dir=build_dir,
                    build_type=context.build_type,
                    compiler=context.compiler,
                )
            except FileNotFoundError as e:
                raise BuildError(
                    f"Build directory not found for {target}",
                    context={"target": target, "error": "Directory not found"},
                ) from e
            except CommandExecutionError as e:
                raise BuildError(
                    f"Failed to install {target}",
                    context={"target": target, "error": str(e)},
                ) from e
            except Exception as e:
                raise BuildError(
                    f"Unexpected error installing {target}",
                    context={"target": target, "error": str(e)},
                ) from e

        log_success("Artifacts installed successfully")

    def run_clean_build_pipeline(self, context: BuildContext) -> None:
        """Execute complete clean build pipeline.

        This method runs the full clean build pipeline:
        1. Clean build directories
        2. Install dependencies
        3. Configure build system
        4. Build project

        Args:
            context: The build context containing target information.

        Raises:
            BuildError: If any step of the pipeline fails.
        """
        log_info(f"Starting Clean Build Pipeline for {context.product}")
        log_info(f"Build context: product={context.product}, task={context.task}, arch={context.arch}, build_type={context.build_type}, compiler={context.compiler}, lib_flag={context.lib_flag}, st_flag={context.st_flag}")

        try:
            log_info("Step 1: Cleaning build directories")
            self.clean_build_directories(context)
            log_info("Step 2: Installing dependencies")
            # Get terminal environment for MSYS2
            from .utils.terminal_utils import setup_terminal_environment
            terminal_env = setup_terminal_environment(context.compiler)
            self.install_dependencies(context, terminal_env)
            log_info("Step 3: Configuring build system")
            self.configure_build_system(context)
            log_info("Step 4: Building project")
            self.build_project(context)
            log_success("Clean Build Pipeline completed successfully")
        except BuildError:
            # Re-raise BuildError as-is
            raise
        except Exception as e:
            # Wrap unexpected errors
            raise BuildError(
                f"Clean Build Pipeline failed: {e}",
                context={"product": context.product, "error": str(e)},
            ) from e

    def _get_build_dir(
        self,
        target: str,
        arch: str,
        build_type: str,
        compiler: Optional[str],
    ) -> Optional[Path]:
        """Get build directory path for given configuration.

        Args:
            target: The build target (e.g., 'engine', 'game', 'library', 'standalone').
            arch: The target architecture.
            build_type: The build configuration.
            compiler: The compiler being used.

        Returns:
            The path to the build directory.

        Raises:
            ConfigurationError: If target is unknown.
        """
        build_type_lower: str = build_type.lower()
        compiler_suffix: str = f"{compiler}" if compiler else ""

        if target == "engine":
            return (
                self.workspace_dir
                / "build"
                / build_type_lower
                / compiler_suffix
                / "engine"
            )
        elif target == "game":
            return (
                self.workspace_dir
                / "build"
                / build_type_lower
                / compiler_suffix
                / "game"
            )
        elif target == "library":
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
            return (
                self.workspace_dir
                / "build"
                / build_type_lower
                / compiler_suffix
                / "targets"
                / "qt-vulkan"
                / "library"
            )
        elif target == "targets/qt-vulkan/standalone":
            return (
                self.workspace_dir
                / "build"
                / build_type_lower
                / compiler_suffix
                / "targets"
                / "qt-vulkan"
                / "standalone"
            )
        else:
            log_error(f"Unknown target requested: {target}")
            raise ConfigurationError(
                f"Unknown target: {target}",
                parameter="target",
                value=target,
            )

    def _get_source_dir(self, target: str) -> Path:
        """Get source directory for given target.

        Args:
            target: The build target (e.g., 'engine', 'game', 'library', 'standalone').

        Returns:
            The path to the source directory.

        Raises:
            ConfigurationError: If target is unknown.
        """
        if target == "engine":
            return self.workspace_dir / "src" / "engine"
        elif target == "game":
            return self.workspace_dir / "src" / "game"
        elif target == "library":
            return self.workspace_dir
        elif target == "standalone":
            return self.workspace_dir
        elif target == "targets/qt-vulkan/library":
            return self.workspace_dir / "targets" / "qt-vulkan" / "library"
        elif target == "targets/qt-vulkan/standalone":
            return self.workspace_dir / "targets" / "qt-vulkan" / "standalone"
        else:
            log_error(f"Unknown source target requested: {target}")
            raise ConfigurationError(
                f"Unknown target: {target}",
                parameter="target",
                value=target,
            )

    def _get_cmake_target_name(self, target: str) -> str:
        """Get CMake target name for high-level target.

        Args:
            target: The high-level target name (e.g., 'engine', 'game').

        Returns:
            The CMake target name.

        Raises:
            ConfigurationError: If target is unknown.
        """
        target_mapping: Dict[str, str] = {
            "engine": "warploom_engine",
            "game": "warploom_game",
            "library": "warploom_library",
            "standalone": "warploom_standalone",
        }

        if target in target_mapping:
            return target_mapping[target]
        else:
            log_error(f"Unknown target requested: {target}")
            raise ConfigurationError(
                f"Unknown target: {target}",
                parameter="target",
                value=target,
            )

    def _get_conan_profile(
        self,
        compiler: Optional[str],
        build_type: str,
    ) -> str:
        """Get Conan profile name for given compiler and build type.

        Args:
            compiler: The compiler being used.
            build_type: The build configuration.

        Returns:
            The Conan profile name.

        Raises:
            ToolchainError: If compiler is unknown.
        """
        if not compiler:
            return build_type.lower()

        compiler_lower: str = compiler.lower()
        if compiler_lower in ["msvc", "clang-msvc"]:
            return f"{compiler_lower}-{build_type.lower()}"
        elif compiler_lower in ["clang-mingw-ucrt", "gcc-mingw-ucrt", "mingw-clang", "mingw-gcc"]:
            return f"{compiler_lower}-{build_type.lower()}"
        else:
            log_error(f"Unknown compiler requested: {compiler}")
            raise ToolchainError(
                f"Unknown compiler: {compiler}",
                toolchain=compiler,
            )
