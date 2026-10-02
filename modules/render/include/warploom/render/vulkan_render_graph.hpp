#pragma once

/**
 * @file vulkan_render_graph.hpp
 * @brief Declarative pass graph with automatic image barriers.
 *
 * Passes declare color/depth attachments with explicit usage timelines.
 * `compile()` computes, for every image, the layout/access/stage transition
 * between consecutive passes and emits `VkImageMemoryBarrier`s. `execute()`
 * inserts the barriers and records each pass via a caller callback, all
 * inside one command buffer.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>
#include <vector>

namespace warploom::render {

//! Mirrors VK_QUEUE_FAMILY_IGNORED (0xFFFFFFFF) so the Vulkan-off shim build
//! compiles the same headers; identical value when Vulkan is enabled.
inline constexpr std::uint32_t kIgnoredQueueFamily = 0xFFFFFFFFU;

//! Declares how one pass uses one image attachment.
struct RenderPassAttachment {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
  //! Layout the image must be in when the pass starts (after barriers).
  VkImageLayout used_layout{VK_IMAGE_LAYOUT_UNDEFINED};   // set per role below
  //! Layout the pass leaves the image in when it ends.
  VkImageLayout final_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  //! Access mask the pass performs on the image.
  std::uint32_t access{0};   // VkAccessFlags value (0 when Vulkan-off)
  //! Pipeline stage where the access happens.
  std::uint32_t stage{0};    // VkPipelineStageFlags value (0 when Vulkan-off)
  bool is_depth{false};
};

//! Builder helpers so callers do not touch raw masks/levels by hand.
[[nodiscard]] RenderPassAttachment color_attachment(
    VkImage image, VkImageView view, VkFormat format,
    VkImageLayout final_layout);
[[nodiscard]] RenderPassAttachment depth_attachment(
    VkImage image, VkImageView view, VkFormat format,
    VkImageLayout final_layout);

//! Declares how one pass SAMPLES an image (shader read, not an attachment).
//! The compiler tracks sampled state between passes exactly like attachments:
//! a shadow pre-pass writes depth and the next pass samples it, so the
//! executor materializes the DEPTH_ATTACHMENT -> DEPTH_READ layout transition
//! and the write->read execution barrier without manual authoring.
struct GraphSampledImage {
  VkImage image{VK_NULL_HANDLE};
  VkImageLayout used_layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  std::uint32_t aspect{0};  //!< VkImageAspectFlags; 0 means infer from layout.
};

//! One render pass: a name, its attachments, and a record callback handle.
struct GraphBufferEdge {
  VkBuffer buffer{VK_NULL_HANDLE};
  std::uint32_t producer_stage{0};   //!< Stage that wrote the buffer.
  std::uint32_t consumer_stage{0};   //!< Stage that reads/writes it next.
  std::uint32_t consumer_access{0};  //!< Consumer's access mask.
  //! Queue-family indices for ownership transfer (set by the caller when the
  //! producer is recorded on another queue); VK_QUEUE_FAMILY_IGNORED otherwise.
  std::uint32_t producer_family{kIgnoredQueueFamily};
  std::uint32_t consumer_family{kIgnoredQueueFamily};
};

//! Declares one image subresource use by a compute pass. Unlike a render
//! attachment, this can name an individual H-Z mip and its explicit layout.
  //! The renderer records exactly one depth-sourced H-Z pass per frame when H-Z is
  //! enabled; any future scene integration that wants post-depth culling must reuse the
  //! renderer-owned reduction pass, the single callback slot, or a separate graph
  //! submission path rather than recording a second depth-sourced H-Z pass here.
  //!
  //! Design note: when the renderer owns the reduction pass, the callback slot, and
  //! the frame token, duplicate depth-sourced H-Z work in the same frame is an ABI
  //! misuse, not a feature. That constraint is enforced at record time, not postponed
  //! to submission, so a broken scene integration fails fast instead of leaking a stale
  //! previous-frame pyramid.
struct GraphImageUse {
  VkImage image{VK_NULL_HANDLE};
  std::uint32_t base_mip{0};
  std::uint32_t level_count{1};
  std::uint32_t aspect{0};   //!< VkImageAspectFlags; 0 means infer from layout.
  VkImageLayout used_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  VkImageLayout final_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  std::uint32_t access{0};
  std::uint32_t stage{0};
  //! Layout before the first graph use when an external pass produced it.
  VkImageLayout initial_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  //! Access/stage of an external producer when this is the first graph use.
  //! These are needed for a real render-pass -> compute dependency.
  std::uint32_t initial_access{0};
  std::uint32_t initial_stage{0};
};

struct GraphPass {
  const char* name{nullptr};
  VkRenderPass render_pass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  std::vector<RenderPassAttachment> attachments;
  //! Buffer dependencies this pass CONSUMES (barriers run before the pass).
  std::vector<GraphBufferEdge> buffer_edges;
  //! Images this pass SAMPLES (shader reads, e.g. a shadow map written by an
  //! earlier pass). Barriers run before the pass; must NOT alias attachments.
  std::vector<GraphSampledImage> sampled_images;
  //! Clear values handed to the record callback (ownership stays with caller).
  const VkClearValue* clear_values{nullptr};
  std::uint32_t clear_value_count{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  //! Opaque handle returned to the record callback (e.g. the pass index).
  void* user_data{nullptr};
};

//! One compute pass: dispatches recorded through a callback (no render pass).
struct GraphBarrier {
  VkImage image{VK_NULL_HANDLE};
  VkImageLayout old_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  VkImageLayout new_layout{VK_IMAGE_LAYOUT_UNDEFINED};
  std::uint32_t src_access{0};
  std::uint32_t dst_access{0};
  std::uint32_t src_stage{0};
  std::uint32_t dst_stage{0};
  std::uint32_t base_mip{0};
  std::uint32_t level_count{1};
  std::uint32_t aspect{0}; //!< VkImageAspectFlags; 0 means infer from layout.
};

struct GraphComputePass {
  const char* name{nullptr};
  std::uint32_t group_count_x{0};
  std::uint32_t group_count_y{1};
  std::uint32_t group_count_z{1};
  //! Buffer resource edges the pass needs synchronized (see GraphBufferEdge).
  std::vector<GraphBufferEdge> buffer_edges;
  //! Image subresources the pass reads/writes, including individual H-Z mips.
  std::vector<GraphImageUse> image_uses;
  //! Opaque handle returned to the record callback.
  void* user_data{nullptr};
};

//! An ordered graph node: either a render pass or a compute pass.
struct GraphNode {
  const GraphPass* render{nullptr};
  const GraphComputePass* compute{nullptr};
  [[nodiscard]] static GraphNode from_render(const GraphPass& p) noexcept { return {&p, nullptr}; }
  [[nodiscard]] static GraphNode from_compute(const GraphComputePass& p) noexcept { return {nullptr, &p}; }
};

//! A compiled buffer dependency edge between two passes (producer → this
//! consumer). When producer and consumer land on different queue families,
//! the executor emits an ownership RELEASE on the producer side and ACQUIRE
//! on the consumer side; within one queue it is a plain buffer barrier.
//! Compiled plan for a mixed render/compute graph.
struct CompiledGraph {
  //! Per node: image barriers to insert before it (layout transitions and
  //! write edges, computed exactly as before).
  std::vector<std::vector<GraphBarrier>> barriers_per_node;
  //! Per node: buffer edges to synchronize before it.
  std::vector<std::vector<GraphBufferEdge>> buffer_edges_per_node;
};

//!
//! @brief Compiles a mixed render/compute node sequence.
//!
//! Image tracking is unchanged; buffer edges are validated (non-null buffer,
//! non-zero stages) and passed through to the executor, which emits either a
//! plain barrier or a family release/acquire pair depending on the families.
[[nodiscard]] CompiledGraph compile_graph(
    const std::vector<GraphNode>& nodes);

/**
 * @brief Executes a compiled mixed graph inside one command buffer.
 *
 * Compute passes run their record callback outside any render pass; render
 * passes behave exactly as execute_render_graph. Buffer edges whose producer
 * family differs from the consumer family emit release (producer stage, on
 * the node BEFORE the consumer) and acquire halves — callers driving a
 * multi-queue split pass each queue's own sub-sequence and call this once
 * per queue; single-queue graphs get plain barriers only.
 */
