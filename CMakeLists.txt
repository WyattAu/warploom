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
option(OMNICPP_BUILD_ENGINE "Build engine library" ON)
option(OMNICPP_LEGACY_ENGINE "Build legacy subsystem engine and game" OFF)
option(OMNICPP_BUILD_GAME "Build game executable (requires OMNICPP_LEGACY_ENGINE)" OFF)
option(OMNICPP_BUILD_TESTS "Build tests" ON)
option(OMNICPP_BUILD_EXAMPLES "Build examples" OFF)
option(OMNICPP_ENABLE_COVERAGE "Enable code coverage" OFF)
option(OMNICPP_ENABLE_FORMATTING "Enable code formatting targets" ON)
option(OMNICPP_ENABLE_LINTING "Enable code linting targets" ON)
option(OMNICPP_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(OMNICPP_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(OMNICPP_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(OMNICPP_VULKAN_VALIDATION_TESTS
       "Force VK_LAYER_KHRONOS_validation for Vulkan CTest runs" OFF)

if(OMNICPP_ENABLE_ASAN AND OMNICPP_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
endif()
if(OMNICPP_ENABLE_ASAN OR OMNICPP_ENABLE_UBSAN OR OMNICPP_ENABLE_TSAN)
    add_compile_options(-fno-omit-frame-pointer)
    if(OMNICPP_ENABLE_ASAN)
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
    if(OMNICPP_ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
    if(OMNICPP_ENABLE_TSAN)
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# ============================================================================
# Dependency Options
# ============================================================================
option(OMNICPP_USE_VULKAN "Use Vulkan graphics API" ON)
option(OMNICPP_USE_OPENGL "Use OpenGL graphics API" OFF)
option(OMNICPP_USE_GLFW "Use GLFW windowing library" ON)
option(OMNICPP_USE_QT6 "Use Qt6 framework" ON)
option(OMNICPP_USE_QUILL "Use Quill logging library" ON)
option(OMNICPP_USE_GLM "Use GLM math library" ON)
option(OMNICPP_USE_STB "Use STB image library" ON)
option(OMNICPP_USE_NLOHMANN_JSON "Use nlohmann/json library" ON)

# ============================================================================
# Package Manager Options
# ============================================================================
option(OMNICPP_USE_CONAN "Use Conan package manager" OFF)
option(OMNICPP_USE_VCPKG "Use vcpkg package manager" OFF)
option(OMNICPP_USE_CPM "Use CPM.cmake package manager" ON)

# ============================================================================
# Package Manager Integration
# ============================================================================
include(cmake/CPM.cmake)
include(cmake/ConanIntegration.cmake)
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
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/OmniCppLib
    ${CMAKE_SOURCE_DIR}/include/engine
    ${CMAKE_SOURCE_DIR}/include/game
)

# ============================================================================
# Qt6 Configuration
# ============================================================================
if(OMNICPP_USE_QT6)
    find_package(Qt6 COMPONENTS Core Gui Widgets)

    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)
        message(STATUS "Qt6 found: ${Qt6_VERSION}")
    else()
        message(FATAL_ERROR "Qt6 requested (OMNICPP_USE_QT6=ON) but not found. "
            "Please install Qt6 or set OMNICPP_USE_QT6=OFF to build without Qt6.")
    endif()
else()
    message(STATUS "Qt6 support disabled (OMNICPP_USE_QT6=OFF)")
endif()

# Vulkan is discovered by cmake/FindDependencies.cmake. Do not manufacture
# imported targets or host-specific paths here; this keeps optional builds portable.

# ============================================================================
# Vulkan Configuration
# ============================================================================
# Vulkan is configured in cmake/FindDependencies.cmake for Nix environment
# Vulkan_FOUND is set to TRUE after Qt6 configuration
# No need to check Vulkan_FOUND here

# ============================================================================
# OpenGL Configuration
# ============================================================================
if(OMNICPP_USE_OPENGL)
    find_package(OpenGL)

    if(NOT OpenGL_FOUND)
        message(WARNING "OpenGL not found, OpenGL support disabled")
        set(OMNICPP_USE_OPENGL OFF)
    endif()
endif()

# ============================================================================
# Thread Support
# ============================================================================
find_package(Threads REQUIRED)

# ============================================================================
# Build Targets
# ============================================================================
if(OMNICPP_BUILD_ENGINE)
    # S1: warploom-ui is a real module (Warploom::ui) consumed by the runtime.
    add_subdirectory(modules/ui)
    # S2: the determinism substrate is a real module (Warploom::core).
    add_subdirectory(modules/core)
    add_subdirectory(src/engine)
    # Headless proof host (P0): runs the live-proof harness in CI —
    # EditorSession + ControlServer, no GPU/window. Builds on every leg.
    add_subdirectory(tools)
endif()

if(OMNICPP_LEGACY_ENGINE)
    if(OMNICPP_BUILD_GAME)
        message(WARNING "Game target requires legacy engine source tree which has been archived.")
    endif()
endif()

if(OMNICPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OMNICPP_BUILD_EXAMPLES)
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
include(cmake/FormatTargets.cmake)
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
message(STATUS "  Windows: ${OMNICPP_PLATFORM_WINDOWS}")
message(STATUS "  Linux: ${OMNICPP_PLATFORM_LINUX}")
message(STATUS "  WASM: ${OMNICPP_PLATFORM_WASM}")
message(STATUS "")
message(STATUS "Build Targets:")
message(STATUS "  Engine: ${OMNICPP_BUILD_ENGINE}")
message(STATUS "  Game: ${OMNICPP_BUILD_GAME}")
message(STATUS "  Tests: ${OMNICPP_BUILD_TESTS}")
message(STATUS "  Examples: ${OMNICPP_BUILD_EXAMPLES}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  Vulkan: ${OMNICPP_USE_VULKAN} (${Vulkan_FOUND})")
message(STATUS "  OpenGL: ${OMNICPP_USE_OPENGL} (${OpenGL_FOUND})")
message(STATUS "  GLFW: ${OMNICPP_USE_GLFW} (${glfw_FOUND})")

if(OMNICPP_USE_QT6)
    if(Qt6_FOUND)
        message(STATUS "  Qt6: ENABLED (version ${Qt6_VERSION})")
    else()
        message(STATUS "  Qt6: REQUESTED BUT NOT FOUND")
    endif()
else()
    message(STATUS "  Qt6: DISABLED")
endif()

message(STATUS "  Quill: ${OMNICPP_USE_QUILL}")
message(STATUS "  GLM: ${OMNICPP_USE_GLM}")
message(STATUS "  STB: ${OMNICPP_USE_STB}")
message(STATUS "")
message(STATUS "Package Managers:")
message(STATUS "  Conan: ${OMNICPP_USE_CONAN}")
message(STATUS "  vcpkg: ${OMNICPP_USE_VCPKG}")
message(STATUS "  CPM: ${OMNICPP_USE_CPM}")
message(STATUS "")
message(STATUS "Code Quality:")
message(STATUS "  Coverage: ${OMNICPP_ENABLE_COVERAGE}")
message(STATUS "  Formatting: ${OMNICPP_ENABLE_FORMATTING}")
message(STATUS "  Linting: ${OMNICPP_ENABLE_LINTING}")
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
