cmake_minimum_required(VERSION 3.28...4.0)
project(OmniCppTests VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)


# ============================================================================
# Canonical runtime tests — these link ONLY to the Warploom::engine
# aggregate (the five modules transitively; omnicpp_runtime was deleted
# in S5-A). They must never depend on omnicpp_engine.
# ============================================================================

# GPU-dependent runtime tests (raw Vulkan symbols in the TU): only added
# when Vulkan headers are available. Headless CI legs build the rest.
set(WARPLOOM_GPU_TEST_SOURCES
    unit/test_ray_query_first_contact.cpp
    unit/test_rt_shadows.cpp
    unit/test_rt_reflections.cpp
    unit/test_path_tracing.cpp
    unit/test_path_tracing_real.cpp
    unit/test_ui_gpu.cpp
)

add_executable(warploom_unit_tests
    unit/test_contract.cpp
    unit/test_deterministic_runtime.cpp
    unit/test_animation_state_machine.cpp
    unit/test_physics_world.cpp
    unit/test_physics_ecs_bridge.cpp
    unit/test_control_server.cpp
    unit/test_diagnostics.cpp
    unit/test_document_projection.cpp
    unit/test_session_tick.cpp
    unit/test_scene_document.cpp
    unit/test_ui_paint_golden.cpp
    unit/test_ui_gpu.cpp
    unit/test_editor_session.cpp
    unit/test_replay_scrubber.cpp
    unit/test_command_recorder.cpp
    unit/test_node_graph.cpp
    unit/test_node_persistence.cpp
    unit/test_script_module.cpp
    unit/test_input_state.cpp
    unit/test_input_translators.cpp
    unit/test_engine.cpp
    unit/test_concurrency.cpp
    unit/test_platform_primitives.cpp
    unit/test_ecs_foundation.cpp
    unit/test_rendering.cpp
    unit/test_render_graph_async.cpp
    unit/test_gpu_lod_occlusion.cpp
    unit/test_scene3d.cpp
    unit/test_depth_pyramid.cpp
    unit/test_depth_pyramid_mips.cpp
    unit/test_depth_convention.cpp
    unit/test_hiz_frame_state.cpp
    unit/test_renderer_hiz_contract.cpp
    unit/test_hiz_sampled_pingpong.cpp
    unit/test_dual_queue_graph.cpp
    unit/test_gpu_driven.cpp
    unit/test_gpu_driven_ab.cpp
    unit/test_gpu_driven_cull.cpp
    unit/test_gpu_driven_frame.cpp
    unit/test_gpu_driven_occlusion_frame.cpp
    unit/test_mesh_table.cpp
    unit/test_scene_submission.cpp
    unit/test_scene_extraction.cpp
    unit/test_scene_camera.cpp
    unit/test_scene_resources.cpp
    unit/test_gltf_importer.cpp
    unit/test_gltf_animation.cpp
    unit/test_gltf_scene.cpp
    unit/test_pbr_scene.cpp
    unit/test_pbr_ibl.cpp
    unit/test_shadow_mapping.cpp
    unit/test_sky.cpp
    unit/test_sky_integration.cpp
    unit/test_lod_integration.cpp
    unit/test_postprocessing.cpp
    unit/test_gpu_skinning.cpp
    unit/test_pbr_frame_variants.cpp
    unit/test_gpu_mannequin.cpp
    unit/test_mesh_lod.cpp
    unit/test_png_decoder.cpp
    unit/test_ktx2_decoder.cpp
    unit/test_jpeg_decoder.cpp
    unit/test_png_texture.cpp
    unit/test_reflector_and_allocator_robustness.cpp
    unit/test_latency_telemetry.cpp
    unit/test_job_system.cpp
)
set_target_properties(warploom_unit_tests PROPERTIES OUTPUT_NAME warploom_unit_tests)  # S5-B phase 2 binary rename
if(WARPLOOM_USE_VULKAN AND Vulkan_FOUND)
    target_sources(warploom_unit_tests PRIVATE ${WARPLOOM_GPU_TEST_SOURCES})
