cmake_minimum_required(VERSION 3.28...4.0)

# ============================================================================
# warploom-engine — the aggregate (S5-A).
#
# NOT a library: an INTERFACE target over the five real modules, so
# downstream consumers do find_package(WarploomEngine CONFIG REQUIRED)
# + link Warploom::engine and get every module's usage requirements
# transitively. No headers, no DSO (rename-and-modules.md §5: "deps:
# all of the above; the aggregate").
#
# In-tree:    add_subdirectory(modules/engine) from the root project.
# Standalone: find_package(WarploomEngine CONFIG REQUIRED) after install.
#
# S5-A deliberately changes no identifiers, no forwarder spellings, and
# no module surfaces — it deletes the last monolith target
# (omnicpp_runtime) and flips the in-tree consumers to the aggregate.
# Identity migration (namespaces, flags, binaries, ABI shim, repo name)
# is S5-B (docs/warploom-engine-plan.md §6).
# ============================================================================

project(WarploomEngine VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(warploom_engine INTERFACE)
set_target_properties(warploom_engine PROPERTIES EXPORT_NAME engine)
add_library(Warploom::engine ALIAS warploom_engine)

target_compile_features(warploom_engine INTERFACE cxx_std_23)

# The five real modules, in dependency order. Threads flows transitively
# from core/ui/asset; Vulkan from render (with the PUBLIC
# WARPLOOM_HAS_VULKAN definition that gates render public headers).
target_link_libraries(warploom_engine
    INTERFACE
        Warploom::core
        Warploom::ui
        Warploom::editor
        Warploom::render
        Warploom::asset
)

# ---------------------------------------------------------------------------
# Install + package config (no DSO, no headers — targets export only)
# ---------------------------------------------------------------------------

install(TARGETS warploom_engine
    EXPORT WarploomEngineTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT WarploomEngineTargets
    NAMESPACE Warploom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomEngine
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WarploomEngineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEngineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomEngine
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEngineConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEngineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEngineConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomEngine
)
