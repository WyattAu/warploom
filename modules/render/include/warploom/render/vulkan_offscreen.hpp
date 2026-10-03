#pragma once

/**
 * @file vulkan_offscreen.hpp
 * @brief Offscreen Vulkan color target for deterministic rendering tests.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>

namespace warploom::render {

class VulkanOffscreenTarget final {
public:
  VulkanOffscreenTarget() = default;
  ~VulkanOffscreenTarget();

  VulkanOffscreenTarget(const VulkanOffscreenTarget&) = delete;
  VulkanOffscreenTarget& operator=(const VulkanOffscreenTarget&) = delete;
  VulkanOffscreenTarget(VulkanOffscreenTarget&&) = delete;
  VulkanOffscreenTarget& operator=(VulkanOffscreenTarget&&) = delete;

  //! \param extra_image_usage OR-ed into the color image's usage flags. The
  //! default set is COLOR_ATTACHMENT | TRANSFER_SRC. Pass
  //! VK_IMAGE_USAGE_SAMPLED_BIT for a target that is read back by a later
  //! pass -- without it, sampling the view is
  //! VUID-VkWriteDescriptorSet-descriptorType-00337, and the bindless
  //! descriptor path can fail silently instead.
  [[nodiscard]] ::warploom::core::Result<void> create(
      VkDevice device, VkPhysicalDevice physical_device,
      VkFormat format, std::uint32_t width, std::uint32_t height,
      VulkanMemoryAllocator* allocator = nullptr,
      VkImageUsageFlags extra_image_usage = 0U);

  //! Optional depth-stencil attachment. Call BEFORE create_render_pass();
  //! when present, the render pass gains a depth attachment (cleared to
  //! 1.0, stored for post-pass depth extraction) and the framebuffer includes
  //! its view.
  [[nodiscard]] ::warploom::core::Result<void> create_depth(
      VkDevice device, VkPhysicalDevice physical_device, VkFormat depth_format);

  //! \param color_final_layout overrides the color attachment's finalLayout.
  //! The default (TRANSFER_SRC_OPTIMAL) suits a readback target. A target that
  //! is immediately sampled wants COLOR_ATTACHMENT_OPTIMAL instead, so the
  //! pass performs no implicit transition and the caller owns the single
  //! explicit barrier to SHADER_READ_ONLY -- deterministic, and it keeps the
  //! validation layer's layout tracking in step with the descriptor.
  [[nodiscard]] ::warploom::core::Result<void> create_render_pass(
      VkDevice device,
      VkImageLayout color_final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  [[nodiscard]] ::warploom::core::Result<void> create_framebuffer(VkDevice device);
  //! Release every handle. Pass a real VkDevice (or omit it and use the
  //! recorded one) -- a null device is a no-op that leaks.
  void cleanup(VkDevice device = VK_NULL_HANDLE) noexcept;

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
  //! Layouts the color/depth attachments are left in when this target's
  //! render pass ends. Callers that sample the result must barrier FROM
  //! these, not from an assumed value: assuming TRANSFER_SRC_OPTIMAL where
  //! the pass actually ends in COLOR_ATTACHMENT_OPTIMAL turns the barrier
  //! into a silent no-op and the sample reads the wrong layout
  //! (VUID-vkCmdDraw-imageLayout-00344).
  [[nodiscard]] VkImageLayout color_final_layout() const noexcept {
    return color_final_layout_;
  }
  [[nodiscard]] VkImageLayout depth_final_layout() const noexcept {
    return depth_final_layout_;
  }

  [[nodiscard]] bool is_valid() const noexcept {
    return image_ != VK_NULL_HANDLE && image_view_ != VK_NULL_HANDLE &&
           render_pass_ != VK_NULL_HANDLE && framebuffer_ != VK_NULL_HANDLE;
  }

private:
  //! Device that owns every handle below. Recorded on first use so the
  //! destructor can actually release them -- VulkanOffscreenTarget used to
  //! call cleanup(VK_NULL_HANDLE), and cleanup() ignores a null device, so
  //! every offscreen target leaked its image, views, render pass and
  //! framebuffer until the device was destroyed.
  VkDevice device_{VK_NULL_HANDLE};
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
  VkImageLayout color_final_layout_{VK_IMAGE_LAYOUT_UNDEFINED};
  VkImageLayout depth_final_layout_{VK_IMAGE_LAYOUT_UNDEFINED};
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
