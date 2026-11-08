"""
Build system integration module for OmniCPP.

Provides unified interface for CMake, Conan, vcpkg,
and build optimization operations.
"""

from .cmake import (
    CMakeError,
    CMakeWrapper,
    cmake_build,
    cmake_clean,
    cmake_configure,
)
from .conan import (
    ConanError,
    ConanWrapper,
    conan_create_profile,
    conan_install,
)
from .optimizer import (
    BuildOptimizer,
    BuildOptimizerError,
    optimize_build,
)
from .vcpkg import (
    VcpkgError,
    VcpkgWrapper,
    vcpkg_install,
    vcpkg_integrate,
)

__all__ = [
    # CMake
    "CMakeError",
    "CMakeWrapper",
    "cmake_configure",
    "cmake_build",
    "cmake_clean",
    # Conan
    "ConanError",
    "ConanWrapper",
    "conan_install",
    "conan_create_profile",
    # vcpkg
    "VcpkgError",
    "VcpkgWrapper",
    "vcpkg_install",
    "vcpkg_integrate",
    # Optimizer
    "BuildOptimizer",
    "BuildOptimizerError",
    "optimize_build",
]
