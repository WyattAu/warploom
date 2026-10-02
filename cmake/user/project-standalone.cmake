# Licensed under the Apache License, Version 2.0 (the "License");

# ==============================================================================
# STANDALONE-SPECIFIC CONFIGURATION
# ==============================================================================
include(${CMAKE_CURRENT_LIST_DIR}/../user/project-common.cmake)

project(
    ${STANDALONE_NAME}
    LANGUAGES C CXX
    DESCRIPTION "template"
    HOMEPAGE_URL "https://github.com/your-org/your-repo")

# ==============================================================================
# Build guards
# ==============================================================================
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(
        WARNING
            "In-source builds. Please make a new directory (called a Build directory) and run CMake from there."
    )
endif()

# ==============================================================================
# Standalone dependencies
# ==============================================================================
# Note: TestLib target should be available from orchestrator
# Dependencies for library headers
# Skip find_package(fmt) when using Conan as it's broken for MSVC
# find_package(fmt QUIET)
# if(NOT fmt_FOUND)
    CPMAddPackage("gh:fmtlib/fmt#11.2.0")
# endif()

# Qt6 and Vulkan dependencies via vcpkg (only for non-Emscripten builds and when Qt/Vulkan standalone is enabled)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten" AND BUILD_QT_VULKAN_STANDALONE)
    # Find Qt6 from vcpkg
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)
        message(STATUS "Qt6 found via vcpkg")
    else()
        message(WARNING "Qt6 not found via vcpkg. Standalone will be built without Qt support.")
    endif()

    # Find Vulkan from vcpkg
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        message(STATUS "Vulkan found via vcpkg")
    else()
        message(WARNING "Vulkan not found via vcpkg. Qt/Vulkan support will be limited.")
    endif()
endif()

CPMAddPackage("gh:cpm-cmake/CPMLicenses.cmake@0.0.7")
cpm_licenses_create_disclaimer_target(
    write-licenses-${STANDALONE_NAME}
    "${CMAKE_CURRENT_BINARY_DIR}/${STANDALONE_NAME}_third_party.txt" "${CPM_PACKAGES}")
CPMAddPackage(
    GITHUB_REPOSITORY jarro2783/cxxopts
    VERSION 3.2.1
    OPTIONS "CXXOPTS_BUILD_EXAMPLES NO" "CXXOPTS_BUILD_TESTS NO" "CXXOPTS_ENABLE_INSTALL NO")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    CPMAddPackage(
        NAME cpptrace
        GITHUB_REPOSITORY jeremy-rifkin/cpptrace
        VERSION 1.0.4)
endif()

# ==============================================================================
# Standalone source files
# ==============================================================================
if(BUILD_QT_VULKAN_STANDALONE)
    gather_sources(sources ${CMAKE_SOURCE_DIR}/targets/qt-vulkan/standalone/src)
    list(APPEND sources ${CMAKE_SOURCE_DIR}/targets/qt-vulkan/standalone/src/Main.cpp)
