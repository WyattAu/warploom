cmake_minimum_required(VERSION 3.28...4.0)

# ============================================================================
# warploom-render — Vulkan rendering + scene projection (S3).
#
# Owns: Vulkan context/surface/swapchain, renderer, render graph (incl.
# parallel recorder), pipelines (graphics/compute/RT), descriptors,
# memory allocator, frame upload, offscreen, HiZ (pyramid + frame state),
# IBL baker, acceleration structures + ray query, mesh table, scene +
# scene camera, frustum math, depth convention, software rasterizer,
# UI renderer (paints Warploom::ui widget trees on the GPU).
#
# In-tree:    add_subdirectory(modules/render) from the root project.
# Standalone: find_package(WarploomRender CONFIG REQUIRED) after install.
#
# S3 is an include-path and packaging extraction only: public names stay
# `warploom::render` since S5-B phase 1a; the compat footers keep the old
# docs/warploom-render-plan.md §1). The compat forwarders under
# include/engine/render/ keep all 44 existing include sites compiling.
# ============================================================================

project(WarploomRender VERSION 0.1.0 LANGUAGES CXX)

# S3: honor the superproject's Vulkan switch (headless legs build with
# WARPLOOM_USE_VULKAN=OFF and must stay GPU-free); standalone default ON.
if(DEFINED WARPLOOM_USE_VULKAN)
    set(WARPLOOM_RENDER_USE_VULKAN_DEFAULT ${WARPLOOM_USE_VULKAN})
elseif(DEFINED OMNICPP_USE_VULKAN)
    set(WARPLOOM_RENDER_USE_VULKAN_DEFAULT ${OMNICPP_USE_VULKAN})
else()
    set(WARPLOOM_RENDER_USE_VULKAN_DEFAULT ON)
endif()
option(WARPLOOM_RENDER_USE_VULKAN "Use Vulkan graphics API" ${WARPLOOM_RENDER_USE_VULKAN_DEFAULT})

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(warploom_render SHARED
    src/warploom/render/scene_camera.cpp
    src/warploom/render/vulkan_acceleration_structure.cpp
    src/warploom/render/vulkan_compute.cpp
    src/warploom/render/vulkan_context.cpp
    src/warploom/render/vulkan_descriptors.cpp
    src/warploom/render/vulkan_frame_upload.cpp
    src/warploom/render/vulkan_hiz_pyramid.cpp
    src/warploom/render/vulkan_ibl_baker.cpp
    src/warploom/render/vulkan_memory_allocator.cpp
    src/warploom/render/vulkan_mesh_table.cpp
    src/warploom/render/vulkan_offscreen.cpp
    src/warploom/render/vulkan_parallel_recorder.cpp
    src/warploom/render/vulkan_pipeline.cpp
    src/warploom/render/vulkan_renderer.cpp
    src/warploom/render/vulkan_render_graph.cpp
    src/warploom/render/vulkan_render_pass.cpp
    src/warploom/render/vulkan_rt_pipeline.cpp
    src/warploom/render/vulkan_rt_query.cpp
    src/warploom/render/vulkan_scene.cpp
    src/warploom/render/vulkan_surface.cpp
    src/warploom/render/vulkan_swapchain.cpp
    src/warploom/render/vulkan_ui_renderer.cpp
)
add_library(Warploom::render ALIAS warploom_render)

target_compile_features(warploom_render PUBLIC cxx_std_23)
set_target_properties(warploom_render PROPERTIES
    OUTPUT_NAME warploom_render
    EXPORT_NAME render
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(warploom_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# S1: the UI renderer paints Warploom::ui widget trees (warploom/ui/widget.hpp
# in a public header, glyphs.hpp in a TU). S2: every render header includes
# warploom/core/* (deterministic_runtime x18, ecs, job_system,
# latency_telemetry) — PUBLIC so the include dirs + DSO flow to consumers.
target_link_libraries(warploom_render
    PUBLIC
        Warploom::core
        Warploom::ui
)

find_package(Threads REQUIRED)
target_link_libraries(warploom_render PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Vulkan plumbing (the S3-specific contract, docs/warploom-render-plan.md §2.2)
#
# vulkan_types.hpp includes <vulkan/vulkan.h>, so the dependency is PUBLIC;
# WARPLOOM_HAS_VULKAN gates declarations in 3 public headers
# (vulkan_types/vulkan_context/vulkan_renderer), so it is a PUBLIC compile
# definition — PRIVATE here would change what consumers see per-TU (ODR).
# xcb/win32 and their platform definitions appear in TUs only -> PRIVATE.
# Graceful degrade is preserved: with Vulkan absent the module still builds
# and every guarded TU compiles its no-Vulkan error path.
# ---------------------------------------------------------------------------

find_package(Vulkan QUIET)
if(WARPLOOM_RENDER_USE_VULKAN AND Vulkan_FOUND)
    message(STATUS "warploom-render: Vulkan found: ${Vulkan_LIBRARIES}")
    target_link_libraries(warploom_render PUBLIC Vulkan::Vulkan)
    target_compile_definitions(warploom_render PUBLIC WARPLOOM_HAS_VULKAN OMNICPP_HAS_VULKAN) # OMNICPP alias: one-window compat (S5-B phase 2)
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb)
            if(XCB_FOUND)
                target_link_libraries(warploom_render PRIVATE PkgConfig::XCB)
                target_compile_definitions(warploom_render PRIVATE VK_USE_PLATFORM_XCB_KHR=1)
            endif()
        endif()
    endif()
else()
    message(STATUS "warploom-render: Vulkan not found — rendering will return errors")
endif()

# ---------------------------------------------------------------------------
# Public header allowlist (the module's entire frozen surface)
# ---------------------------------------------------------------------------

file(GLOB WARPLOOM_RENDER_PUBLIC_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    include/warploom/render/*.hpp
)

# ---------------------------------------------------------------------------
# Install + package config
# ---------------------------------------------------------------------------

install(TARGETS warploom_render
    EXPORT WarploomRenderTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ${WARPLOOM_RENDER_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/warploom/render
)
install(EXPORT WarploomRenderTargets
    NAMESPACE Warploom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomRender
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WarploomRenderConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomRenderConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomRender
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomRenderConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomRenderConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WarploomRenderConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WarploomRender
)
