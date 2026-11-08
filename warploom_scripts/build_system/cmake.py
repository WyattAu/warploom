"""
CMake wrapper and integration module for OmniCPP build system.

Provides CMake configuration, build functions, preset management,
and toolchain file selection for cross-platform compilation.
"""

from __future__ import annotations

import json
import logging
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from warploom_scripts.logging.logger import get_logger
from warploom_scripts.utils.command_utils import execute_command
from warploom_scripts.utils.file_utils import FileUtils


logger = get_logger(__name__)


class CMakeError(Exception):
    """Base exception for CMake-related errors."""

    def __init__(self, message: str, command: str, exit_code: int = 1) -> None:
        """Initialize CMake error.

        Args:
            message: Human-readable error message.
            command: The CMake command that failed.
            exit_code: The exit code to return.
        """
        self.message = message
        self.command = command
        self.exit_code = exit_code
        super().__init__(self.message)


class CMakeWrapper:
    """Wrapper class for CMake operations.

    Provides high-level interface for CMake configuration, building,
    and management operations with proper error handling and logging.
    """

    def __init__(
        self,
        source_dir: Optional[Path] = None,
        build_dir: Optional[Path] = None,
        cmake_path: Optional[Path] = None,
    ) -> None:
        """Initialize CMake wrapper.

        Args:
            source_dir: Path to CMake source directory (default: current directory).
            build_dir: Path to CMake build directory (default: build/).
            cmake_path: Path to CMake executable (default: cmake from PATH).
        """
        self.source_dir = source_dir or Path.cwd()
        self.build_dir = build_dir or self.source_dir / "build"
        self.cmake_path = cmake_path or Path("cmake")

        logger.debug(
            f"CMakeWrapper initialized: source={self.source_dir}, "
            f"build={self.build_dir}, cmake={self.cmake_path}"
        )

    def configure(
        self,
        build_type: str = "Release",
        generator: Optional[str] = None,
        toolchain: Optional[Path] = None,
        preset: Optional[str] = None,
        extra_args: Optional[List[str]] = None,
    ) -> int:
        """Configure CMake project.

        Args:
            build_type: Build type (Debug, Release, RelWithDebInfo, MinSizeRel).
            generator: CMake generator name (e.g., "Ninja", "Visual Studio 17 2022").
            toolchain: Path to CMake toolchain file for cross-compilation.
            preset: CMake preset name to use.
            extra_args: Additional CMake arguments.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            CMakeError: If CMake configuration fails.
        """
        logger.info(f"Configuring CMake project: {self.source_dir}")

        # Build CMake command
        cmd_parts = [str(self.cmake_path)]

        # Add preset if specified
        if preset:
            cmd_parts.extend(["--preset", preset])
            logger.info(f"Using CMake preset: {preset}")
        else:
            # Add source directory
            cmd_parts.append("-S")
            cmd_parts.append(str(self.source_dir))
            cmd_parts.append("-B")
            cmd_parts.append(str(self.build_dir))

            # Add build type
            cmd_parts.append(f"-DCMAKE_BUILD_TYPE={build_type}")

            # Add generator if specified
            if generator:
                cmd_parts.extend(["-G", generator])
                logger.info(f"Using CMake generator: {generator}")

            # Add toolchain file if specified
            if toolchain:
                if not toolchain.exists():
                    raise CMakeError(
                        f"Toolchain file not found: {toolchain}",
                        f"cmake -DCMAKE_TOOLCHAIN_FILE={toolchain}",
                    )
                cmd_parts.extend(["-DCMAKE_TOOLCHAIN_FILE", str(toolchain)])
                logger.info(f"Using toolchain file: {toolchain}")

        # Add extra arguments
        if extra_args:
            cmd_parts.extend(extra_args)

        cmd = " ".join(cmd_parts)
        logger.debug(f"CMake configure command: {cmd}")
        try:
            execute_command(cmd, timeout=600)
            logger.info("CMake configuration completed successfully")
            return 0
        except Exception as e:
            error_msg = f"CMake configuration failed: {e}"
            logger.error(error_msg)
            raise CMakeError(error_msg, cmd) from e

    def build(
        self,
        target: str = "all",
        config: str = "Release",
        parallel: Optional[int] = None,
        clean: bool = False,
        extra_args: Optional[List[str]] = None,
    ) -> int:
        """Build CMake project.

        Args:
            target: Target to build (default: "all").
            config: Build configuration (Debug, Release, etc.).
            parallel: Number of parallel jobs (default: auto-detect).
            clean: Clean build directory before building.
            extra_args: Additional build arguments.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            CMakeError: If CMake build fails.
        """
        logger.info(f"Building CMake target: {target}")

        # Clean if requested
        if clean:
            self.clean(target)
            logger.info("Cleaned build directory")

        # Build CMake command
        cmd_parts = [str(self.cmake_path), "--build", str(self.build_dir)]

        # Add target
        cmd_parts.extend(["--target", target])

        # Add configuration
        cmd_parts.extend(["--config", config])

        # Add parallel jobs
        if parallel is None:
            # Auto-detect number of CPU cores
            try:
                import os
                parallel = os.cpu_count() or 1
            except Exception:
                parallel = 1

        if parallel > 1:
            cmd_parts.extend(["--parallel", str(parallel)])
            logger.info(f"Building with {parallel} parallel jobs")

        # Add extra arguments
        if extra_args:
            cmd_parts.extend(extra_args)

        cmd = " ".join(cmd_parts)
        logger.debug(f"CMake build command: {cmd}")

        try:
            execute_command(cmd, timeout=3600)
            logger.info(f"CMake build completed successfully: {target}")
            return 0
        except Exception as e:
            error_msg = f"CMake build failed: {e}"
            logger.error(error_msg)
            raise CMakeError(error_msg, cmd) from e

    def clean(self, target: Optional[str] = None) -> int:
        """Clean CMake build artifacts.

        Args:
            target: Specific target to clean (default: clean all).

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            CMakeError: If CMake clean fails.
        """
        logger.info("Cleaning CMake build directory")

        # Build CMake command
        cmd_parts = [str(self.cmake_path), "--build", str(self.build_dir)]

        if target:
            cmd_parts.extend(["--target", "clean"])
        else:
            # Remove entire build directory
            try:
                if self.build_dir.exists():
                    import shutil
                    shutil.rmtree(self.build_dir)
                    logger.info(f"Removed build directory: {self.build_dir}")
                    return 0
            except Exception as e:
                error_msg = f"Failed to remove build directory: {e}"
                logger.error(error_msg)
                raise CMakeError(error_msg, "rm -rf build") from e

        cmd = " ".join(cmd_parts)
        logger.debug(f"CMake clean command: {cmd}")

        try:
            execute_command(cmd, timeout=300)
            logger.info("CMake clean completed successfully")
            return 0
        except Exception as e:
            error_msg = f"CMake clean failed: {e}"
            logger.error(error_msg)
            raise CMakeError(error_msg, cmd) from e

    def install(
        self,
        prefix: Optional[Path] = None,
        config: str = "Release",
        extra_args: Optional[List[str]] = None,
    ) -> int:
        """Install CMake project.

        Args:
            prefix: Installation prefix path.
            config: Build configuration (Debug, Release, etc.).
            extra_args: Additional install arguments.

        Returns:
            Exit code (0 for success, non-zero for failure).

        Raises:
            CMakeError: If CMake install fails.
        """
        logger.info("Installing CMake project")

        # Build CMake command
        cmd_parts = [str(self.cmake_path), "--install", str(self.build_dir)]

        # Add prefix if specified
        if prefix:
            cmd_parts.extend(["--prefix", str(prefix)])
            logger.info(f"Installing to prefix: {prefix}")

        # Add configuration
        cmd_parts.extend(["--config", config])

        # Add extra arguments
        if extra_args:
            cmd_parts.extend(extra_args)

        cmd = " ".join(cmd_parts)
        logger.debug(f"CMake install command: {cmd}")

        try:
            execute_command(cmd, timeout=600)
            logger.info("CMake install completed successfully")
            return 0
        except Exception as e:
            error_msg = f"CMake install failed: {e}"
            logger.error(error_msg)
            raise CMakeError(error_msg, cmd) from e

    def list_presets(self) -> List[Dict[str, str]]:
        """List available CMake presets.

        Returns:
            List of preset dictionaries containing name and description.

        Raises:
            CMakeError: If preset listing fails.
        """
        logger.info("Listing CMake presets")

        presets_file = self.source_dir / "CMakePresets.json"
        user_presets_file = self.source_dir / "CMakeUserPresets.json"

        presets = []

        # Read CMakePresets.json
        if presets_file.exists():
            try:
                with open(presets_file, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    for preset in data.get("configurePresets", []):
                        presets.append({
                            "name": preset.get("name", ""),
                            "description": preset.get("description", ""),
                            "source": "CMakePresets.json",
                        })
            except Exception as e:
                logger.warning(f"Failed to read CMakePresets.json: {e}")

        # Read CMakeUserPresets.json
        if user_presets_file.exists():
            try:
                with open(user_presets_file, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    for preset in data.get("configurePresets", []):
                        presets.append({
                            "name": preset.get("name", ""),
                            "description": preset.get("description", ""),
                            "source": "CMakeUserPresets.json",
                        })
            except Exception as e:
                logger.warning(f"Failed to read CMakeUserPresets.json: {e}")

        logger.info(f"Found {len(presets)} CMake presets")
        return presets

    def get_preset(self, preset_name: str) -> Optional[Dict[str, str]]:
        """Get specific CMake preset by name.

        Args:
            preset_name: Name of the preset to retrieve.

        Returns:
            Preset dictionary if found, None otherwise.
        """
        presets = self.list_presets()
        for preset in presets:
            if preset["name"] == preset_name:
                return preset
        return None

    def select_toolchain(
        self,
        target_platform: str,
        target_arch: str,
        toolchain_dir: Optional[Path] = None,
    ) -> Path:
        """Select appropriate CMake toolchain file for cross-compilation.

        Args:
            target_platform: Target platform (linux, windows, wasm, etc.).
            target_arch: Target architecture (x86_64, ARM64, etc.).
            toolchain_dir: Directory containing toolchain files (default: cmake/toolchains/).

        Returns:
            Path to selected toolchain file.

        Raises:
            CMakeError: If no suitable toolchain file is found.
        """
        logger.info(f"Selecting toolchain for {target_platform}-{target_arch}")

        if toolchain_dir is None:
            toolchain_dir = self.source_dir / "cmake" / "toolchains"

        # Build toolchain file name based on target
        toolchain_files = {
            ("linux", "x86_64"): "x86-linux-gnu.cmake",
            ("linux", "ARM64"): "arm64-linux-gnu.cmake",
            ("windows", "ARM64"): "arm64-windows-msvc.cmake",
            ("wasm", "any"): "emscripten.cmake",
        }

        # Get toolchain file name
        key = (target_platform, target_arch)
        if key not in toolchain_files:
            key = (target_platform, "any")

        if key not in toolchain_files:
            raise CMakeError(
                f"No toolchain file found for {target_platform}-{target_arch}",
                "select_toolchain",
            )

        toolchain_name = toolchain_files[key]
        toolchain_path = toolchain_dir / toolchain_name

        if not toolchain_path.exists():
            raise CMakeError(
                f"Toolchain file not found: {toolchain_path}",
                "select_toolchain",
            )

        logger.info(f"Selected toolchain file: {toolchain_path}")
        return toolchain_path

    def validate_toolchain(self, toolchain_path: Path) -> bool:
        """Validate CMake toolchain file.

        Args:
            toolchain_path: Path to toolchain file to validate.

        Returns:
            True if toolchain file is valid, False otherwise.
        """
        logger.debug(f"Validating toolchain file: {toolchain_path}")

        if not toolchain_path.exists():
            logger.error(f"Toolchain file does not exist: {toolchain_path}")
            return False

        # Check if file is readable
        try:
            content = toolchain_path.read_text(encoding="utf-8")
            if not content.strip():
                logger.error(f"Toolchain file is empty: {toolchain_path}")
                return False
        except Exception as e:
            logger.error(f"Failed to read toolchain file: {e}")
            return False

        # Basic syntax check (look for CMake commands)
        required_keywords = ["CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_PROCESSOR"]
        for keyword in required_keywords:
            if keyword not in content:
                logger.warning(
                    f"Toolchain file may be missing required keyword: {keyword}"
                )

        logger.debug(f"Toolchain file validation passed: {toolchain_path}")
        return True

    def get_version(self) -> Optional[str]:
        """Get CMake version.

        Returns:
            CMake version string if available, None otherwise.
        """
        try:
            result = subprocess.run(
                [str(self.cmake_path), "--version"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            if result.returncode == 0:
                # Parse version from output (e.g., "cmake version 3.28.0")
                version_line = result.stdout.split("\n")[0]
                version = version_line.split()[-1]
                logger.debug(f"CMake version: {version}")
                return version
        except Exception as e:
            logger.warning(f"Failed to get CMake version: {e}")

        return None


def cmake_configure(args: Dict[str, Any]) -> int:
    """Configure CMake project with given arguments.

    This function provides a command-line interface for CMake configuration.

    Args:
        args: Dictionary containing configuration arguments:
            - build_type: Build type (Debug, Release, etc.)
            - generator: CMake generator name
            - toolchain: Path to toolchain file
            - preset: CMake preset name
            - source_dir: Source directory path
            - build_dir: Build directory path

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    try:
        wrapper = CMakeWrapper(
            source_dir=Path(args.get("source_dir", ".")),
            build_dir=Path(args.get("build_dir", "build")),
        )

        return wrapper.configure(
            build_type=args.get("build_type", "Release"),
            generator=args.get("generator"),
            toolchain=Path(args["toolchain"]) if args.get("toolchain") else None,
            preset=args.get("preset"),
        )
    except CMakeError as e:
        logger.error(f"CMake configuration failed: {e.message}")
        return e.exit_code
    except Exception as e:
        logger.error(f"Unexpected error during CMake configuration: {e}")
        return 1


def cmake_build(args: Dict[str, Any]) -> int:
    """Build CMake project with given arguments.

    This function provides a command-line interface for CMake building.

    Args:
        args: Dictionary containing build arguments:
            - target: Target to build
            - config: Build configuration
            - parallel: Number of parallel jobs
            - clean: Clean before building
            - build_dir: Build directory path

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    try:
        wrapper = CMakeWrapper(
            build_dir=Path(args.get("build_dir", "build")),
        )

        return wrapper.build(
            target=args.get("target", "all"),
            config=args.get("config", "Release"),
            parallel=args.get("parallel"),
            clean=args.get("clean", False),
        )
    except CMakeError as e:
        logger.error(f"CMake build failed: {e.message}")
        return e.exit_code
    except Exception as e:
        logger.error(f"Unexpected error during CMake build: {e}")
        return 1


def cmake_clean(args: Dict[str, Any]) -> int:
    """Clean CMake build artifacts.

    Args:
        args: Dictionary containing clean arguments:
            - target: Specific target to clean
            - build_dir: Build directory path

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    try:
        wrapper = CMakeWrapper(
            build_dir=Path(args.get("build_dir", "build")),
        )

        return wrapper.clean(target=args.get("target"))
    except CMakeError as e:
        logger.error(f"CMake clean failed: {e.message}")
        return e.exit_code
    except Exception as e:
        logger.error(f"Unexpected error during CMake clean: {e}")
        return 1


__all__ = [
    "CMakeError",
    "CMakeWrapper",
    "cmake_configure",
    "cmake_build",
    "cmake_clean",
]
