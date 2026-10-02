#pragma once

/**
 * @file vulkan_render_pass.hpp
 * @brief Vulkan render pass and framebuffer management.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>
#include <vector>

namespace warploom::render {

class VulkanRenderPass final {
public:
  VulkanRenderPass() = default;
  ~VulkanRenderPass();

  VulkanRenderPass(const VulkanRenderPass&) = delete;
  VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;
  VulkanRenderPass(VulkanRenderPass&&) = delete;
  VulkanRenderPass& operator=(VulkanRenderPass&&) = delete;

  [[nodiscard]] ::warploom::core::Result<void> create(
      VkDevice device, VkFormat color_format, VkFormat depth_format,
      VkImageLayout final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  [[nodiscard]] ::warploom::core::Result<void> create_framebuffers(
      VkDevice device, const std::vector<VkImageView>& swapchain_views,
      std::uint32_t width, std::uint32_t height);

  [[nodiscard]] ::warploom::core::Result<void> create_depth_resources(
      VkDevice device, VkPhysicalDevice physical_device,
      VkFormat format, std::uint32_t width, std::uint32_t height);

  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] VkRenderPass render_pass() const noexcept { return render_pass_; }
  [[nodiscard]] std::size_t framebuffer_count() const noexcept { return framebuffers_.size(); }
  [[nodiscard]] VkFramebuffer framebuffer(std::size_t index) const noexcept { return framebuffers_[index]; }
  [[nodiscard]] VkImage depth_image() const noexcept { return depth_image_; }
  [[nodiscard]] VkImageView depth_view() const noexcept { return depth_view_; }
  [[nodiscard]] VkFormat depth_format() const noexcept { return depth_format_; }
  //! True when the depth attachment was created with sampled-image usage.
  [[nodiscard]] bool depth_is_sampleable() const noexcept { return depth_sampleable_; }

  [[nodiscard]] static VkFormat find_supported_depth_format(VkPhysicalDevice device);

private:
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  std::vector<VkFramebuffer> framebuffers_;
  VkImage depth_image_{VK_NULL_HANDLE};
  VkDeviceMemory depth_memory_{VK_NULL_HANDLE};
  VkImageView depth_view_{VK_NULL_HANDLE};
  VkFormat depth_format_{VK_FORMAT_UNDEFINED};
  bool depth_sampleable_{false};
};

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
