cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Warploom Viewport — windowed PBR scene application
# ============================================================================
# This is the app that proves the shipped engine modules actually drive a
# real application. It is a Vulkan + XCB program, so it is only configured
# when Vulkan headers are present: the headless CI legs have no use for a
# window and previously failed to compile here instead of being skipped.

add_executable(omnicpp_viewport main.cpp)
set_target_properties(omnicpp_viewport PROPERTIES OUTPUT_NAME warploom_viewport)  # S5-B phase 2 binary rename

# S5-A: the aggregate (Warploom::engine) replaces the deleted
# omnicpp_runtime monolith. Module headers arrive through each module's
# PUBLIC interface include directory; main.cpp and telemetry.hpp spell
# the real warploom/... paths (no superproject include shim).
target_link_libraries(omnicpp_viewport PRIVATE Warploom::engine)
target_compile_features(omnicpp_viewport PRIVATE cxx_std_26)

# The app owns a window, so it needs the XCB headers itself. warploom_render
# links xcb PRIVATE (correct for its own TUs), which does not propagate the
# include dirs — main.cpp includes <xcb/xcb.h> directly.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
target_link_libraries(omnicpp_viewport PRIVATE PkgConfig::XCB)
target_compile_definitions(omnicpp_viewport PRIVATE VK_USE_PLATFORM_XCB_KHR=1)

# Shaders: the app depends on the same root-scope target the tests use, and
# receives its output directory as a compile definition. This is what
# removed the "build it, set WARPLOOM_SHADER_DIR by hand, hope it works"
# step from the 0.1 clone-and-build gate.
if(TARGET warploom_shaders)
    add_dependencies(omnicpp_viewport warploom_shaders)
    target_compile_definitions(omnicpp_viewport PRIVATE
        WARPLOOM_SHADER_DIR="${WARPLOOM_SHADER_OUTPUT_DIR}"
        WARPLOOM_INSTALL_SHADERS_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/shaders")
else()
    message(WARNING
        "Warploom: viewport built without the warploom_shaders target — no "
        "SPIR-V compiler was found. It will not run. Install glslc or "
        "glslang-tools.")
    target_compile_definitions(omnicpp_viewport PRIVATE
        WARPLOOM_INSTALL_SHADERS_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/shaders")
endif()

install(TARGETS omnicpp_viewport RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)