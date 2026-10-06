cmake_minimum_required(VERSION 3.28...4.0)

# ============================================================================
# warploom-ui — retained widget tree, deterministic layout, paint list,
# bitmap font. Std-only; no engine dependencies. Pilot module of the
# S-track split (docs/rename-and-modules.md, docs/roadmap.md).
#
# In-tree:  add_subdirectory(modules/ui) from the root project.
# Standalone: find_package(WarploomUI CONFIG REQUIRED) after installation.
# ============================================================================

project(WarploomUI VERSION ${WARPLOOM_VERSION} LANGUAGES CXX)

option(WARPLOOM_UI_BUILD_TESTS "Build warploom-ui module tests" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(warploom_ui SHARED
    src/warploom/ui/widget.cpp
    src/warploom/ui/glyphs.cpp
)
add_library(Warploom::ui ALIAS warploom_ui)

target_compile_features(warploom_ui PUBLIC cxx_std_23)
set_target_properties(warploom_ui PROPERTIES
    OUTPUT_NAME warploom_ui
    EXPORT_NAME ui
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(warploom_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

find_package(Threads REQUIRED)
target_link_libraries(warploom_ui PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Public header allowlist (the module's entire frozen surface)
# ---------------------------------------------------------------------------

set(WARPLOOM_UI_PUBLIC_HEADERS
    include/warploom/ui/widget.hpp
    include/warploom/ui/glyphs.hpp
)

# ---------------------------------------------------------------------------
# Module tests (run in-tree; standalone consumers use the root suite)
# ---------------------------------------------------------------------------

if(WARPLOOM_UI_BUILD_TESTS AND (NOT CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME))
    # googletest is provided by the superproject (CPM); guard for standalone.
    if(TARGET gtest)
        enable_testing()
        add_executable(warploom_ui_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_widget.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_glyphs.cpp
        )
        target_link_libraries(warploom_ui_tests PRIVATE warploom_ui gtest gtest_main)
        # Third-party headers included as SYSTEM: warnings raised inside
        # GoogleTest are not actionable here, and letting them through masks
        # warnings in our own code -- which happened repeatedly during the
        # warning-debt cleanup.
        get_target_property(WARPLOOM_GTEST_INCLUDES gtest INTERFACE_INCLUDE_DIRECTORIES)
        if(WARPLOOM_GTEST_INCLUDES)
            target_include_directories(warploom_ui_tests SYSTEM PRIVATE ${WARPLOOM_GTEST_INCLUDES})
        endif()
        target_include_directories(warploom_ui_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME WarploomUiTests COMMAND warploom_ui_tests)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install + package config
# ---------------------------------------------------------------------------

install(TARGETS warploom_ui
    EXPORT WarploomUITargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${WARPLOOM_UI_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/warploom/ui
)
install(EXPORT WarploomUITargets
    NAMESPACE Warploom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomUI
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WarploomUIConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomUIConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomUI
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomUIConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomUIConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomUIConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomUI
)
