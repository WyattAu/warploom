#pragma once

/**
 * @file vulkan_hiz_pyramid.hpp
 * @brief Sampled-image hierarchical-Z pyramid resource.
 *
 * Owns an R32_SFLOAT image with one view per mip and a full-chain sampled
 * view. The caller records the reduction dispatches and layout transitions;
 * this class owns the image, views, sampler, and allocator binding.
 */

#include "engine/core/deterministic_runtime.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_types.hpp"
#include <cstdint>
#include <vector>

namespace omnicpp::render {

class VulkanHiZPyramid final {
public:
  VulkanHiZPyramid() = default;
  ~VulkanHiZPyramid();
  VulkanHiZPyramid(const VulkanHiZPyramid&) = delete;
  VulkanHiZPyramid& operator=(const VulkanHiZPyramid&) = delete;
  VulkanHiZPyramid(VulkanHiZPyramid&&) = delete;
  VulkanHiZPyramid& operator=(VulkanHiZPyramid&&) = delete;

  //! Return the complete mip-chain length for a 2D extent.
  //! A 1x1 image has one level; zero extents return zero.
  [[nodiscard]] static constexpr std::uint32_t mip_levels_for_extent(
      std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0 || height == 0) return 0;
    std::uint32_t levels = 1;
    const std::uint32_t largest = width > height ? width : height;
    for (std::uint32_t extent = largest; extent > 1; extent >>= 1) ++levels;
    return levels;
  }

  //! Create a pyramid. Pass levels=0 to allocate the complete runtime chain.
  [[nodiscard]] omnicpp::core::Result<void> create(
      VkDevice device, VkPhysicalDevice physical_device,
      std::uint32_t width, std::uint32_t height, std::uint32_t levels = 0,
      VulkanMemoryAllocator* allocator = nullptr);
  //! Releases all Vulkan objects. The device and allocator must outlive this call.
  void cleanup(VkDevice device) noexcept;

  [[nodiscard]] VkImage image() const noexcept { return image_; }
  //! Full-chain view used by textureLod in the cull shader.
  [[nodiscard]] VkImageView view() const noexcept { return full_view_; }
  //! Single-mip view used as a storage-image reduction destination.
  [[nodiscard]] VkImageView mip_view(std::uint32_t level) const noexcept;
  [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] std::uint32_t levels() const noexcept { return levels_; }
  [[nodiscard]] bool is_valid() const noexcept {
    return image_ != VK_NULL_HANDLE && full_view_ != VK_NULL_HANDLE &&
           sampler_ != VK_NULL_HANDLE;
  }

private:
  VkDevice device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkImageView full_view_{VK_NULL_HANDLE};
  std::vector<VkImageView> mip_views_;
  VkSampler sampler_{VK_NULL_HANDLE};
  VulkanMemoryAllocator* allocator_{nullptr};
  Allocation allocation_{};
  bool uses_allocator_{false};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  std::uint32_t levels_{0};
};

} // namespace omnicpp::render