else()
    # Add main.cpp from src directory for basic standalone demo
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.cpp)
        set(sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
    endif()
    # Add C++ modules
    file(GLOB_RECURSE module_sources "${CMAKE_SOURCE_DIR}/src/modules/*.cppm")
    list(APPEND sources ${module_sources})
endif()

# ==============================================================================
# Create standalone target
# ==============================================================================
add_executable(${STANDALONE_NAME})
target_sources(${STANDALONE_NAME} PRIVATE ${sources})

# Apply common target settings
apply_common_target_settings(${STANDALONE_NAME})

# Ensure filesystem is available for MSVC
if(MSVC)
    target_compile_options(${STANDALONE_NAME} PRIVATE /std:c++23)
endif()

# Include library headers and source headers
target_include_directories(${STANDALONE_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)

# Include Qt/Vulkan library headers if available
if(TARGET ${QT_VULKAN_LIBRARY_NAME})
    target_include_directories(${STANDALONE_NAME} PRIVATE $<TARGET_PROPERTY:${QT_VULKAN_LIBRARY_NAME},INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Conditionally enable Qt GUI support based on Qt6 availability
if(Qt6_FOUND)
    target_compile_definitions(${STANDALONE_NAME} PRIVATE WARPLOOM_QT_VULKAN_AVAILABLE=1)
    target_compile_definitions(${STANDALONE_NAME} PRIVATE QT_GUI_ENABLED=1)
else()
    target_compile_definitions(${STANDALONE_NAME} PRIVATE WARPLOOM_QT_VULKAN_AVAILABLE=0)
    target_compile_definitions(${STANDALONE_NAME} PRIVATE QT_GUI_ENABLED=0)
endif()

# Link with dependencies
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_link_libraries(${STANDALONE_NAME} PRIVATE fmt::fmt nlohmann_json::nlohmann_json cxxopts::cxxopts cpptrace::cpptrace)
    # Link Qt libraries if available
    if(Qt6_FOUND)
        target_link_libraries(${STANDALONE_NAME} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
        if(Vulkan_FOUND)
            target_link_libraries(${STANDALONE_NAME} PRIVATE Qt6::VulkanSupport Vulkan::Vulkan)
        endif()
    endif()
else()
    target_link_libraries(${STANDALONE_NAME} PRIVATE fmt::fmt nlohmann_json::nlohmann_json cxxopts::cxxopts)
endif()

# Link to Qt/Vulkan library if Qt is enabled and library is built
if(Qt6_FOUND AND TARGET ${QT_VULKAN_LIBRARY_NAME})
    target_link_libraries(${STANDALONE_NAME} PRIVATE ${QT_VULKAN_LIBRARY_NAME})
endif()

# Link to library if built
if(TARGET ${LIBRARY_NAME})
    target_link_libraries(${STANDALONE_NAME} PRIVATE ${LIBRARY_NAME})
endif()

# ==============================================================================
# Asset processing and Emscripten configuration
# ==============================================================================
include(${CMAKE_CURRENT_LIST_DIR}/../user/tmplt-assets.cmake)
apply_assets_processing_standalone()

include(${CMAKE_CURRENT_LIST_DIR}/../user/tmplt-emscripten.cmake)
emscripten(${STANDALONE_NAME} 1 1 "")

# ==============================================================================
# Tests configuration
# ==============================================================================
if(ENABLE_TESTS)
    message(STATUS "Tests enabled with Catch2")
    include(CTest) # Enable testing at the root level
    add_library(${TEST_NAME_LOWER}_standalone_common INTERFACE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        target_link_libraries(${TEST_NAME_LOWER}_standalone_common
                             INTERFACE fmt::fmt nlohmann_json::nlohmann_json cxxopts::cxxopts cpptrace::cpptrace)
        if(TARGET Vulkan::Vulkan)
            target_link_libraries(${TEST_NAME_LOWER}_standalone_common INTERFACE Vulkan::Vulkan)
        endif()
    else()
        target_link_libraries(${TEST_NAME_LOWER}_standalone_common INTERFACE ${LIBRARY_NAME}
                                                                             cxxopts::cxxopts)
        if(TARGET Vulkan::Vulkan)
            target_link_libraries(${TEST_NAME_LOWER}_standalone_common INTERFACE Vulkan::Vulkan)
        endif()
    endif()

    # Core standalone tests are console-only, no Qt/Vulkan dependencies

    add_library(omnicpp::${TEST_NAME_LOWER}_standalone_common ALIAS
                ${TEST_NAME_LOWER}_standalone_common)
    if(BUILD_QT_VULKAN_STANDALONE AND EXISTS ${CMAKE_SOURCE_DIR}/targets/qt-vulkan/standalone/tests)
        add_subdirectory(${CMAKE_SOURCE_DIR}/targets/qt-vulkan/standalone/tests tests)
    endif()

    # Add main tests directory
    if(EXISTS ${CMAKE_SOURCE_DIR}/tests)
        add_subdirectory(${CMAKE_SOURCE_DIR}/tests main_tests)
    endif()
endif()

# ==============================================================================
# Installation
# ==============================================================================
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})

# Standard installation for native platforms
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    install(TARGETS ${STANDALONE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    # Special installation for Emscripten - install all generated files
    install(TARGETS ${STANDALONE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Install additional Emscripten files (js, wasm, data) These files are generated alongside the
    # .html file
    get_target_property(TARGET_SUFFIX ${STANDALONE_NAME} SUFFIX)
    if(TARGET_SUFFIX STREQUAL ".html")
        set(BASE_NAME "${STANDALONE_NAME}")

        # Install .js file (JavaScript wrapper)
        install(
            FILES "${CMAKE_CURRENT_BINARY_DIR}/bin/${BASE_NAME}.js"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            OPTIONAL)

        # Install .wasm file (WebAssembly binary)
        install(
            FILES "${CMAKE_CURRENT_BINARY_DIR}/bin/${BASE_NAME}.wasm"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            OPTIONAL)

        # Install .data file (preloaded assets)
        install(
            FILES "${CMAKE_CURRENT_BINARY_DIR}/bin/${BASE_NAME}.data"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            OPTIONAL)

        # Install .wasm.map file (WebAssembly source map for debugging)
        install(
            FILES "${CMAKE_CURRENT_BINARY_DIR}/bin/${BASE_NAME}.wasm.map"
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            OPTIONAL)
    endif()
endif()
