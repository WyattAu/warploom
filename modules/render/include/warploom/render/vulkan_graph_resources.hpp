#pragma once

/**
 * @file vulkan_graph_resources.hpp
 * @brief Derive frame-graph resource LIFETIMES from a declared node sequence
 *        (R6, Loop 13).
 *
 * The pure planner (`vulkan_frame_allocator.hpp`) answers "what may alias"
 * given declared lifetimes, but nothing derived those lifetimes from the
 * thing the renderer actually records: the `GraphNode` sequence. Lifetimes
 * were hand-declared per resource against a fixed pass list, so "live on
 * passes [i, j]" described nothing real -- the missing prerequisite the
 * comparison matrix names as gating R6b.
 *
 * This header closes exactly that gap and nothing more: walk the nodes in
 * order, record every image's first/last touching pass, and emit
 * `FrameResource`s in first-touch order. Three things the graph CANNOT
 * supply travel in as caller callbacks (the RPS resource taxonomy:
 * external/persistent resources are caller knowledge, not graph facts):
 *
 * - byte size (extents x format; the graph holds handles, not descriptions),
 * - cross-frame history (pyramid, PT accumulation, swapchain: read by a
 *   LATER frame, so RPS-persistent and never aliasable),
 * - the alias-compatibility tag (format/layout identity).
 *
 * Pure, deterministic, no device: first-touch order fixes names, the walk
 * order fixes intervals, and the callbacks are the caller's explicit,
 * auditable policy. The Vulkan side (create/alias/destroy from the plan)
 * remains R6b and is still unwired -- this only makes the plan's INPUT
 * derived instead of hand-written.
 */

#include <cstdint>
#include <vector>

#include "warploom/render/vulkan_frame_allocator.hpp"
#include "warploom/render/vulkan_render_graph.hpp"

namespace warploom::render {

//! Caller knowledge the node sequence cannot supply. Plain function
//! pointers (matching execute_graph's callback style): no allocation, no
//! capture, no state to invalidate determinism.
using GraphImageSizeFn = std::uint64_t (*)(VkImage) noexcept;
using GraphImageHistoryFn = bool (*)(VkImage) noexcept;
using GraphImageTagFn = std::uint64_t (*)(VkImage) noexcept;

//! Derive one FrameResource per distinct image in the node sequence.
//!
//! Touched images per node, in fixed sub-order: render attachments, then
//! sampled images, then compute image uses. First touch opens
//! [pass, pass]; every later touch extends last_use. VK_NULL_HANDLE uses
//! are skipped (undeclared slots, not resources).
//!
//! `timeline` for the planner is nodes.size(): history spans [0, N], which
//! overlaps every in-frame interval by construction.
[[nodiscard]] std::vector<FrameResource> derive_frame_resources(
    const std::vector<GraphNode>& nodes, GraphImageSizeFn size_of,
    GraphImageHistoryFn is_history, GraphImageTagFn tag_of);

}  // namespace warploom::render
