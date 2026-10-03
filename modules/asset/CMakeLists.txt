cmake_minimum_required(VERSION 3.28...4.0)

# ============================================================================
# warploom-asset — asset import (S3.5).
#
# Owns: glTF/GLB mesh + scene import, skeletal animation import, and the
# strict self-contained PNG/JPEG/KTX2 decoders (no external image
# libraries — the in-tree decoders are a determinism feature). The
# internal glTF JSON/parser infrastructure (gltf_json.hpp) is TU-private
# and not part of the public surface.
#
# In-tree:    add_subdirectory(modules/asset) from the root project.
# Standalone: find_package(WarploomAsset CONFIG REQUIRED) after install.
#
# S3.5 is an include-path and packaging extraction only: public names
# re-badged to `warploom::asset` in S5-B phase 1a (see
# docs/warploom-asset-plan.md §1). Public headers are spellable as
# warploom/asset/*.hpp; the compat forwarders are gone.
# ============================================================================

project(WarploomAsset VERSION ${WARPLOOM_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(warploom_asset SHARED
    src/warploom/asset/gltf_animation.cpp
    src/warploom/asset/gltf_importer.cpp
    src/warploom/asset/image_decode.cpp
    src/warploom/asset/jpeg_decoder.cpp
    src/warploom/asset/ktx2_decoder.cpp
    src/warploom/asset/png_decoder.cpp
)
add_library(Warploom::asset ALIAS warploom_asset)

target_compile_features(warploom_asset PUBLIC cxx_std_23)
set_target_properties(warploom_asset PROPERTIES
    OUTPUT_NAME warploom_asset
    EXPORT_NAME asset
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(warploom_asset
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Core is the only module dependency (deterministic_runtime + RuntimeError
# in headers/TUs). No Vulkan/platform/codec-library plumbing at all.
target_link_libraries(warploom_asset PUBLIC Warploom::core)

find_package(Threads REQUIRED)
target_link_libraries(warploom_asset PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Public header allowlist (the module's entire frozen surface)
# ---------------------------------------------------------------------------

file(GLOB WARPLOOM_ASSET_PUBLIC_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    include/warploom/asset/*.hpp
)

# ---------------------------------------------------------------------------
# Install + package config
# ---------------------------------------------------------------------------

install(TARGETS warploom_asset
    EXPORT WarploomAssetTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${WARPLOOM_ASSET_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/warploom/asset
)
install(EXPORT WarploomAssetTargets
    NAMESPACE Warploom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomAsset
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WarploomAssetConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomAssetConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomAsset
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomAssetConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomAssetConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomAssetConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomAsset
)
