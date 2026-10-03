# ============================================================================
# Warploom - Root CMakeLists.txt
# ============================================================================
# CMake 3.28+ Root Configuration
# Supports: Windows, Linux, WASM
# Compilers: MSVC, MSVC-Clang, MinGW-GCC, MinGW-Clang, GCC, Clang
# ============================================================================

cmake_minimum_required(VERSION 3.28...4.0)
project(Warploom
    VERSION 1.0.0
    DESCRIPTION "Warploom - deterministic simulation + node-based editor engine"
    LANGUAGES CXX
)

# ============================================================================
# Project Configuration
# ============================================================================
include(cmake/ProjectConfig.cmake)

# ============================================================================
# Platform Configuration
# ============================================================================
include(cmake/PlatformConfig.cmake)

# ============================================================================
# Compiler Flags Configuration
# ============================================================================
include(cmake/CompilerFlags.cmake)

# ============================================================================
# Build Options
# ============================================================================
# ============================================================================
# S5-B phase 2 compat (docs/warploom-identity-plan.md): the legacy
# OMNICPP_* option names keep working for one window. If an old name is
# set and the new one is not, mirror it. Removed post-0.1.
macro(_warploom_compat_option _new _old)
    if(DEFINED ${_old} AND NOT DEFINED ${_new})
        set(${_new} ${${_old}})
    endif()
endmacro()
_warploom_compat_option(WARPLOOM_BUILD_ENGINE OMNICPP_BUILD_ENGINE)
_warploom_compat_option(WARPLOEM_BUILD_TESTS OMNICPP_BUILD_TESTS)
_warploom_compat_option(WARPLOOM_BUILD_EXAMPLES OMNICPP_BUILD_EXAMPLES)
_warploom_compat_option(WARPLOOM_ENABLE_COVERAGE OMNICPP_ENABLE_COVERAGE)
_warploom_compat_option(WARPLOOM_ENABLE_FORMATTING OMNICPP_ENABLE_FORMATTING)
_warploom_compat_option(WARPLOOM_ENABLE_LINTING OMNICPP_ENABLE_LINTING)
_warploom_compat_option(WARPLOOM_ENABLE_ASAN OMNICPP_ENABLE_ASAN)
_warploom_compat_option(WARPLOOM_ENABLE_UBSAN OMNICPP_ENABLE_UBSAN)
_warploom_compat_option(WARPLOOM_ENABLE_TSAN OMNICPP_ENABLE_TSAN)
_warploom_compat_option(WARPLOOM_VULKAN_VALIDATION_TESTS OMNICPP_VULKAN_VALIDATION_TESTS)
_warploom_compat_option(WARPLOOM_USE_VULKAN OMNICPP_USE_VULKAN)
_warploom_compat_option(WARPLOOM_USE_VCPKG OMNICPP_USE_VCPKG)
_warploom_compat_option(WARPLOOM_USE_CPM OMNICPP_USE_CPM)

