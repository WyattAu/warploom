cmake_minimum_required(VERSION 3.20)

# ============================================================================
# S5-A: the monolith is gone. The five real modules (core, ui, editor,
# render, asset) plus the Warploom::engine aggregate (modules/engine)
# replace omnicpp_runtime entirely — the directory exists only to keep
# the compatibility alias names and the software-rasterizer shim
# configuring for in-tree consumers.
# ============================================================================

# (CMake forbids alias-of-alias, so these point at the real module targets.
# The old omnicpp_* spellings stay until the S5-B identifier migration.)
add_library(omnicpp_core ALIAS warploom_core)
add_library(omnicpp_ui ALIAS warploom_ui)
add_library(omnicpp_editor ALIAS warploom_editor)
add_library(omnicpp_render_lib ALIAS warploom_render)
add_library(omnicpp_asset ALIAS warploom_asset)

# ============================================================================
# Software rasterizer (header-only, no dependencies)
# Kept as a target name for compatibility; the header lives in
# modules/render's include root and reaches consumers through
# Warploom::render usage requirements (or the superproject include root).
# ============================================================================

add_library(omnicpp_render INTERFACE)
target_link_libraries(omnicpp_render INTERFACE Warploom::render)
