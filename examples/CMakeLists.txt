# ============================================================================
# Warploom - Examples CMakeLists.txt
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

# Pong Game Example (Vulkan rendering; requires Qt6 + quill). Includes the
# archived legacy headers (engine/graphics/renderer.hpp), which no longer
# exist in this tree, so it is opt-in until it is ported to the current
# engine modules.
option(OMNICPP_BUILD_PONG "Build the legacy pong example" OFF)
if(OMNICPP_BUILD_PONG)
    if(Qt6_FOUND AND quill_FOUND)
        add_subdirectory(pong)
    else()
        message(STATUS "Examples: pong requested but Qt6/quill unavailable")
    endif()
else()
    message(STATUS "Examples: pong skipped (legacy headers; -DOMNICPP_BUILD_PONG=ON to force)")
endif()

# Viewport (windowed PBR scene on the current engine stack)
add_subdirectory(viewport)