option(WARPLOOM_BUILD_ENGINE "Build engine library" ON)
option(WARPLOOM_BUILD_TESTS "Build tests" ON)
option(WARPLOOM_BUILD_EXAMPLES "Build examples" OFF)
option(WARPLOOM_ENABLE_COVERAGE "Enable code coverage" OFF)
option(WARPLOOM_ENABLE_FORMATTING "Enable code formatting targets" ON)
option(WARPLOOM_ENABLE_LINTING "Enable code linting targets" ON)
option(WARPLOOM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(WARPLOOM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(WARPLOOM_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(WARPLOOM_VULKAN_VALIDATION_TESTS
       "Force VK_LAYER_KHRONOS_validation for Vulkan CTest runs" OFF)

if(WARPLOOM_ENABLE_ASAN AND WARPLOOM_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
endif()
if(WARPLOOM_ENABLE_ASAN OR WARPLOOM_ENABLE_UBSAN OR WARPLOOM_ENABLE_TSAN)
    add_compile_options(-fno-omit-frame-pointer)
    if(WARPLOOM_ENABLE_ASAN)
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
    if(WARPLOOM_ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
    if(WARPLOOM_ENABLE_TSAN)
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# ============================================================================
# Dependency Options
# ============================================================================
option(WARPLOOM_USE_VULKAN "Use Vulkan graphics API" ON)

# ============================================================================
# Package Manager Options
# ============================================================================
option(WARPLOOM_USE_VCPKG "Use vcpkg (for consumers of the installed packages)" OFF)
option(WARPLOOM_USE_CPM "Use CPM.cmake to fetch GoogleTest" ON)

# ============================================================================
# Package Manager Integration
# ============================================================================
include(cmake/CPM.cmake)
include(cmake/VcpkgIntegration.cmake)
include(cmake/FindDependencies.cmake)

# ============================================================================
# Output Directories
# ============================================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Multi-config generators (MSVC, Xcode)
foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${CONFIG} CONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/bin/${CONFIG})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/lib/${CONFIG})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG} ${CMAKE_BINARY_DIR}/lib/${CONFIG})
endforeach()

# ============================================================================
# Include Directories
# ============================================================================
# Module headers are reached through each module's own PUBLIC interface
# include directory (warploom/<module>/...). There is deliberately no
# superproject-wide include path: the 58 engine/* compat forwarders that
# used to live here are gone, and every in-tree consumer spells the real
# warploom/... path.

# ============================================================================
# Graphics Backend
# ============================================================================
# Vulkan is discovered by cmake/FindDependencies.cmake. Do not manufacture
# imported targets or host-specific paths here; this keeps optional builds portable.

# ============================================================================
# Thread Support
# ============================================================================
find_package(Threads REQUIRED)

# ============================================================================
# Build Targets
# ============================================================================
if(WARPLOOM_BUILD_ENGINE)
    # S1: warploom-ui is a real module (Warploom::ui) consumed by the runtime.
    add_subdirectory(modules/ui)
    # S2: the determinism substrate is a real module (Warploom::core).
    add_subdirectory(modules/core)
    # S4: the editor widget/view layer is a real module (Warploom::editor).
    add_subdirectory(modules/editor)
    # S3: Vulkan rendering + scene projection is a real module
    # (Warploom::render). Must precede src/engine (plain-name resolution,
    # same reason core/ui/editor do).
    add_subdirectory(modules/render)
    # S3.5: asset import is a real module (Warploom::asset), same ordering.
    add_subdirectory(modules/asset)
    # S5-A: the aggregate (Warploom::engine) over all five modules.
    add_subdirectory(modules/engine)
    add_subdirectory(src/engine)
    # Headless proof host (P0): runs the live-proof harness in CI —
    # EditorSession + ControlServer, no GPU/window. Builds on every leg.
    add_subdirectory(tools)
    # Shaders are a build product of the engine, not a test fixture: one
    # root-scope target that both the tests and the viewport depend on.
    include(cmake/Shaders.cmake)
endif()

if(WARPLOOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(WARPLOOM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ============================================================================
# Testing Configuration
# ============================================================================
include(cmake/Testing.cmake)

# ============================================================================
# Coverage Configuration
# ============================================================================
include(cmake/Coverage.cmake)

# ============================================================================
# Code Quality Targets
# ============================================================================
# clang-tidy, clang-format, and their targets all live in LintTargets.cmake.
include(cmake/LintTargets.cmake)

# ============================================================================
# Installation Configuration
# ============================================================================
include(cmake/InstallRules.cmake)

# ============================================================================
# Package Configuration
# ============================================================================
include(cmake/PackageConfig.cmake)

# ============================================================================
# Configuration Summary
# ============================================================================
message(STATUS "")
message(STATUS "=== OmniCpp Configuration Summary ===")
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Description: ${PROJECT_DESCRIPTION}")
message(STATUS "")
message(STATUS "Build Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "")
message(STATUS "Platform:")
message(STATUS "  System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Windows: ${WARPLOOM_PLATFORM_WINDOWS}")
message(STATUS "  Linux: ${WARPLOOM_PLATFORM_LINUX}")
message(STATUS "  WASM: ${WARPLOOM_PLATFORM_WASM}")
message(STATUS "")
message(STATUS "Build Targets:")
message(STATUS "  Engine: ${WARPLOOM_BUILD_ENGINE}")
message(STATUS "  Tests: ${WARPLOOM_BUILD_TESTS}")
message(STATUS "  Examples: ${WARPLOOM_BUILD_EXAMPLES}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  Vulkan: ${WARPLOOM_USE_VULKAN} (${Vulkan_FOUND})")
message(STATUS "")
message(STATUS "Package Managers:")
message(STATUS "  CPM: ${WARPLOOM_USE_CPM}")
message(STATUS "  vcpkg: ${WARPLOOM_USE_VCPKG}")
message(STATUS "")
message(STATUS "Code Quality:")
message(STATUS "  Coverage: ${WARPLOOM_ENABLE_COVERAGE}")
message(STATUS "  Formatting: ${WARPLOOM_ENABLE_FORMATTING}")
message(STATUS "  Linting: ${WARPLOOM_ENABLE_LINTING}")
message(STATUS "")
message(STATUS "Installation:")
message(STATUS "  Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Bin Dir: ${CMAKE_INSTALL_BINDIR}")
message(STATUS "  Lib Dir: ${CMAKE_INSTALL_LIBDIR}")
message(STATUS "  Include Dir: ${CMAKE_INSTALL_INCLUDEDIR}")
message(STATUS "  Data Dir: ${CMAKE_INSTALL_DATAROOTDIR}")
message(STATUS "")
message(STATUS "====================================")
message(STATUS "")