endif()
target_link_libraries(warploom_unit_tests PRIVATE Warploom::engine Threads::Threads gtest gtest_main)
# Asset directory for file-based tests (mannequin model); shader-dir define
# only applies when the test shader targets exist (GPU presets).
target_compile_definitions(warploom_unit_tests PRIVATE
    WARPLOOM_TEST_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/assets/models\"
    WARPLOOM_TEST_BIN_DIR=\"${CMAKE_BINARY_DIR}/bin\")

# Native script-module fixtures: real shared objects exercising the dlopen
# path of the script-module host (ok / missing-symbol / ABI-mismatch).
add_library(test_module_ok SHARED module_fixture/test_module_ok.cpp)
add_library(test_module_missing SHARED module_fixture/test_module_missing.cpp)
add_library(test_module_abi SHARED module_fixture/test_module_abi.cpp)
set_target_properties(test_module_ok test_module_missing test_module_abi PROPERTIES
    CXX_STANDARD 23
    POSITION_INDEPENDENT_CODE ON)
add_dependencies(warploom_unit_tests test_module_ok test_module_missing test_module_abi)
target_compile_definitions(warploom_unit_tests PRIVATE
    WARPLOOM_TEST_MODULE_OK="$<TARGET_FILE:test_module_ok>"
    WARPLOOM_TEST_MODULE_MISSING="$<TARGET_FILE:test_module_missing>"    WARPLOOM_TEST_MODULE_ABI="$<TARGET_FILE:test_module_abi>")

# Rust example module (examples/rust_module): built when cargo exists so the
# cross-language dlopen test runs for real; the test self-skips otherwise.
find_program(WARPLOOM_CARGO_EXECUTABLE cargo)
if(WARPLOOM_CARGO_EXECUTABLE)
    add_custom_command(
        OUTPUT ${CMAKE_SOURCE_DIR}/examples/rust_module/target/release/libomnicpp_example_module.so
        COMMAND ${WARPLOOM_CARGO_EXECUTABLE} build --release
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/examples/rust_module
        VERBATIM)
    add_custom_target(omnicpp_rust_module
        DEPENDS ${CMAKE_SOURCE_DIR}/examples/rust_module/target/release/libomnicpp_example_module.so)
    add_dependencies(warploom_unit_tests omnicpp_rust_module)
    target_compile_definitions(warploom_unit_tests PRIVATE
        WARPLOOM_TEST_RUST_MODULE="${CMAKE_SOURCE_DIR}/examples/rust_module/target/release/libomnicpp_example_module.so")
endif()

# Contract-violator child binary: the violation test asserts that
# WARPLOOM_CONTRACT aborts with a diagnostic, in a process that may die.
add_executable(warploom_contract_violator unit/contract_violator.cpp)
set_target_properties(warploom_contract_violator PROPERTIES OUTPUT_NAME warploom_contract_violator)  # S5-B phase 2 binary rename
target_link_libraries(warploom_contract_violator PRIVATE Warploom::engine)
add_dependencies(warploom_unit_tests warploom_contract_violator)
if(TARGET warploom_shaders)
    add_dependencies(warploom_unit_tests warploom_shaders)
    target_compile_definitions(warploom_unit_tests PRIVATE
        WARPLOOM_TEST_SHADER_DIR=\"${WARPLOOM_SHADER_OUTPUT_DIR}\")
endif()

add_executable(warploom_runtime_only_tests
    unit/test_runtime_only.cpp
)
set_target_properties(warploom_runtime_only_tests PROPERTIES OUTPUT_NAME warploom_runtime_only_tests)  # S5-B phase 2 binary rename
target_link_libraries(warploom_runtime_only_tests PRIVATE Warploom::engine Threads::Threads gtest gtest_main)

add_executable(warploom_deterministic_runtime_benchmark
    performance/deterministic_runtime_benchmark.cpp
)
set_target_properties(warploom_deterministic_runtime_benchmark PROPERTIES OUTPUT_NAME warploom_deterministic_runtime_benchmark)  # S5-B phase 2 binary rename
target_link_libraries(warploom_deterministic_runtime_benchmark PRIVATE Warploom::engine Threads::Threads)

# ============================================================================
# Test registration
# ============================================================================

enable_testing()
add_test(NAME WarploomUnitTests COMMAND warploom_unit_tests)
add_test(NAME WarploomRuntimeOnly COMMAND warploom_runtime_only_tests)
add_test(NAME WarploomRuntimeBenchmark COMMAND warploom_deterministic_runtime_benchmark)

if(WARPLOOM_VULKAN_VALIDATION_TESTS AND WARPLOOM_USE_VULKAN AND Vulkan_FOUND)
    set_tests_properties(WarploomUnitTests WarploomRuntimeOnly WarploomRuntimeBenchmark
        PROPERTIES ENVIRONMENT "VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation")
endif()
