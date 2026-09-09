# ============================================================================
# OmniCpp Template - Examples CMakeLists.txt
# ============================================================================
# Builds example applications. The pong example requires Qt6 + quill; it is
# skipped when those dependencies are unavailable (the default in the
# engine presets) so the viewport can build standalone.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(Examples VERSION 1.0.0 LANGUAGES CXX)

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
find_package(quill QUIET)

# C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Simple Game Example (requires the archived legacy engine headers; it was
# already broken before the viewport work and cannot build in this tree)
# add_subdirectory(simple_game)

# Pong Game Example (Vulkan rendering; requires Qt6 + quill)
if(Qt6_FOUND AND quill_FOUND)
    add_subdirectory(pong)
else()
    message(STATUS "Examples: pong skipped (Qt6/quill unavailable)")
endif()

# Viewport (windowed PBR scene on the current engine stack)
add_subdirectory(viewport)
