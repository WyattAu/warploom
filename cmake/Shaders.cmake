# ============================================================================
# Warploom - Shader Compilation
# ============================================================================
# Shaders are a BUILD-TIME product of the engine, not a test fixture. They
# were previously compiled only inside tests/CMakeLists.txt, which meant the
# viewport had no dependency on them and no path was ever handed to it —
# a fresh clone could build the app and then fail at runtime on a missing
# .spv (the 0.1 "clone-and-build" gate).
#
# Rules now:
#   * root scope, so every consumer (tests AND the viewport) can depend on
#     the one target;
#   * driven by a CONFIGURE_DEPENDS glob, so a newly added shader is
#     compiled automatically. The previous hand-written rule-per-shader
#     layout silently skipped any file someone forgot to enumerate;
#   * part of the default build (ALL), not opt-in;
#   * the output directory is a cache variable, so install rules and
#     consumers read one source of truth.
# ============================================================================

if(NOT WARPLOOM_USE_VULKAN OR NOT Vulkan_FOUND)
    return()
endif()

# Prefer glslc (shaderc); fall back to glslangValidator where glslc is not
# packaged (e.g. CI runners with only glslang-tools installed).
#
# The two compilers spell --target-env differently: glslc requires the "="
# form and REJECTS the space form, glslangValidator requires the space form
# and rejects "=". The previous hand-written shader rules hardcoded the "="
# form inside the generic COMMAND, so every ray-tracing stage (and
# cull_and_draw_lod.comp) failed to compile whenever the glslang fallback
# was in use -- which is exactly the CI leg that installs only
# glslang-tools. The flag is therefore chosen per compiler here.
find_program(WARPLOOM_GLSLC glslc)
find_program(WARPLOOM_GLSLANG_VALIDATOR glslangValidator)

if(WARPLOOM_GLSLC)
    set(WARPLOOM_SHADER_COMPILER "${WARPLOOM_GLSLC}")
    set(WARPLOOM_SHADER_COMPILER_ARGS --target-env=vulkan1.2)
elseif(WARPLOOM_GLSLANG_VALIDATOR)
    set(WARPLOOM_SHADER_COMPILER "${WARPLOOM_GLSLANG_VALIDATOR}")
    set(WARPLOOM_SHADER_COMPILER_ARGS -V --target-env vulkan1.2)
endif()

if(NOT WARPLOOM_SHADER_COMPILER)
    message(WARNING
        "Warploom: neither glslc nor glslangValidator found — shaders will NOT "
        "be compiled. The viewport and the GPU tests will fail at runtime on "
        "missing .spv files. Install glslc (shaderc) or glslang-tools.")
    return()
endif()

set(WARPLOOM_SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/assets/shaders")
set(WARPLOOM_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders"
    CACHE PATH "Where Warploom compiles its shaders to .spv")

file(MAKE_DIRECTORY "${WARPLOOM_SHADER_OUTPUT_DIR}")

# Stage extensions: vertex/fragment/compute plus the ray-tracing stages.
set(_warploom_shader_stages
    vert frag comp
    rgen rmiss rchit
    rint rahit rcall
)

set(_warploom_shader_globs "")
foreach(_stage IN LISTS _warploom_shader_stages)
    list(APPEND _warploom_shader_globs "${WARPLOOM_SHADER_SOURCE_DIR}/*.${_stage}")
endforeach()

file(GLOB _warploom_shader_sources CONFIGURE_DEPENDS ${_warploom_shader_globs})
list(SORT _warploom_shader_sources)

set(_warploom_shader_outputs "")
foreach(_src IN LISTS _warploom_shader_sources)
    get_filename_component(_name "${_src}" NAME)
    set(_out "${WARPLOOM_SHADER_OUTPUT_DIR}/${_name}.spv")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND ${WARPLOOM_SHADER_COMPILER} ${WARPLOOM_SHADER_COMPILER_ARGS}
                "${_src}" -o "${_out}"
        DEPENDS "${_src}"
        COMMENT "Compiling shader ${_name}"
        VERBATIM
    )
    list(APPEND _warploom_shader_outputs "${_out}")
endforeach()

list(LENGTH _warploom_shader_outputs _warploom_shader_count)
if(_warploom_shader_count EQUAL 0)
    message(WARNING "Warploom: no shader sources found under ${WARPLOOM_SHADER_SOURCE_DIR}")
endif()

# ALL: shaders are part of a normal build. Consumers add
# add_dependencies(<target> warploom_shaders); the directory reaches them
# through the WARPLOOM_SHADER_OUTPUT_DIR cache variable or a compile
# definition, not through a hand-exported environment variable.
add_custom_target(warploom_shaders ALL DEPENDS ${_warploom_shader_outputs})

message(STATUS "  Shaders: ${_warploom_shader_count} sources -> ${WARPLOOM_SHADER_OUTPUT_DIR}")