#pragma once

/**
 * @file vulkan_offscreen.hpp
 * @brief Offscreen Vulkan color target for deterministic rendering tests.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>

namespace omnicpp::render {

class VulkanOffscreenTarget final {
public:
  VulkanOffscreenTarget() = default;
  ~VulkanOffscreenTarget();

  VulkanOffscreenTarget(const VulkanOffscreenTarget&) = delete;
  VulkanOffscreenTarget& operator=(const VulkanOffscreenTarget&) = delete;
  VulkanOffscreenTarget(VulkanOffscreenTarget&&) = delete;
  VulkanOffscreenTarget& operator=(VulkanOffscreenTarget&&) = delete;

  [[nodiscard]] omnicpp::core::Result<void> create(
      VkDevice device, VkPhysicalDevice physical_device,
      VkFormat format, std::uint32_t width, std::uint32_t height,
      VulkanMemoryAllocator* allocator = nullptr);

  //! Optional depth-stencil attachment. Call BEFORE create_render_pass();
  //! when present, the render pass gains a depth attachment (cleared to
  //! 1.0, stored for post-pass depth extraction) and the framebuffer includes
  //! its view.
  [[nodiscard]] omnicpp::core::Result<void> create_depth(
      VkDevice device, VkPhysicalDevice physical_device, VkFormat depth_format);

  [[nodiscard]] omnicpp::core::Result<void> create_render_pass(VkDevice device);
  [[nodiscard]] omnicpp::core::Result<void> create_framebuffer(VkDevice device);
  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] VkImage image() const noexcept { return image_; }
  [[nodiscard]] VkImageView image_view() const noexcept { return image_view_; }
  [[nodiscard]] VkImage depth_image() const noexcept { return depth_image_; }
  [[nodiscard]] VkImageView depth_view() const noexcept { return depth_view_; }
  [[nodiscard]] VkFormat depth_format() const noexcept { return depth_format_; }
  [[nodiscard]] bool has_depth() const noexcept { return depth_image_ != VK_NULL_HANDLE; }
  //! True when the depth image can be sampled in a shader.
  [[nodiscard]] bool depth_is_sampleable() const noexcept { return depth_sampleable_; }
  [[nodiscard]] VkRenderPass render_pass() const noexcept { return render_pass_; }
  [[nodiscard]] VkFramebuffer framebuffer() const noexcept { return framebuffer_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] bool is_valid() const noexcept {
    return image_ != VK_NULL_HANDLE && image_view_ != VK_NULL_HANDLE &&
           render_pass_ != VK_NULL_HANDLE && framebuffer_ != VK_NULL_HANDLE;
  }

private:
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  // Owns the image binding when created through a VulkanMemoryAllocator.
  VulkanMemoryAllocator* allocator_{nullptr};
  Allocation allocator_allocation_{};
  bool uses_allocator_{false};
  VkImageView image_view_{VK_NULL_HANDLE};
  VkImage depth_image_{VK_NULL_HANDLE};
  VkImageView depth_view_{VK_NULL_HANDLE};
  VkDeviceMemory depth_memory_{VK_NULL_HANDLE};
  VkFormat depth_format_{VK_FORMAT_UNDEFINED};
  bool depth_sampleable_{false};
  bool depth_uses_allocator_{false};
  Allocation depth_allocation_{};
  VkRenderPass render_pass_{VK_NULL_HANDLE};
  VkFramebuffer framebuffer_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
};

} // namespace omnicpp::render
