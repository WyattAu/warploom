cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Canonical deterministic runtime
# S3: the render layer lives in modules/render as Warploom::render.
# S3.5: the asset layer lives in modules/asset as Warploom::asset.
# The runtime is now a zero-source integration shell: it exists so every
# existing consumer target keeps linking unchanged, and pulls the five
# modules PUBLIC. S5-A deletes this target when the Warploom::engine
# aggregate lands (docs/warploom-asset-plan.md §2.4).
# ============================================================================

add_library(omnicpp_runtime INTERFACE)
target_include_directories(omnicpp_runtime
    INTERFACE
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
# S3.5: asset import lives in modules/asset as Warploom::asset; PUBLIC
# for the same reason (viewport/test headers include engine/asset/*
# forwarders).
target_link_libraries(omnicpp_runtime
    INTERFACE Warploom::ui Warploom::core Warploom::editor Warploom::render
              Warploom::asset)

# ============================================================================
# Module-alias libraries (publishing units)
#
# omnicpp_runtime is ONE shared object surface, but the real code lives in
# the five module DSOs; downstream projects consume them through these
# alias targets. S5-A replaces this shell with the Warploom::engine
# aggregate.
# ============================================================================

# (CMake forbids alias-of-alias, so these point at the real module targets.)
add_library(omnicpp_core ALIAS warploom_core)
add_library(omnicpp_ui ALIAS warploom_ui)
add_library(omnicpp_editor ALIAS warploom_editor)
add_library(omnicpp_render_lib ALIAS warploom_render)
add_library(omnicpp_asset ALIAS warploom_asset)

# ============================================================================
# Software rasterizer (header-only, no dependencies)
# Kept as a target name for compatibility; the header now lives in
# modules/render's include root and reaches consumers through
# Warploom::render usage requirements (or the superproject include root).
# ============================================================================

add_library(omnicpp_render INTERFACE)
target_link_libraries(omnicpp_render INTERFACE Warploom::render)
