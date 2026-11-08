cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Warploom Viewport — windowed PBR scene application
# ============================================================================
# This is the app that proves the shipped engine modules actually drive a
# real application. It is a Vulkan + XCB program, so it is only configured
# when Vulkan headers are present: the headless CI legs have no use for a
# window and previously failed to compile here instead of being skipped.

add_executable(warploom_viewport main.cpp)
set_target_properties(warploom_viewport PROPERTIES OUTPUT_NAME warploom_viewport)  # S5-B phase 2 binary rename

# S5-A: the aggregate (Warploom::engine) replaces the deleted
# warploom_runtime monolith. Module headers arrive through each module's
# PUBLIC interface include directory; main.cpp and telemetry.hpp spell
# the real warploom/... paths (no superproject include shim).
target_link_libraries(warploom_viewport PRIVATE Warploom::engine)
target_compile_features(warploom_viewport PRIVATE cxx_std_26)

# The app owns a window, so it needs the XCB headers itself. warploom_render
# links xcb PRIVATE (correct for its own TUs), which does not propagate the
# include dirs — main.cpp includes <xcb/xcb.h> directly.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
target_link_libraries(warploom_viewport PRIVATE PkgConfig::XCB)
target_compile_definitions(warploom_viewport PRIVATE VK_USE_PLATFORM_XCB_KHR=1)

# Wayland backend (loop69, E6): xdg-shell client bindings are generated at
# build time from the vendored stable XML (see wayland-protocol/README.md)
# with wayland-scanner, viewport-only. Absent scanner/client lib degrades
# gracefully: the viewport stays XCB-only on that machine.
find_program(WAYLAND_SCANNER wayland-scanner)
if(WAYLAND_SCANNER)
    pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
endif()
if(WAYLAND_SCANNER AND WAYLAND_CLIENT_FOUND)
    set(WAYLAND_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wayland-protocol)
    set(WAYLAND_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-gen)
    file(MAKE_DIRECTORY ${WAYLAND_GEN_DIR})
    add_custom_command(
        OUTPUT ${WAYLAND_GEN_DIR}/xdg-shell-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header
            ${WAYLAND_PROTOCOL_DIR}/xdg-shell.xml
            ${WAYLAND_GEN_DIR}/xdg-shell-client-protocol.h
        DEPENDS ${WAYLAND_PROTOCOL_DIR}/xdg-shell.xml
        COMMENT "Generating xdg-shell client header")
    add_custom_command(
        OUTPUT ${WAYLAND_GEN_DIR}/xdg-shell-protocol.cpp
        COMMAND ${WAYLAND_SCANNER} private-code
            ${WAYLAND_PROTOCOL_DIR}/xdg-shell.xml
            ${WAYLAND_GEN_DIR}/xdg-shell-protocol.cpp
        DEPENDS ${WAYLAND_PROTOCOL_DIR}/xdg-shell.xml
        COMMENT "Generating xdg-shell protocol code")
    # The header has no TU depending on it explicitly (it is only
    # #included), so anchor both outputs to a target the viewport waits
    # on — otherwise main.cpp can compile before the header exists.
    add_custom_target(wayland_xdg_shell_gen DEPENDS
        ${WAYLAND_GEN_DIR}/xdg-shell-client-protocol.h
        ${WAYLAND_GEN_DIR}/xdg-shell-protocol.cpp)
    add_dependencies(warploom_viewport wayland_xdg_shell_gen)
    # Scanner output is C: in C++ its namespace-scope consts get internal
    # linkage, go unreferenced, and are never emitted — leaving main.cpp's
    # extern references dangling at link time (measured). The project is
    # CXX-only, so enable C for this generated file alone.
    enable_language(C)
    set_source_files_properties(${WAYLAND_GEN_DIR}/xdg-shell-protocol.cpp
        PROPERTIES LANGUAGE C)
    target_sources(warploom_viewport PRIVATE
        ${WAYLAND_GEN_DIR}/xdg-shell-protocol.cpp)
    # SYSTEM: scanner output is third-party code (old-style casts
    # throughout); same treatment as the gtest headers.
    target_include_directories(warploom_viewport SYSTEM PRIVATE ${WAYLAND_GEN_DIR})
    target_link_libraries(warploom_viewport PRIVATE PkgConfig::WAYLAND_CLIENT)
    # Seat keyboard (loop70): xkbcommon decodes the compositor keymap into
    # keysyms (printable chars) and modifiers (Ctrl gate). Viewport-only.
    pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
    target_link_libraries(warploom_viewport PRIVATE PkgConfig::XKBCOMMON)
    target_compile_definitions(warploom_viewport PRIVATE VK_USE_PLATFORM_WAYLAND_KHR=1)
else()
    message(STATUS "warploom-viewport: Wayland backend disabled (need wayland-scanner + wayland-client)")
endif()

# Shaders: the app depends on the same root-scope target the tests use, and
# receives its output directory as a compile definition. This is what
# removed the "build it, set WARPLOOM_SHADER_DIR by hand, hope it works"
# step from the 0.1 clone-and-build gate.
if(TARGET warploom_shaders)
    add_dependencies(warploom_viewport warploom_shaders)
    target_compile_definitions(warploom_viewport PRIVATE
        WARPLOOM_SHADER_DIR="${WARPLOOM_SHADER_OUTPUT_DIR}"
        WARPLOOM_INSTALL_SHADERS_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/shaders")
else()
    message(WARNING
        "Warploom: viewport built without the warploom_shaders target — no "
        "SPIR-V compiler was found. It will not run. Install glslc or "
        "glslang-tools.")
    target_compile_definitions(warploom_viewport PRIVATE
        WARPLOOM_INSTALL_SHADERS_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/shaders")
endif()

install(TARGETS warploom_viewport RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)