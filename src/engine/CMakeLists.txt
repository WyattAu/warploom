cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Canonical deterministic runtime
# This is the core engine target with NO graphics dependencies.
# ============================================================================

add_library(omnicpp_runtime SHARED
    editor/node_editor.cpp
    editor/inspector.cpp
    render/vulkan_ibl_baker.cpp
    asset/gltf_importer.cpp
    asset/gltf_animation.cpp
    asset/image_decode.cpp
    asset/ktx2_decoder.cpp
    asset/png_decoder.cpp
    asset/jpeg_decoder.cpp
    render/vulkan_context.cpp
    render/vulkan_surface.cpp
    render/vulkan_swapchain.cpp
    render/vulkan_render_pass.cpp
    render/vulkan_pipeline.cpp
    render/vulkan_renderer.cpp
    render/vulkan_mesh_table.cpp
    render/vulkan_scene.cpp
    render/scene_camera.cpp
    render/vulkan_frame_upload.cpp
    render/vulkan_offscreen.cpp
    render/vulkan_ui_renderer.cpp
    render/vulkan_memory_allocator.cpp
    render/vulkan_descriptors.cpp
    render/vulkan_render_graph.cpp
    render/vulkan_compute.cpp
    render/vulkan_hiz_pyramid.cpp
    render/vulkan_acceleration_structure.cpp
    render/vulkan_rt_query.cpp
    render/vulkan_rt_pipeline.cpp
    render/vulkan_parallel_recorder.cpp
)
set_target_properties(omnicpp_runtime PROPERTIES OUTPUT_NAME omnicpp_runtime)
target_include_directories(omnicpp_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# S1: the UI toolkit lives in its own module (modules/ui) and is consumed
# as Warploom::ui. PUBLIC: editor/render headers include warploom/ui/*.
# S2: the determinism substrate lives in modules/core as Warploom::core;
# PUBLIC for the same reason (editor/render/engine headers include
# warploom/core/* — via the include/engine/core forwarders today).
target_link_libraries(omnicpp_runtime PUBLIC Warploom::ui Warploom::core)

# ============================================================================
# Module-alias libraries (publishing units)
#
# omnicpp_runtime is ONE shared object (one ABI, one dlopen surface), but
# downstream projects consume it through these alias targets. S2 phase A:
# warploom-core is now a REAL library (modules/core, Warploom::core) and
# omnicpp_runtime links it PUBLIC — the runtime itself shrinks to the
# integration layer (editor widgets + render + asset).
# ============================================================================

# (CMake forbids alias-of-alias, so these point at the real module targets.)
add_library(omnicpp_core ALIAS warploom_core)
add_library(omnicpp_ui ALIAS warploom_ui)
add_library(omnicpp_render_lib ALIAS omnicpp_runtime)

# Link Vulkan if available (gracefully returns errors when absent)
find_package(Vulkan QUIET)
if(OMNICPP_USE_VULKAN AND Vulkan_FOUND)
    message(STATUS "Vulkan found: ${Vulkan_LIBRARIES}")
    target_link_libraries(omnicpp_runtime PUBLIC Vulkan::Vulkan)
    target_compile_definitions(omnicpp_runtime PUBLIC OMNICPP_HAS_VULKAN)
    if(UNIX AND NOT APPLE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb)
            if(XCB_FOUND)
                target_link_libraries(omnicpp_runtime PUBLIC PkgConfig::XCB)
                target_compile_definitions(omnicpp_runtime PUBLIC VK_USE_PLATFORM_XCB_KHR=1)
            endif()
        endif()
    endif()
else()
    message(STATUS "Vulkan not found — rendering will return errors")
endif()

# ============================================================================
# Software rasterizer (header-only, no dependencies)
# ============================================================================

add_library(omnicpp_render INTERFACE)
target_include_directories(omnicpp_render
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
