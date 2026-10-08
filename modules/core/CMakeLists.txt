cmake_minimum_required(VERSION 3.28...4.0)

# ============================================================================
# warploom-core — the determinism substrate (S2, phase A).
#
# Owns: deterministic runtime, document/JSON, ECS/physics/animation,
# node graph, scrubber/recorder, protocol + editor session. Headless,
# std-only, no ui/render/asset dependencies (docs/warploom-core-plan.md).
#
# In-tree:    add_subdirectory(modules/core) from the root project.
# Standalone: find_package(WarploomCore CONFIG REQUIRED) after install.
#
# Public headers are spellable as warploom/core/*.hpp. The Phase-A compat
# forwarders under include/engine/core/ are gone: every in-tree consumer
# names the real path, so the superproject has no include shim at all.
# ============================================================================

project(WarploomCore VERSION ${WARPLOOM_VERSION} LANGUAGES CXX)

option(WARPLOOM_CORE_BUILD_TESTS "Build warploom-core module tests" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(warploom_core SHARED
    src/warploom/core/command_recorder.cpp
    src/warploom/core/control_server.cpp
    src/warploom/core/diagnostics.cpp
    src/warploom/core/document.cpp
    src/warploom/core/document_projection.cpp
    src/warploom/core/editor_session.cpp
    src/warploom/core/engine.cpp
    src/warploom/core/input_state.cpp
    src/warploom/core/input_translators.cpp
    src/warploom/core/node_graph.cpp
    src/warploom/core/property_registry.cpp
    src/warploom/core/replay_scrubber.cpp
    src/warploom/core/script_module.cpp
    src/warploom/core/script_node.cpp
)
add_library(Warploom::core ALIAS warploom_core)

target_compile_features(warploom_core PUBLIC cxx_std_23)
set_target_properties(warploom_core PROPERTIES
    OUTPUT_NAME warploom_core
    EXPORT_NAME core
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(warploom_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

find_package(Threads REQUIRED)
# Physics is its own package (D1); core's session tick consumes it, so the
# include path travels to every core consumer.
target_link_libraries(warploom_core PUBLIC Threads::Threads Warploom::physics)

# ---------------------------------------------------------------------------
# Public header allowlist (the module's entire frozen surface)
# ---------------------------------------------------------------------------

file(GLOB WARPLOOM_CORE_PUBLIC_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    include/warploom/core/*.hpp
)

# ---------------------------------------------------------------------------
# Module tests (run in-tree; standalone consumers use the root suite)
# ---------------------------------------------------------------------------

if(WARPLOOM_CORE_BUILD_TESTS AND (NOT CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME))
    # googletest is provided by the superproject (CPM); guard for standalone.
    if(TARGET gtest)
        enable_testing()
        add_executable(warploom_core_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_document.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scrubber.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_timeline.cpp
        )
        target_link_libraries(warploom_core_tests PRIVATE warploom_core gtest gtest_main)
        # Third-party headers included as SYSTEM: warnings raised inside
        # GoogleTest are not actionable here, and letting them through masks
        # warnings in our own code -- which happened repeatedly during the
        # warning-debt cleanup.
        get_target_property(WARPLOOM_GTEST_INCLUDES gtest INTERFACE_INCLUDE_DIRECTORIES)
        if(WARPLOOM_GTEST_INCLUDES)
            target_include_directories(warploom_core_tests SYSTEM PRIVATE ${WARPLOOM_GTEST_INCLUDES})
        endif()
        target_include_directories(warploom_core_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME WarploomCoreTests COMMAND warploom_core_tests)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install + package config
# ---------------------------------------------------------------------------

install(TARGETS warploom_core
    EXPORT WarploomCoreTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${WARPLOOM_CORE_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/warploom/core
)
install(EXPORT WarploomCoreTargets
    NAMESPACE Warploom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomCore
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WarploomCoreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomCoreConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomCore
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomCoreConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomCoreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomCoreConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomCore
)
