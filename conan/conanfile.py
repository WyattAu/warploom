# DO NOT use Conan cmake_layout(self) HERE!
import os
import json
from conan import ConanFile
from conan.errors import ConanInvalidConfiguration, ConanException
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.env import VirtualBuildEnv
from conan.tools.files import copy
from conan.tools.system import package_manager

# Optional utility script "conantools.py" for common tasks
try:
    import sys
    sys.path.insert(0, os.path.dirname(__file__))
    from .conantools import (
        generate_cmake_with_custom_presets,
        apply_cmake_post_processing,
        copy_additional_files
    )
    UTILITIES_AVAILABLE = True
except ImportError:
    UTILITIES_AVAILABLE = False


class Warploom(ConanFile):
    """
    Conan recipe for OmniCpp - Advanced C++ Development Template.

    This package provides a comprehensive set of dependencies for C++ development,
    including testing frameworks, and utility libraries with C++23 support.
    """

    # Package metadata
    name = "warploom"
    version = "0.0.3"
    description = "OmniCpp - Advanced C++ Development Template with C++23 support"
    topics = ("cpp", "template", "cmake", "cpp23")
    license = "MIT"

    # Build settings
    settings = "os", "compiler", "build_type", "arch"

    # Package options
    options = {
        "fPIC": [True, False],
        "shared": [True, False],
        "with_vulkan": [True, False],
        "with_opengl": [True, False],
        "with_qt6": [True, False],
        "with_testing": [True, False],
        "with_logging": [True, False],
        "with_math": [True, False],
        "with_image": [True, False],
        "with_networking": [True, False],
        "with_debugging": [True, False]
    }
    default_options = {
        "fPIC": True,
        "shared": False,
        "with_vulkan": True,
        "with_opengl": False,
        "with_qt6": False,
        "with_testing": True,
        "with_logging": True,
        "with_math": True,
        "with_image": True,
        "with_networking": True,
        "with_debugging": True
    }

    # Generators
    # generators = "CMakeDeps"  # Moved to generate() method for proper configuration

    # Source exports
    exports_sources = "patches/*", "include/*", "src/*", "CMakeLists.txt"

    def requirements(self):
        """Define package dependencies."""
        # Core utility libraries
        self.requires("fmt/[~10.2]")                    # Formatting library
        self.requires("nlohmann_json/[~3.12]")          # JSON library
        self.requires("zlib/[~1.3]")                    # Compression library
        self.requires("spdlog/[~1.14]")                 # Logging library

        # COMPLIANCE: Phase 5 - Zero-copy deserialization
        self.requires("simdjson/[~3.6]")                # Zero-copy JSON parsing
        self.requires("flatbuffers/[~23.5]")            # Zero-copy serialization

        # COMPLIANCE: Phase 3 - Structured concurrency
        self.requires("asio/[~1.30]")                   # Async I/O and structured concurrency

        # COMPLIANCE: Phase 5 - Hardware-optimized containers
        self.requires("abseil/[~20240100]")             # SwissTables and utilities

        # COMPLIANCE: Phase 5 - High-concurrency allocators
        self.requires("mimalloc/[~2.1]")                # High-performance allocator

        # COMPLIANCE: Phase 4 - Security
        self.requires("libsodium/[~1.0]")               # Cryptographic security

        # COMPLIANCE: Phase 5 - RPC contracts
        self.requires("grpc/[~1.60]")                   # RPC framework
        self.requires("protobuf/[~4.25]")               # Protocol buffers

        # Math library
        if self.options.with_math:
            self.requires("glm/[~1.0]")                # GLM math library

        # Image library
        if self.options.with_image:
            self.requires("stb/[>=2023]")               # STB image library

        # Testing frameworks
        if self.options.with_testing:
            self.requires("catch2/3.7.1")             # Catch2 testing framework
            self.requires("gtest/1.15.0")             # Google Test framework

        # Debugging support
        if self.options.with_debugging:
            self.requires("cpptrace/[~0.5]")           # Stack trace library

        # Networking support
        if self.options.with_networking:
            self.requires("openssl/[~3.2]")            # OpenSSL library
            # Database library override (to prevent source builds)
            self.requires("libpq/15.4", override=True, visible=False)

        # Vulkan support
        # Check for system-wide Vulkan SDK installation
        # If VULKAN_SDK is set, use system SDK instead of Conan dependencies
        use_system_vulkan = os.environ.get("VULKAN_SDK") is not None

        if self.options.with_vulkan:
            if use_system_vulkan:
                self.output.info("VULKAN_SDK environment variable detected. Using system-wide Vulkan SDK.")
                self.output.info("Skipping Vulkan dependencies from Conan.")
            else:
                self.output.info("VULKAN_SDK not set. Using Conan-provided Vulkan SDK.")
                self.requires("vulkan-headers/1.3.296.0", options={"build": "None"})     # Vulkan headers
                self.requires("vulkan-loader/[>=1.3]", options={"build": "None"})      # Vulkan loader
                self.requires("vulkan-validationlayers/1.3.296.0", options={"build": "None"})  # Vulkan validation layers
                self.requires("shaderc/[~2023]", options={"build": "None"})            # Shader compiler
                self.requires("spirv-tools/[~2023]", options={"build": "None"})        # SPIRV tools
                self.requires("glslang/[~13]", options={"build": "None"})             # GLSL to SPIRV compiler
                self.requires("spirv-cross/[~2023]", options={"build": "None"})        # SPIRV cross compiler

        # OpenGL support
        if self.options.with_opengl:
            self.requires("opengl/system")              # OpenGL system library
            self.requires("glad/[~0.1]")              # OpenGL loader

        # Qt6 support
        if self.options.with_qt6:
            self.requires("qt/[~6.6]")                # Qt6 framework

    def configure(self):
        """Configure package options based on build settings."""
        # Set shared library preference for all dependencies
        self.options["*"].shared = self.options.shared

        # Configure libpq options (minimal configuration)
        if self.options.with_networking and "libpq" in self.dependencies:
            self.options["libpq"].with_openssl = True
            self.options["libpq"].with_iconv = False

        # Disable iconv for other packages to prevent compilation issues
        packages_with_iconv = ["glib", "libgettext", "harfbuzz", "freetype", "libpng"]
        for pkg in packages_with_iconv:
            if pkg in self.dependencies:
                if hasattr(self.options[pkg], "with_iconv"):
                    self.options[pkg].with_iconv = False
                if hasattr(self.options[pkg], "with_libiconv"):
                    self.options[pkg].with_libiconv = False

        # Compiler-specific configurations for C++23 support
        if self.settings.compiler == "clang":
            # Clang-specific optimizations or workarounds
            pass
        elif self.settings.compiler == "gcc":
            # GCC-specific configurations
            pass
        elif self.settings.compiler == "msvc":
            # MSVC-specific settings
            pass

    def build_requirements(self):
        """Define build tool requirements."""
        self.tool_requires("cmake/[>=4.0]")            # Minimum CMake version for C++23
        self.tool_requires("ninja/[>=1.10]")           # Ninja build system

    def layout(self):
        """Define the layout for the package."""
        cmake_layout(self)

    def generate(self):
        """Generate build files and apply customizations."""
        # Generate virtual build environment
        tc_env = VirtualBuildEnv(self)
        tc_env.generate()

        # Generate CMakeDeps first to create paths file before toolchain
        deps = CMakeDeps(self)
        deps.generate()

        tc = CMakeToolchain(self)
        tc.variables["CMAKE_BUILD_TYPE"] = str(self.settings.build_type)

        # Set C++23 standard
        tc.variables["CMAKE_CXX_STANDARD"] = "23"
        tc.variables["CMAKE_CXX_STANDARD_REQUIRED"] = "ON"
        tc.variables["CMAKE_CXX_EXTENSIONS"] = "OFF"

        # Set project options based on Conan options
        tc.variables["OMNICPP_USE_VULKAN"] = "ON" if self.options.with_vulkan else "OFF"
        tc.variables["OMNICPP_USE_OPENGL"] = "ON" if self.options.with_opengl else "OFF"
        tc.variables["OMNICPP_USE_QT6"] = "ON" if self.options.with_qt6 else "OFF"
        tc.variables["OMNICPP_USE_SPDLOG"] = "ON" if self.options.with_logging else "OFF"
        tc.variables["OMNICPP_USE_GLM"] = "ON" if self.options.with_math else "OFF"
        tc.variables["OMNICPP_USE_STB"] = "ON" if self.options.with_image else "OFF"

        # On Windows with MSVC, try to set full compiler paths to avoid CPM issues
        if self.settings.os == "Windows" and self.settings.compiler == "msvc":
            import os
            # Try to find cl.exe in PATH
            cl_path = None
            try:
                import subprocess
                result = subprocess.run(['where', 'cl.exe'], capture_output=True, text=True, shell=True)
                if result.returncode == 0:
                    cl_path = result.stdout.strip().split('\n')[0]
            except:
                pass

            if cl_path:
                tc.variables["CMAKE_C_COMPILER"] = cl_path
                tc.variables["CMAKE_CXX_COMPILER"] = cl_path
                self.output.info(f"Set MSVC compiler paths: {cl_path}")

        tc.generate()

        if UTILITIES_AVAILABLE:
            # Use custom utilities for advanced CMake generation
            generate_cmake_with_custom_presets(self)
            apply_cmake_post_processing(self)
            copy_additional_files(self)
        else:
            # Utility functions not available - basic generation already done
            self.output.info("Utility functions not available - using basic CMake generation")

    def imports(self):
        """Import license files from dependencies."""
        self.copy("license*", dst="licenses", folder=True, ignore_case=True)

    def validate(self):
        """Validate configuration for compatibility."""
        # Check for incompatible compiler/library combinations
        try:
            libcxx = self.settings.compiler.libcxx
            if self.settings.os == "Windows" and libcxx == "libstdc++11":
                raise ConanInvalidConfiguration("libstdc++11 is not supported on Windows")
        except (AttributeError, KeyError, ConanException):
            pass  # libcxx not available for this compiler

        # Ensure minimum compiler versions for C++23 support
        if self.settings.compiler == "gcc" and str(self.settings.compiler.version) < "13":
            raise ConanInvalidConfiguration(
                "GCC version must be at least 13.0 for C++23 support. "
                "Current version: {}".format(self.settings.compiler.version)
            )

        if self.settings.compiler == "clang" and str(self.settings.compiler.version) < "16":
            raise ConanInvalidConfiguration(
                "Clang version must be at least 16.0 for C++23 support. "
                "Current version: {}".format(self.settings.compiler.version)
            )

        if self.settings.compiler == "msvc" and str(self.settings.compiler.version) < "193":
            raise ConanInvalidConfiguration(
                "MSVC version must be at least 2022 (19.3x) for C++23 support. "
                "Current version: {}".format(self.settings.compiler.version)
            )

        # Validate Apple Clang for macOS
        if self.settings.os == "Macos" and self.settings.compiler == "apple-clang":
            if str(self.settings.compiler.version) < "15":
                raise ConanInvalidConfiguration(
                    "Apple Clang version must be at least 15.0 for C++23 support. "
                    "Current version: {}".format(self.settings.compiler.version)
                )

    def system_requirements(self):
        """Install system-level dependencies if needed."""
        # Uncomment and modify for specific system requirements
        # apt = package_manager.Apt(self)
        # apt.install(["libsdl2-dev", "libgl1-mesa-dev"])
        # dnf = package_manager.Dnf(self)
        # dnf.install(["SDL2-devel", "mesa-libGL-devel"])
        # pacman = package_manager.PacMan(self)
        # pacman.install(["sdl2", "mesa"])
        pass

    def package_info(self):
        """Define package information."""
        self.cpp_info.libs = ["warploom_core", "warploom_ui", "warploom_editor", "warploom_render", "warploom_asset"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.defines = ["OMNICPP_CXX23_ENABLED"]
