#pragma once

//! @file vulkan_fullscreen.hpp
//! @brief Shared full-screen-triangle pass: description, recorder, graph node.
//!
//! This used to live inside VulkanRenderer as a nested struct and two member
//! functions. Promoting it out is B3b step 1 (docs/roadmap.md): the compose
//! chain is becoming instantiable per target, and both the chain and the
//! renderer's graph callbacks need these pieces. Neither should own the
//! other's helpers -- when the renderer owned them, the chain reached back
//! into private statics, which is what made a second chain instance impossible
//! without sharing the renderer's live intermediates (the bug that reverted
//! the first B3b attempt).
//!
//! Everything here is self-contained: a FullscreenPass describes a draw
//! against caller-owned pipeline / render pass / framebuffer handles. Nothing
//! captures or references a VulkanRenderer, so any owner can record one.

#include <array>
#include <cstdint>

#include <warploom/core/deterministic_runtime.hpp>
#include <warploom/render/vulkan_types.hpp>
#include <warploom/render/vulkan_render_graph.hpp>

namespace warploom::render {

//! One full-screen triangle sampling up to 4 source images (post-process:
//! tonemap, FXAA, bloom up/downsample). Caller owns every Vulkan handle; this
//! is a pure description of a draw.
struct FullscreenPass {
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  VkRenderPass render_pass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  std::uint32_t width{0};
  std::uint32_t height{0};
  //! Clear values for the target (color+depth as declared by the render
  //! pass; may be null when every attachment load-ops LOAD).
  const VkClearValue* clear_values{nullptr};
  std::uint32_t clear_value_count{0};
  //! Images the shader samples; layouts must match the descriptor writes.
  struct Sampled {
    VkImage image{VK_NULL_HANDLE};
    VkImageLayout layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::uint32_t aspect{1};  //!< VkImageAspectFlags; 1 = COLOR_BIT.
  };
  std::array<Sampled, 4> samples{};
  std::uint32_t sample_count{0};
  //! Optional draw parameters (default: 3-vertex triangle, 1 instance).
  std::uint32_t vertex_count{3};
  std::uint32_t instance_count{1};
  //! Optional push constants. The pass's pipeline layout must declare a
  //! range covering [0, push_size) for the stages the shader reads. Used by
  //! the compose chain to carry exposure.
  const void* push_data{nullptr};
  std::uint32_t push_size{0};
  //! Stages to bind the push constants for. MUST be covered by the
  //! pass's pipeline layout: recording a stage the layout does not cover is
  //! VUID-vkCmdPushConstants-offset-01795, and a vertex stage that declares
  //! no push block still has to be excluded explicitly.
  VkShaderStageFlags push_stage_flags{
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
};

//! Record one FullscreenPass inside an ACTIVE render pass (no begin/end,
//! viewport/scissor to width x height, bind pipeline + set 0 = the caller's
//! single set, draw). Callback-compatible with graph record hooks, and a free
//! function rather than a member for the same reason the compose chain needs
//! it: a capture-free lambda cannot call a const member without `this`.
[[nodiscard]] ::warploom::core::Result<void> record_fullscreen_draw(
    VkCommandBuffer command_buffer, const FullscreenPass& pass,
    VkDescriptorSet set0);

//! Record render-pass begin + record_fullscreen_draw + end. The
//! user_data-pointer shape execute_graph's record hooks expect.
[[nodiscard]] ::warploom::core::Result<void> record_fullscreen_pass(
    VkCommandBuffer command_buffer, const FullscreenPass& pass,
    VkDescriptorSet set0);

//! Build a GraphPass for a FullscreenPass: extents/clears forwarded,
//! sampled_images filled from pass.samples so compile_graph computes the
//! producer -> sample transitions. Attachments stay empty (fullscreen
//! targets have no in-graph producer; VkRenderPass owns their layout).
//! The FullscreenPass must outlive the recording call (clear pointer).
[[nodiscard]] GraphPass fullscreen_graph_pass(const FullscreenPass& pass);

}  // namespace warploom::render
