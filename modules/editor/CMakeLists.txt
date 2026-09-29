cmake_minimum_required(VERSION 3.28...4.0)

# ============================================================================
# warploom-editor — the widget/view layer over core + ui (S4, phase A).
#
# Owns: NodeEditorView (graph -> widget-tree projection, wire routing,
# hit-testing, link-drag/param-edit state machines), InspectorPanel
# (outliner/properties/bindings projection), GraphSignalAdapter (graph
# outputs -> animation actions). Pure view layer: consumes Warploom::core
# (document/graph/session) and Warploom::ui (widgets); zero render/asset
# dependencies (docs/warploom-editor-plan.md).
#
# In-tree:    add_subdirectory(modules/editor) from the root project.
# Standalone: find_package(WarploomEditor CONFIG REQUIRED) after install.
#
# Phase A keeps the omnicpp::* namespaces (identifier migration is the
# S2-B / S-decision pass); the compat forwarders under
# include/engine/editor/ keep every existing include site compiling.
# ============================================================================

project(WarploomEditor VERSION 0.1.0 LANGUAGES CXX)

option(WARPLOOM_EDITOR_BUILD_TESTS "Build warploom-editor module tests" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(warploom_editor SHARED
    src/warploom/editor/node_editor.cpp
    src/warploom/editor/inspector.cpp
    src/warploom/editor/clip_timeline_view.cpp
)
add_library(Warploom::editor ALIAS warploom_editor)

target_compile_features(warploom_editor PUBLIC cxx_std_23)
set_target_properties(warploom_editor PROPERTIES
    OUTPUT_NAME warploom_editor
    EXPORT_NAME editor
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(warploom_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

find_package(Threads REQUIRED)
# PUBLIC: the public headers include warploom/ui/widget.hpp and the
# warploom/core document/graph/session headers (via forwarders in-tree).
target_link_libraries(warploom_editor
    PUBLIC
        Warploom::ui
        Warploom::core
        Threads::Threads
)

# ---------------------------------------------------------------------------
# Public header allowlist (the module's entire frozen surface)
# ---------------------------------------------------------------------------

file(GLOB WARPLOOM_EDITOR_PUBLIC_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    include/warploom/editor/*.hpp
)

# ---------------------------------------------------------------------------
# Module tests (run in-tree; standalone consumers use the root suite)
# ---------------------------------------------------------------------------

if(WARPLOOM_EDITOR_BUILD_TESTS AND (NOT CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME))
    # googletest is provided by the superproject (CPM); guard for standalone.
    if(TARGET gtest)
        enable_testing()
        add_executable(warploom_editor_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_node_editor.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_inspector.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_graph_anim_bridge.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_clip_timeline_view.cpp
        )
        target_link_libraries(warploom_editor_tests PRIVATE warploom_editor warploom_render gtest gtest_main)
        target_include_directories(warploom_editor_tests
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                # test_node_editor's two golden-pixel tests include the
                # software rasterizer header (header-only, now in
                # modules/render's include root post-S3; the include is
                # spelled engine/render/software_rasterizer.hpp via the
                # superproject forwarder). In-tree only: the gtest guard
                # above keeps this block out of standalone configures, and
                # module smoke tests never depend on the rasterizer.
                ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        )
        add_test(NAME WarploomEditorTests COMMAND warploom_editor_tests)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install + package config
# ---------------------------------------------------------------------------

install(TARGETS warploom_editor
    EXPORT WarploomEditorTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${WARPLOOM_EDITOR_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/warploom/editor
)
install(EXPORT WarploomEditorTargets
    NAMESPACE Warploom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomEditor
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WarploomEditorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEditorConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomEditor
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEditorConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEditorConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomEditorConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomEditor
)
