# ============================================================================
# OmniCpp Template - Project Configuration
# ============================================================================
# Defines project-wide variables and paths
# ============================================================================

# Project metadata
set(WARPLOOM_PROJECT_NAME "Warploom" CACHE STRING "Project name")
set(WARPLOOM_PROJECT_VERSION "1.0.0" CACHE STRING "Project version")
set(WARPLOOM_PROJECT_DESCRIPTION "Warploom — data-oriented C++ engine with deterministic simulation and Vulkan rendering" CACHE STRING "Project description")

# Source directories
set(WARPLOOM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Source directory")
set(WARPLOOM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "Source code directory")
set(WARPLOOM_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests" CACHE PATH "Tests directory")
set(WARPLOOM_EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/examples" CACHE PATH "Examples directory")
set(WARPLOOM_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Assets directory")

# Build directories
set(WARPLOOM_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "Build directory")
set(WARPLOOM_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin" CACHE PATH "Binary output directory")
set(WARPLOOM_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib" CACHE PATH "Library output directory")
set(WARPLOOM_OBJ_DIR "${CMAKE_CURRENT_BINARY_DIR}/obj" CACHE PATH "Object files directory")

# Installation directories
set(WARPLOOM_INSTALL_BIN_DIR "bin" CACHE STRING "Binary installation directory")
set(WARPLOOM_INSTALL_LIB_DIR "lib" CACHE STRING "Library installation directory")
set(WARPLOOM_INSTALL_INCLUDE_DIR "include" CACHE STRING "Header installation directory")
set(WARPLOOM_INSTALL_DATA_DIR "share/${WARPLOOM_PROJECT_NAME}" CACHE STRING "Data installation directory")
set(WARPLOOM_INSTALL_DOC_DIR "share/doc/${WARPLOOM_PROJECT_NAME}" CACHE STRING "Documentation installation directory")
set(WARPLOOM_INSTALL_CMAKE_DIR "lib/cmake/${WARPLOOM_PROJECT_NAME}" CACHE STRING "CMake config installation directory")

# C++ standard
# Using C++23 as per ADR-016: C++23 Without Modules
set(WARPLOOM_CPP_STANDARD "23" CACHE STRING "C++ standard to use")
set_property(CACHE WARPLOOM_CPP_STANDARD PROPERTY STRINGS "23")

# Set C++ standard
set(CMAKE_CXX_STANDARD ${WARPLOOM_CPP_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Validate configuration
if(NOT EXISTS "${WARPLOOM_SOURCE_DIR}")
    message(FATAL_ERROR "Source directory does not exist: ${WARPLOOM_SOURCE_DIR}")
endif()

message(STATUS "Project configuration loaded successfully")
