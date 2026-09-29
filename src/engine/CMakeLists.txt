cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Canonical deterministic runtime
# S3: the render layer (22 TUs) lives in modules/render as Warploom::render;
# the runtime is the integration layer: asset + glue, plus the include root
# for the engine/* forwarder paths.
# ============================================================================

add_library(omnicpp_runtime SHARED
    asset/gltf_importer.cpp
    asset/gltf_animation.cpp
    asset/image_decode.cpp
    asset/ktx2_decoder.cpp
    asset/png_decoder.cpp
    asset/jpeg_decoder.cpp
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
# S4: the widget/view layer (node editor, inspector, graph-anim bridge)
# lives in modules/editor as Warploom::editor; PUBLIC for the same reason
# (viewport/tests include engine/editor/* forwarders and need the
# transitive include dirs + DSO).
# S3: rendering lives in modules/render as Warploom::render; PUBLIC for
# the same reason (glue/viewport/test headers include engine/render/*
# forwarders and need the transitive include dirs + DSO; the render DSO
# carries the Vulkan plumbing, incl. the PUBLIC OMNICPP_HAS_VULKAN
# definition that gates declarations in render public headers).
target_link_libraries(omnicpp_runtime
    PUBLIC Warploom::ui Warploom::core Warploom::editor Warploom::render)

# ============================================================================
# Module-alias libraries (publishing units)
#
# omnicpp_runtime is ONE shared object (one ABI, one dlopen surface), but
# downstream projects consume it through these alias targets. S3: the
# runtime is the integration layer only (asset + glue); render, editor,
# core and ui are real modules linked PUBLIC.
# ============================================================================

# (CMake forbids alias-of-alias, so these point at the real module targets.)
add_library(omnicpp_core ALIAS warploom_core)
add_library(omnicpp_ui ALIAS warploom_ui)
add_library(omnicpp_editor ALIAS warploom_editor)
add_library(omnicpp_render_lib ALIAS warploom_render)

# ============================================================================
# Software rasterizer (header-only, no dependencies)
# Kept as a target name for compatibility; the header now lives in
# modules/render's include root and reaches consumers through
# Warploom::render usage requirements (or the superproject include root).
# ============================================================================

add_library(omnicpp_render INTERFACE)
target_link_libraries(omnicpp_render INTERFACE Warploom::render)