void execute_graph(
    VkCommandBuffer command_buffer,
    const std::vector<GraphNode>& nodes,
    const CompiledGraph& compiled,
    void (*record_render)(VkCommandBuffer, const GraphPass&, void*),
    void (*record_compute)(VkCommandBuffer, const GraphComputePass&, void*),
    std::uint32_t current_family = kIgnoredQueueFamily);

//! A computed transition inserted before a pass.
//! Compiled barrier plan: barriers_[pass_index] run before that pass.
struct CompiledRenderGraph {
  std::vector<std::vector<GraphBarrier>> barriers_per_pass;
};

/**
 * @brief Computes the minimal barrier set between declared passes.
 *
 * The compiler tracks each image's current layout/access/stage across the
 * declared sequence. When a pass needs a different layout or when writes
 * follow reads, it emits exactly the transitions required — no manual
 * barrier authoring, no hidden state.
 */
[[nodiscard]] CompiledRenderGraph compile_render_graph(
    const std::vector<GraphPass>& passes);

/**
 * @brief Executes a compiled graph inside one command buffer.
 *
 * @param command_buffer  Buffer in the recording state (caller began it).
 * @param passes          Same pass list given to compile_render_graph.
 * @param compiled        Output of compile_render_graph.
 * @param record_pass     Callback recording the pass's commands; receives
 *                        the command buffer and pass->user_data. It runs
 *                        inside an active render pass and must not end it.
 */
void execute_render_graph(
    VkCommandBuffer command_buffer,
    const std::vector<GraphPass>& passes,
    const CompiledRenderGraph& compiled,
    void (*record_pass)(VkCommandBuffer, const GraphPass&, void*));

} // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef WARPLOOM_COMPAT_RENDER_NS
#define WARPLOOM_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // WARPLOOM_COMPAT_RENDER_NS
