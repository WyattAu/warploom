cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Canonical deterministic runtime
# This is the core engine target with NO graphics dependencies.
# ============================================================================

add_library(omnicpp_runtime SHARED
    core/engine.cpp
    core/input_state.cpp
    core/control_server.cpp
    core/document.cpp
    core/property_registry.cpp
    core/editor_session.cpp
    core/node_graph.cpp
    core/script_module.cpp
    editor/node_editor.cpp
    editor/inspector.cpp
    core/input_translators.cpp
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
target_link_libraries(omnicpp_runtime PUBLIC Warploom::ui)

# ============================================================================
# Module-alias libraries (publishing units)
#
# omnicpp_runtime is ONE shared object (one ABI, one dlopen surface), but
# downstream projects consume it through these alias targets so a future
# physical split (omnicpp_core / omnicpp_ui / omnicpp_render shared
# libraries) changes no consumer CMake. Dependency direction is enforced
# by convention today (core has no ui/render includes) and becomes
# link-time enforcement when the split happens.
# ============================================================================

add_library(omnicpp_core ALIAS omnicpp_runtime)
add_library(omnicpp_ui ALIAS omnicpp_runtime)
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
