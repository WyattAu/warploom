# ============================================================================
# Warploom - Examples CMakeLists.txt
# ============================================================================
# Builds the example applications. The viewport is the only example: it is
# the windowed PBR scene that proves the shipped engine modules actually
# drive a real application.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(Examples VERSION 1.0.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The viewport is a Vulkan + XCB program. Headless legs have no window and
# no Vulkan headers, so skip it cleanly instead of failing to compile.
if(WARPLOOM_USE_VULKAN AND Vulkan_FOUND)
    add_subdirectory(viewport)
else()
    message(STATUS "Examples: viewport skipped (no Vulkan headers found)")
endif()