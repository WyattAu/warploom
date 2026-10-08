cmake_minimum_required(VERSION 3.28...4.0)

# ============================================================================
# warploom-physics — the deterministic physics package (D1).
#
# Owns: PhysicsWorld (fixed-step integration, frozen-snapshot contact
# detection, uniform-grid broadphase, iterated positional correction).
# Header-only, std-only, no dependencies — consumers add the include path.
#
# In-tree:    add_subdirectory(modules/physics) from the root project.
# Standalone: find_package(WarploomPhysics CONFIG REQUIRED) after install.
#
# Split out of core (D1): physics is a distinct product surface with its own
# release cadence; core keeps the ECS/document substrate it feeds.
# ============================================================================

project(WarploomPhysics VERSION ${WARPLOOM_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(warploom_physics INTERFACE)
add_library(Warploom::physics ALIAS warploom_physics)

target_include_directories(warploom_physics INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(warploom_physics INTERFACE cxx_std_26)

install(DIRECTORY include/warploom/physics
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/warploom)

install(TARGETS warploom_physics
        EXPORT WarploomPhysicsTargets)

install(EXPORT WarploomPhysicsTargets
        NAMESPACE Warploom::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomPhysics)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WarploomPhysicsConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/WarploomPhysicsConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomPhysics)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/WarploomPhysicsConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/WarploomPhysicsConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/WarploomPhysicsConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomPhysics)