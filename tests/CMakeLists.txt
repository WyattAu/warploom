cmake_minimum_required(VERSION 3.28...4.0)
project(OmniCppTests VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(WARPLOOM_USE_VULKAN AND Vulkan_FOUND)
    # Prefer glslc (shaderc); fall back to glslangValidator where glslc is not
    # packaged (e.g. CI runners with only glslang-tools installed).
    find_program(WARPLOOM_GLSLC glslc)
    find_program(WARPLOOM_GLSLANG_VALIDATOR glslangValidator)
    if(WARPLOOM_GLSLC)
        set(WARPLOOM_SHADER_COMPILER "${WARPLOOM_GLSLC}")
        set(WARPLOOM_SHADER_COMPILER_ARGS "")
    elseif(WARPLOOM_GLSLANG_VALIDATOR)
        set(WARPLOOM_SHADER_COMPILER "${WARPLOOM_GLSLANG_VALIDATOR}")
        set(WARPLOOM_SHADER_COMPILER_ARGS "-V")
    endif()
    if(WARPLOOM_SHADER_COMPILER)
        set(WARPLOOM_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
        file(MAKE_DIRECTORY ${WARPLOOM_SHADER_DIR})
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/triangle.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.vert
                    -o ${WARPLOOM_SHADER_DIR}/triangle.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/triangle.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.frag
                    -o ${WARPLOOM_SHADER_DIR}/triangle.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/triangle.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ubo_triangle.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ubo_triangle.frag
                    -o ${WARPLOOM_SHADER_DIR}/ubo_triangle.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ubo_triangle.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/fill_gradient.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/fill_gradient.comp
                    -o ${WARPLOOM_SHADER_DIR}/fill_gradient.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/fill_gradient.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/gradient_triangle.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/gradient_triangle.vert
                    -o ${WARPLOOM_SHADER_DIR}/gradient_triangle.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/gradient_triangle.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/gradient_triangle.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/gradient_triangle.frag
                    -o ${WARPLOOM_SHADER_DIR}/gradient_triangle.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/gradient_triangle.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/gen_triangle.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/gen_triangle.comp
                    -o ${WARPLOOM_SHADER_DIR}/gen_triangle.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/gen_triangle.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/vertexpull_triangle.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/vertexpull_triangle.vert
                    -o ${WARPLOOM_SHADER_DIR}/vertexpull_triangle.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/vertexpull_triangle.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/vertexpull_triangle.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/vertexpull_triangle.frag
                    -o ${WARPLOOM_SHADER_DIR}/vertexpull_triangle.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/vertexpull_triangle.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/cull_and_draw.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/cull_and_draw.comp
                    -o ${WARPLOOM_SHADER_DIR}/cull_and_draw.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/cull_and_draw.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/gpu_objects.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/gpu_objects.vert
                    -o ${WARPLOOM_SHADER_DIR}/gpu_objects.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/gpu_objects.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/gpu_objects.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/gpu_objects.frag
                    -o ${WARPLOOM_SHADER_DIR}/gpu_objects.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/gpu_objects.frag
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/cube.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/cube.vert
                    -o ${WARPLOOM_SHADER_DIR}/cube.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/cube.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/cube.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/cube.frag
                    -o ${WARPLOOM_SHADER_DIR}/cube.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/cube.frag
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/cull_lod_occlude.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/cull_lod_occlude.comp
                    -o ${WARPLOOM_SHADER_DIR}/cull_lod_occlude.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/cull_lod_occlude.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/depth_reduce.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/depth_reduce.comp
                    -o ${WARPLOOM_SHADER_DIR}/depth_reduce.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/depth_reduce.comp
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/depth_reduce_mips.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/depth_reduce_mips.comp
                    -o ${WARPLOOM_SHADER_DIR}/depth_reduce_mips.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/depth_reduce_mips.comp
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/depth_reduce_image.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/depth_reduce_image.comp
                    -o ${WARPLOOM_SHADER_DIR}/depth_reduce_image.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/depth_reduce_image.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/cull_hiz_sampled.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/cull_hiz_sampled.comp
                    -o ${WARPLOOM_SHADER_DIR}/cull_hiz_sampled.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/cull_hiz_sampled.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/gen_cube_ring.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/gen_cube_ring.comp
                    -o ${WARPLOOM_SHADER_DIR}/gen_cube_ring.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/gen_cube_ring.comp
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/scene.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/scene.vert
                    -o ${WARPLOOM_SHADER_DIR}/scene.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/scene.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/scene.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/scene.frag
                    -o ${WARPLOOM_SHADER_DIR}/scene.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/scene.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/bindless_textures.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/bindless_textures.frag
                    -o ${WARPLOOM_SHADER_DIR}/bindless_textures.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/bindless_textures.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/bindless_palette.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/bindless_palette.frag
                    -o ${WARPLOOM_SHADER_DIR}/bindless_palette.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/bindless_palette.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/indexed_scene.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene.vert
                    -o ${WARPLOOM_SHADER_DIR}/indexed_scene.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/indexed_scene.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene.frag
                    -o ${WARPLOOM_SHADER_DIR}/indexed_scene.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/indexed_scene_material.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene_material.vert
                    -o ${WARPLOOM_SHADER_DIR}/indexed_scene_material.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene_material.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/indexed_scene_material.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene_material.frag
                    -o ${WARPLOOM_SHADER_DIR}/indexed_scene_material.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/indexed_scene_material.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_scene.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_scene.vert
                    -o ${WARPLOOM_SHADER_DIR}/pbr_scene.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_scene.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_gpu_driven.vert
                    -o ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_gpu_driven.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_scene.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_scene.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_scene.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_scene.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_gpu_driven.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_gpu_driven.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/cull_and_draw_lod.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/cull_and_draw_lod.comp
                    -o ${WARPLOOM_SHADER_DIR}/cull_and_draw_lod.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/cull_and_draw_lod.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rgen.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace.rgen
                    -o ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rgen.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace.rgen
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rmiss.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace.rmiss
                    -o ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rmiss.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace.rmiss
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rchit.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace.rchit
                    -o ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rchit.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace.rchit
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_pathtrace_leaf.rchit.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace_leaf.rchit
                    -o ${WARPLOOM_SHADER_DIR}/pt_pathtrace_leaf.rchit.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace_leaf.rchit
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_pathtrace_leaf.rmiss.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace_leaf.rmiss
                    -o ${WARPLOOM_SHADER_DIR}/pt_pathtrace_leaf.rmiss.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_pathtrace_leaf.rmiss
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_real.rgen.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_real.rgen
                    -o ${WARPLOOM_SHADER_DIR}/pt_real.rgen.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_real.rgen
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_real.rmiss.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_real.rmiss
                    -o ${WARPLOOM_SHADER_DIR}/pt_real.rmiss.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_real.rmiss
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pt_real.rchit.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pt_real.rchit
                    -o ${WARPLOOM_SHADER_DIR}/pt_real.rchit.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pt_real.rchit
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ray_query_first.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ray_query_first.comp
                    -o ${WARPLOOM_SHADER_DIR}/ray_query_first.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ray_query_first.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/rt_shadows_a_vs_b.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/rt_shadows_a_vs_b.comp
                    -o ${WARPLOOM_SHADER_DIR}/rt_shadows_a_vs_b.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/rt_shadows_a_vs_b.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_rt_shadow.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_shadow.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_rt_shadow.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_shadow.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_rt_reflect.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_reflect.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_rt_reflect.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_reflect.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/cull_and_draw_lod_occlude.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/cull_and_draw_lod_occlude.comp
                    -o ${WARPLOOM_SHADER_DIR}/cull_and_draw_lod_occlude.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/cull_and_draw_lod_occlude.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_ibl.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_ibl.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_ibl.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_ibl.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ibl_sky.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_sky.comp
                    -o ${WARPLOOM_SHADER_DIR}/ibl_sky.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_sky.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ibl_prefilter.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_prefilter.comp
                    -o ${WARPLOOM_SHADER_DIR}/ibl_prefilter.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_prefilter.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ibl_irradiance.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_irradiance.comp
                    -o ${WARPLOOM_SHADER_DIR}/ibl_irradiance.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_irradiance.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ibl_brdf_lut.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_brdf_lut.comp
                    -o ${WARPLOOM_SHADER_DIR}/ibl_brdf_lut.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ibl_brdf_lut.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/shadow.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/shadow.vert
                    -o ${WARPLOOM_SHADER_DIR}/shadow.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/shadow.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/shadow.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/shadow.frag
                    -o ${WARPLOOM_SHADER_DIR}/shadow.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/shadow.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_shadow.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_shadow.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_shadow.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_shadow.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_ibl_shadow.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_ibl_shadow.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_ibl_shadow.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_ibl_shadow.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_full.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_full.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_full.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_full.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_full_ml.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_full_ml.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_full_ml.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_full_ml.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_rt_full_ml.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_full_ml.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_rt_full_ml.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_full_ml.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_rt_full.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_full.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_rt_full.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_rt_full.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven_full.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_gpu_driven_full.frag
                    -o ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven_full.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/pbr_gpu_driven_full.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/shadow_skinned.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/shadow_skinned.vert
                    -o ${WARPLOOM_SHADER_DIR}/shadow_skinned.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/shadow_skinned.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/sky.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/sky.vert
                    -o ${WARPLOOM_SHADER_DIR}/sky.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/sky.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/sky.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/sky.frag
                    -o ${WARPLOOM_SHADER_DIR}/sky.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/sky.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/fullscreen.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/fullscreen.vert
                    -o ${WARPLOOM_SHADER_DIR}/fullscreen.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/fullscreen.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/tonemap_fxaa.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/tonemap_fxaa.frag
                    -o ${WARPLOOM_SHADER_DIR}/tonemap_fxaa.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/tonemap_fxaa.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/bloom_downsample.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/bloom_downsample.frag
                    -o ${WARPLOOM_SHADER_DIR}/bloom_downsample.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/bloom_downsample.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/bloom_upsample.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/bloom_upsample.frag
                    -o ${WARPLOOM_SHADER_DIR}/bloom_upsample.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/bloom_upsample.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/skinned_scene.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/skinned_scene.vert
                    -o ${WARPLOOM_SHADER_DIR}/skinned_scene.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/skinned_scene.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/lod_select.comp.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/lod_select.comp
                    -o ${WARPLOOM_SHADER_DIR}/lod_select.comp.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/lod_select.comp
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ui_quad.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ui_quad.vert
                    -o ${WARPLOOM_SHADER_DIR}/ui_quad.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ui_quad.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/ui_quad.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/ui_quad.frag
                    -o ${WARPLOOM_SHADER_DIR}/ui_quad.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/ui_quad.frag
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/textured_quad.vert.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/textured_quad.vert
                    -o ${WARPLOOM_SHADER_DIR}/textured_quad.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/textured_quad.vert
            VERBATIM
        )
        add_custom_command(
            OUTPUT ${WARPLOOM_SHADER_DIR}/textured_quad.frag.spv
            COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                    ${CMAKE_SOURCE_DIR}/assets/shaders/textured_quad.frag
                    -o ${WARPLOOM_SHADER_DIR}/textured_quad.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/assets/shaders/textured_quad.frag
            VERBATIM
        )
        add_custom_target(omnicpp_test_shaders
            DEPENDS
                ${WARPLOOM_SHADER_DIR}/triangle.vert.spv
                ${WARPLOOM_SHADER_DIR}/triangle.frag.spv
                ${WARPLOOM_SHADER_DIR}/scene.vert.spv
                ${WARPLOOM_SHADER_DIR}/scene.frag.spv
                ${WARPLOOM_SHADER_DIR}/ubo_triangle.frag.spv
                ${WARPLOOM_SHADER_DIR}/bindless_palette.frag.spv
                ${WARPLOOM_SHADER_DIR}/bindless_textures.frag.spv
                ${WARPLOOM_SHADER_DIR}/fill_gradient.comp.spv
                ${WARPLOOM_SHADER_DIR}/gradient_triangle.vert.spv
                ${WARPLOOM_SHADER_DIR}/gradient_triangle.frag.spv
                ${WARPLOOM_SHADER_DIR}/gen_triangle.comp.spv
                ${WARPLOOM_SHADER_DIR}/vertexpull_triangle.vert.spv
                ${WARPLOOM_SHADER_DIR}/vertexpull_triangle.frag.spv
                ${WARPLOOM_SHADER_DIR}/cull_and_draw.comp.spv
                ${WARPLOOM_SHADER_DIR}/gpu_objects.vert.spv
                ${WARPLOOM_SHADER_DIR}/gpu_objects.frag.spv
                ${WARPLOOM_SHADER_DIR}/cube.vert.spv
                ${WARPLOOM_SHADER_DIR}/cube.frag.spv
                ${WARPLOOM_SHADER_DIR}/cull_lod_occlude.comp.spv
                ${WARPLOOM_SHADER_DIR}/depth_reduce.comp.spv
                ${WARPLOOM_SHADER_DIR}/depth_reduce_mips.comp.spv
                ${WARPLOOM_SHADER_DIR}/depth_reduce_image.comp.spv
                ${WARPLOOM_SHADER_DIR}/cull_hiz_sampled.comp.spv
                ${WARPLOOM_SHADER_DIR}/gen_cube_ring.comp.spv
                ${WARPLOOM_SHADER_DIR}/indexed_scene.vert.spv
                ${WARPLOOM_SHADER_DIR}/indexed_scene.frag.spv
                ${WARPLOOM_SHADER_DIR}/indexed_scene_material.vert.spv
                ${WARPLOOM_SHADER_DIR}/indexed_scene_material.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_scene.vert.spv
                ${WARPLOOM_SHADER_DIR}/pbr_scene.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven.vert.spv
                ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven.frag.spv
                ${WARPLOOM_SHADER_DIR}/cull_and_draw_lod.comp.spv
                ${WARPLOOM_SHADER_DIR}/cull_and_draw_lod_occlude.comp.spv
                ${WARPLOOM_SHADER_DIR}/pbr_ibl.frag.spv
                ${WARPLOOM_SHADER_DIR}/ibl_sky.comp.spv
                ${WARPLOOM_SHADER_DIR}/ibl_prefilter.comp.spv
                ${WARPLOOM_SHADER_DIR}/ibl_irradiance.comp.spv
                ${WARPLOOM_SHADER_DIR}/ibl_brdf_lut.comp.spv
                ${WARPLOOM_SHADER_DIR}/shadow.vert.spv
                ${WARPLOOM_SHADER_DIR}/shadow.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_shadow.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_ibl_shadow.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_full.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_full_ml.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_rt_full.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_rt_full_ml.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_gpu_driven_full.frag.spv
                ${WARPLOOM_SHADER_DIR}/shadow_skinned.vert.spv
                ${WARPLOOM_SHADER_DIR}/fullscreen.vert.spv
                ${WARPLOOM_SHADER_DIR}/tonemap_fxaa.frag.spv
                ${WARPLOOM_SHADER_DIR}/bloom_downsample.frag.spv
                ${WARPLOOM_SHADER_DIR}/bloom_upsample.frag.spv
                ${WARPLOOM_SHADER_DIR}/skinned_scene.vert.spv
                ${WARPLOOM_SHADER_DIR}/lod_select.comp.spv
                ${WARPLOOM_SHADER_DIR}/ray_query_first.comp.spv
                ${WARPLOOM_SHADER_DIR}/rt_shadows_a_vs_b.comp.spv
                ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rgen.spv
                ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rmiss.spv
                ${WARPLOOM_SHADER_DIR}/pt_pathtrace.rchit.spv
                ${WARPLOOM_SHADER_DIR}/pt_pathtrace_leaf.rchit.spv
                ${WARPLOOM_SHADER_DIR}/pt_pathtrace_leaf.rmiss.spv
                ${WARPLOOM_SHADER_DIR}/pt_real.rgen.spv
                ${WARPLOOM_SHADER_DIR}/pt_real.rmiss.spv
                ${WARPLOOM_SHADER_DIR}/pt_real.rchit.spv
                ${WARPLOOM_SHADER_DIR}/pbr_rt_shadow.frag.spv
                ${WARPLOOM_SHADER_DIR}/pbr_rt_reflect.frag.spv
                ${WARPLOOM_SHADER_DIR}/sky.vert.spv
                ${WARPLOOM_SHADER_DIR}/sky.frag.spv
                ${WARPLOOM_SHADER_DIR}/ui_quad.vert.spv
                ${WARPLOOM_SHADER_DIR}/ui_quad.frag.spv
                ${WARPLOOM_SHADER_DIR}/textured_quad.vert.spv
                ${WARPLOOM_SHADER_DIR}/textured_quad.frag.spv
        )
    endif()
endif()

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
target_include_directories(warploom_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
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
target_include_directories(warploom_contract_violator PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_dependencies(warploom_unit_tests warploom_contract_violator)
if(TARGET omnicpp_test_shaders)
    add_dependencies(warploom_unit_tests omnicpp_test_shaders)
    target_compile_definitions(warploom_unit_tests PRIVATE
        WARPLOOM_TEST_SHADER_DIR=\"${WARPLOOM_SHADER_DIR}\")
endif()

add_executable(warploom_runtime_only_tests
    unit/test_runtime_only.cpp
)
set_target_properties(warploom_runtime_only_tests PROPERTIES OUTPUT_NAME warploom_runtime_only_tests)  # S5-B phase 2 binary rename
target_link_libraries(warploom_runtime_only_tests PRIVATE Warploom::engine Threads::Threads gtest gtest_main)
target_include_directories(warploom_runtime_only_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(warploom_deterministic_runtime_benchmark
    performance/deterministic_runtime_benchmark.cpp
)
set_target_properties(warploom_deterministic_runtime_benchmark PROPERTIES OUTPUT_NAME warploom_deterministic_runtime_benchmark)  # S5-B phase 2 binary rename
target_link_libraries(warploom_deterministic_runtime_benchmark PRIVATE Warploom::engine Threads::Threads)
target_include_directories(warploom_deterministic_runtime_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Legacy tests — only built when WARPLOOM_LEGACY_ENGINE=ON.
# ============================================================================

if(WARPLOOM_LEGACY_ENGINE)
    add_executable(omnicpp_legacy_tests
        unit/test_input_manager.cpp
    )
    target_link_libraries(omnicpp_legacy_tests PRIVATE omnicpp_engine Threads::Threads gtest gtest_main)
    target_include_directories(omnicpp_legacy_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

    add_executable(omnicpp_integration_tests
        test_engine.cpp
    )
    target_link_libraries(omnicpp_integration_tests PRIVATE omnicpp_engine Threads::Threads)
    target_include_directories(omnicpp_integration_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

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

if(WARPLOOM_LEGACY_ENGINE)
    add_test(NAME OmniCppLegacyTests COMMAND omnicpp_legacy_tests)
    add_test(NAME OmniCppEngineIntegration COMMAND omnicpp_integration_tests --test-engine)
    add_test(NAME OmniCppGameIntegration COMMAND omnicpp_integration_tests --test-game)
endif()
