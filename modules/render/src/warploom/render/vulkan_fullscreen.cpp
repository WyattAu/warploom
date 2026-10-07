//! @file vulkan_fullscreen.cpp
//! @brief Full-screen-triangle pass recording (see vulkan_fullscreen.hpp for
//!        why this is shared infrastructure rather than renderer internals).

#include <warploom/render/vulkan_fullscreen.hpp>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

::warploom::core::Result<void> record_fullscreen_draw(
    VkCommandBuffer command_buffer, const FullscreenPass& pass,
    VkDescriptorSet set0) {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || pass.pipeline == VK_NULL_HANDLE ||
      pass.pipeline_layout == VK_NULL_HANDLE) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }
  // Dynamic viewport/scissor: safe inside an active render pass (graph
  // callback path) and idempotent before one (direct path).
  VkViewport viewport{};
  viewport.width = static_cast<float>(pass.width);
  viewport.height = static_cast<float>(pass.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {pass.width, pass.height};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);

  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pass.pipeline);
  if (set0 != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pass.pipeline_layout, 0, 1, &set0, 0, nullptr);
  }
  if (pass.push_data != nullptr && pass.push_size > 0U) {
    vkCmdPushConstants(command_buffer, pass.pipeline_layout,
                       pass.push_stage_flags, 0U, pass.push_size,
                       pass.push_data);
  }
  vkCmdDraw(command_buffer, pass.vertex_count, pass.instance_count, 0, 0);
  return ::warploom::core::Result<void>::ok();
#else
  (void)command_buffer;
  (void)pass;
  (void)set0;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> record_fullscreen_pass(
    VkCommandBuffer command_buffer, const FullscreenPass& pass,
    VkDescriptorSet set0) {
#ifdef OMNICPP_HAS_VULKAN
  if (!command_buffer || pass.render_pass == VK_NULL_HANDLE ||
      pass.framebuffer == VK_NULL_HANDLE || pass.width == 0U ||
      pass.height == 0U) {
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::invalid_config);
  }

  VkRenderPassBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = pass.render_pass;
  begin.framebuffer = pass.framebuffer;
  begin.renderArea.extent = {pass.width, pass.height};
  begin.clearValueCount = pass.clear_value_count;
  begin.pClearValues = pass.clear_values;
  vkCmdBeginRenderPass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);

  const auto draw = record_fullscreen_draw(command_buffer, pass, set0);
  vkCmdEndRenderPass(command_buffer);
  return draw;
#else
  (void)command_buffer;
  (void)pass;
  (void)set0;
  return ::warploom::core::Result<void>::error(
      ::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

GraphPass fullscreen_graph_pass(const FullscreenPass& pass) {
  GraphPass out{};
  out.name = "fullscreen";
  out.render_pass = pass.render_pass;
  out.framebuffer = pass.framebuffer;
  out.width = pass.width;
  out.height = pass.height;
  out.clear_values = pass.clear_values;
  out.clear_value_count = pass.clear_value_count;
  for (std::uint32_t i = 0; i < pass.sample_count && i < pass.samples.size();
       ++i) {
    GraphSampledImage s{};
    s.image = pass.samples[i].image;
    s.used_layout = pass.samples[i].layout;
    s.aspect = pass.samples[i].aspect;
    out.sampled_images.push_back(s);
  }
  return out;
}

}  // namespace warploom::render
