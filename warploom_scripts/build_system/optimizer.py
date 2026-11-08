"""
Build optimization module for Warploom build system.

Provides build optimization strategies, parallel build support,
and performance tuning for CMake builds.
"""

from __future__ import annotations

import os
import platform
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from warploom_scripts.logging.logger import get_logger
from warploom_scripts.utils.command_utils import execute_command


logger = get_logger(__name__)


class BuildOptimizerError(Exception):
    """Base exception for build optimizer errors."""

    def __init__(self, message: str, exit_code: int = 1) -> None:
        """Initialize build optimizer error.

        Args:
            message: Human-readable error message.
            exit_code: The exit code to return.
        """
        self.message = message
        self.exit_code = exit_code
        super().__init__(self.message)


class BuildOptimizer:
    """Build optimization strategies and parallel build support.

    Provides intelligent build optimization based on system resources,
    build type, and target platform.
    """

    def __init__(
        self,
        build_dir: Optional[Path] = None,
        max_jobs: Optional[int] = None,
    ) -> None:
        """Initialize build optimizer.

        Args:
            build_dir: Path to build directory.
            max_jobs: Maximum number of parallel jobs (default: auto-detect).
        """
        self.build_dir = build_dir or Path.cwd() / "build"
        self.max_jobs = max_jobs or self._detect_cpu_count()

        logger.debug(
            f"BuildOptimizer initialized: build_dir={self.build_dir}, "
            f"max_jobs={self.max_jobs}"
        )

    @staticmethod
    def _detect_cpu_count() -> int:
        """Detect number of CPU cores available.

        Returns:
            Number of CPU cores.
        """
        try:
            return os.cpu_count() or 1
        except Exception:
            return 1

    @staticmethod
    def _detect_memory_gb() -> float:
        """Detect available system memory in GB.

        Returns:
            Available memory in GB.
        """
        try:
            if platform.system().lower() == "windows":
                import ctypes
                kernel32 = ctypes.windll.kernel32
                c_ulonglong = ctypes.c_ulonglong
                class MEMORYSTATUSEX(ctypes.Structure):
                    _fields_ = [
                        ("dwLength", ctypes.c_ulong),
                        ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", c_ulonglong),
                        ("ullAvailPhys", c_ulonglong),
                        ("ullTotalPageFile", c_ulonglong),
                        ("ullAvailPageFile", c_ulonglong),
                        ("ullTotalVirtual", c_ulonglong),
                        ("ullAvailVirtual", c_ulonglong),
                        ("ullAvailExtendedVirtual", c_ulonglong),
                    ]
                memory_status = MEMORYSTATUSEX()
                memory_status.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
                kernel32.GlobalMemoryStatusEx(ctypes.byref(memory_status))
                return memory_status.ullAvailPhys / (1024 ** 3)  # type: ignore[no-any-return]
            else:
                # Linux/macOS
                with open("/proc/meminfo", "r") as f:
                    for line in f:
                        if line.startswith("MemAvailable:"):
                            mem_kb = int(line.split()[1])
                            return mem_kb / (1024 ** 2)
                # Default fallback if MemAvailable not found
                return 8.0
        except Exception as e:
            logger.warning(f"Failed to detect memory: {e}")
            return 8.0

    def calculate_optimal_jobs(
        self,
        build_type: str = "Release",
        memory_per_job: float = 1.0,
    ) -> int:
        """Calculate optimal number of parallel jobs.

        Args:
            build_type: Build type (Debug, Release, etc.).
            memory_per_job: Estimated memory per job in GB.

        Returns:
            Optimal number of parallel jobs.
        """
        cpu_count = self._detect_cpu_count()
        memory_gb = self._detect_memory_gb()

        # Adjust memory per job based on build type
        if build_type == "Debug":
            memory_per_job *= 2.0  # Debug builds use more memory
        elif build_type == "RelWithDebInfo":
            memory_per_job *= 1.5

        # Calculate jobs based on memory
        memory_limited_jobs = int(memory_gb / memory_per_job)

        # Use minimum of CPU count and memory-limited jobs
        optimal_jobs = min(cpu_count, memory_limited_jobs)

        # Apply max_jobs limit
        optimal_jobs = min(optimal_jobs, self.max_jobs)

        # Ensure at least 1 job
        optimal_jobs = max(1, optimal_jobs)

        logger.info(
            f"Optimal jobs: {optimal_jobs} (CPU: {cpu_count}, "
            f"Memory: {memory_gb:.1f}GB, Limit: {self.max_jobs})"
        )

        return optimal_jobs

    def get_optimization_flags(
        self,
        build_type: str = "Release",
        compiler: str = "gcc",
    ) -> List[str]:
        """Get compiler optimization flags for build type.

        Args:
            build_type: Build type (Debug, Release, RelWithDebInfo, MinSizeRel).
            compiler: Compiler name (gcc, clang, msvc).

        Returns:
            List of optimization flags.
        """
        flags = []

        if build_type == "Debug":
            flags.extend(["-O0", "-g"])
        elif build_type == "Release":
            flags.extend(["-O3", "-DNDEBUG"])
        elif build_type == "RelWithDebInfo":
            flags.extend(["-O2", "-g", "-DNDEBUG"])
        elif build_type == "MinSizeRel":
            flags.extend(["-Os", "-DNDEBUG"])

        # Compiler-specific optimizations
        if compiler.lower() in ["gcc", "clang"]:
            flags.extend(["-march=native", "-mtune=native"])
        elif compiler.lower() == "msvc":
            if build_type == "Release":
                flags.extend(["/O2", "/GL"])
            elif build_type == "MinSizeRel":
                flags.extend(["/O1", "/GL"])

        logger.debug(f"Optimization flags for {compiler} {build_type}: {flags}")
        return flags

    def get_linker_flags(
        self,
        build_type: str = "Release",
        compiler: str = "gcc",
    ) -> List[str]:
        """Get linker optimization flags for build type.

        Args:
            build_type: Build type (Debug, Release, RelWithDebInfo, MinSizeRel).
            compiler: Compiler name (gcc, clang, msvc).

        Returns:
            List of linker flags.
        """
        flags = []

        if build_type == "Debug":
            # No special linker flags for debug
            pass
        elif build_type in ["Release", "RelWithDebInfo"]:
            if compiler.lower() in ["gcc", "clang"]:
                flags.extend(["-Wl,--as-needed", "-Wl,--strip-all"])
            elif compiler.lower() == "msvc":
                flags.extend(["/LTCG", "/OPT:REF", "/OPT:ICF"])
        elif build_type == "MinSizeRel":
            if compiler.lower() in ["gcc", "clang"]:
                flags.extend(["-Wl,--as-needed", "-Wl,--strip-all", "-s"])
            elif compiler.lower() == "msvc":
                flags.extend(["/LTCG", "/OPT:REF", "/OPT:ICF", "/OPT:SIZE"])

        logger.debug(f"Linker flags for {compiler} {build_type}: {flags}")
        return flags

    def get_cmake_args(
        self,
        build_type: str = "Release",
        parallel_jobs: Optional[int] = None,
        use_ccache: bool = True,
        use_unity_builds: bool = False,
    ) -> List[str]:
        """Get CMake arguments for optimized build.

        Args:
            build_type: Build type (Debug, Release, etc.).
            parallel_jobs: Number of parallel jobs (default: auto-detect).
            use_ccache: Enable ccache for faster rebuilds.
            use_unity_builds: Enable unity builds for faster compilation.

        Returns:
            List of CMake arguments.
        """
        args = []

        # Build type
        args.append(f"-DCMAKE_BUILD_TYPE={build_type}")

        # Parallel jobs
        if parallel_jobs is None:
            parallel_jobs = self.calculate_optimal_jobs(build_type)
        args.append(f"-DCMAKE_JOB_POOLS:STRING=compile={parallel_jobs}")

        # ccache support
        if use_ccache:
            if self._is_ccache_available():
                args.extend([
                    "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                    "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
                ])
                logger.info("Enabled ccache for faster rebuilds")

        # Unity builds
        if use_unity_builds:
            args.extend([
                "-DCMAKE_UNITY_BUILD=ON",
                "-DCMAKE_UNITY_BUILD_BATCH_SIZE=8",
            ])
            logger.info("Enabled unity builds for faster compilation")

        # Ninja generator optimizations
        args.append("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")

        logger.debug(f"CMake optimization args: {args}")
        return args

    @staticmethod
    def _is_ccache_available() -> bool:
        """Check if ccache is available.

        Returns:
            True if ccache is available, False otherwise.
        """
        try:
            result = subprocess.run(
                ["ccache", "--version"],
                capture_output=True,
                timeout=5,
            )
            return result.returncode == 0
        except Exception:
            return False

    def optimize_build_cache(self) -> int:
        """Optimize build cache for faster incremental builds.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        logger.info("Optimizing build cache")

        try:
            # Enable CMake build cache
            cache_file = self.build_dir / "CMakeCache.txt"
            if cache_file.exists():
                # Set cache optimization flags
                cache_vars = {
                    "CMAKE_DISABLE_SOURCE_CHANGES": "ON",
                    "CMAKE_DEPENDS_CHECK_STAGE": "TRUE",
                }

                for var, value in cache_vars.items():
                    cmd = f"cmake -D {var}={value} -S . -B {self.build_dir}"
                    execute_command(cmd, timeout=60)

                logger.info("Build cache optimization completed")
                return 0
            else:
                logger.warning("CMakeCache.txt not found, skipping cache optimization")
                return 1
        except Exception as e:
            error_msg = f"Build cache optimization failed: {e}"
            logger.error(error_msg)
            raise BuildOptimizerError(error_msg) from e

    def get_build_statistics(self) -> Dict[str, Any]:
        """Get build statistics and performance metrics.

        Returns:
            Dictionary containing build statistics.
        """
        stats = {
            "cpu_count": self._detect_cpu_count(),
            "memory_gb": self._detect_memory_gb(),
            "max_jobs": self.max_jobs,
            "build_dir": str(self.build_dir),
        }

        # Check if build directory exists
        if self.build_dir.exists():
            # Calculate build directory size
            total_size = 0
            for root, dirs, files in os.walk(self.build_dir):
                for file in files:
                    file_path = Path(root) / file
                    try:
                        total_size += file_path.stat().st_size
                    except Exception:
                        pass

            stats["build_dir_size_mb"] = total_size / (1024 ** 2)
            stats["build_dir_exists"] = True
        else:
            stats["build_dir_exists"] = False
            stats["build_dir_size_mb"] = 0

        # Check ccache statistics
        if self._is_ccache_available():
            try:
                result = subprocess.run(
                    ["ccache", "-s"],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                if result.returncode == 0:
                    stats["ccache_enabled"] = True
                    stats["ccache_stats"] = result.stdout
                else:
                    stats["ccache_enabled"] = False
            except Exception:
                stats["ccache_enabled"] = False
        else:
            stats["ccache_enabled"] = False

        logger.debug(f"Build statistics: {stats}")
        return stats

    def recommend_optimizations(
        self,
        build_type: str = "Release",
        target_platform: str = "native",
    ) -> List[str]:
        """Recommend build optimizations based on system and target.

        Args:
            build_type: Build type (Debug, Release, etc.).
            target_platform: Target platform (native, linux, windows, wasm).

        Returns:
            List of recommended optimizations.
        """
        recommendations = []

        # CPU-based recommendations
        cpu_count = self._detect_cpu_count()
        if cpu_count >= 8:
            recommendations.append(
                f"High CPU count detected ({cpu_count} cores). "
                "Consider using parallel builds with -j{cpu_count}."
            )

        # Memory-based recommendations
        memory_gb = self._detect_memory_gb()
        if memory_gb >= 16:
            recommendations.append(
                f"Sufficient memory available ({memory_gb:.1f}GB). "
                "Consider enabling unity builds for faster compilation."
            )
        elif memory_gb < 8:
            recommendations.append(
                f"Low memory available ({memory_gb:.1f}GB). "
                "Consider reducing parallel jobs to avoid swapping."
            )

        # ccache recommendation
        if not self._is_ccache_available():
            recommendations.append(
                "ccache not detected. Installing ccache can significantly "
                "speed up incremental builds."
            )

        # Build type-specific recommendations
        if build_type == "Debug":
            recommendations.append(
                "Debug builds are slow. Consider using RelWithDebInfo "
                "for better performance with debugging symbols."
            )

        # Platform-specific recommendations
        if target_platform == "wasm":
            recommendations.append(
                "WASM builds can be slow. Consider using "
                "Emscripten's parallel compilation features."
            )

        logger.info(f"Optimization recommendations: {len(recommendations)} suggestions")
        return recommendations

    def clean_build_cache(self) -> int:
        """Clean build cache to free disk space.

        Returns:
            Exit code (0 for success, non-zero for failure).
        """
        logger.info("Cleaning build cache")

        try:
            # Clean ccache if available
            if self._is_ccache_available():
                execute_command("ccache -C", timeout=60)
                logger.info("Cleaned ccache")

            # Clean CMake cache
            cache_file = self.build_dir / "CMakeCache.txt"
            if cache_file.exists():
                cache_file.unlink()
                logger.info("Removed CMakeCache.txt")

            # Clean CMake files directory
            cmake_files = self.build_dir / "CMakeFiles"
            if cmake_files.exists():
                import shutil
                shutil.rmtree(cmake_files)
                logger.info("Removed CMakeFiles directory")

            logger.info("Build cache cleaning completed")
            return 0
        except Exception as e:
            error_msg = f"Build cache cleaning failed: {e}"
            logger.error(error_msg)
            raise BuildOptimizerError(error_msg) from e


def optimize_build(args: Dict[str, Any]) -> int:
    """Optimize build with given arguments.

    This function provides a command-line interface for build optimization.

    Args:
        args: Dictionary containing optimization arguments:
            - build_type: Build type
            - parallel_jobs: Number of parallel jobs
            - build_dir: Build directory path
            - use_ccache: Enable ccache
            - use_unity_builds: Enable unity builds

    Returns:
        Exit code (0 for success, non-zero for failure).
    """
    try:
        optimizer = BuildOptimizer(
            build_dir=Path(args.get("build_dir", "build")),
            max_jobs=args.get("max_jobs"),
        )

        # Get optimization flags
        cmake_args = optimizer.get_cmake_args(
            build_type=args.get("build_type", "Release"),
            parallel_jobs=args.get("parallel_jobs"),
            use_ccache=args.get("use_ccache", True),
            use_unity_builds=args.get("use_unity_builds", False),
        )

        # Print recommendations
        recommendations = optimizer.recommend_optimizations(
            build_type=args.get("build_type", "Release"),
            target_platform=args.get("target_platform", "native"),
        )

        logger.info("Build optimization recommendations:")
        for rec in recommendations:
            logger.info(f"  - {rec}")

        return 0
    except BuildOptimizerError as e:
        logger.error(f"Build optimization failed: {e.message}")
        return e.exit_code
    except Exception as e:
        logger.error(f"Unexpected error during build optimization: {e}")
        return 1


__all__ = [
    "BuildOptimizerError",
    "BuildOptimizer",
    "optimize_build",
]
