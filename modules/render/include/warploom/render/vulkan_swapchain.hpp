#pragma once

/**
 * @file vulkan_swapchain.hpp
 * @brief Vulkan swapchain management.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>
#include <vector>

namespace omnicpp::render {

struct SwapchainSupportDetails {
#if OMNICPP_VULKAN_TYPES_AVAILABLE
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkSurfaceFormatKHR> formats;
#else
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkFormat> formats;
#endif
  std::vector<VkPresentModeKHR> present_modes;

  [[nodiscard]] bool is_valid() const noexcept {
    return !formats.empty() && !present_modes.empty();
  }
};

struct SwapchainConfig {
  std::uint32_t width{800};
  std::uint32_t height{600};
  bool vsync{true};
  std::uint32_t max_frames_in_flight{2};
};

class VulkanSwapchain final {
public:
  VulkanSwapchain() = default;
  ~VulkanSwapchain();

  VulkanSwapchain(const VulkanSwapchain&) = delete;
  VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;
  VulkanSwapchain(VulkanSwapchain&&) = delete;
  VulkanSwapchain& operator=(VulkanSwapchain&&) = delete;

  [[nodiscard]] omnicpp::core::Result<void> query_support(
      VkPhysicalDevice physical_device, VkSurfaceKHR surface);
  [[nodiscard]] omnicpp::core::Result<void> create(
      VkDevice device, VkPhysicalDevice physical_device,
      VkSurfaceKHR surface, const SwapchainConfig& config);
  [[nodiscard]] omnicpp::core::Result<void> create_image_views(VkDevice device);
  void cleanup(VkDevice device) noexcept;
  [[nodiscard]] omnicpp::core::Result<void> recreate(
      VkDevice device, VkPhysicalDevice physical_device,
      VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height);

  [[nodiscard]] bool is_valid() const noexcept { return swapchain_ != nullptr; }
  [[nodiscard]] VkSwapchainKHR swapchain() const noexcept { return swapchain_; }
  [[nodiscard]] std::uint32_t image_count() const noexcept { return static_cast<std::uint32_t>(images_.size()); }
  [[nodiscard]] VkFormat image_format() const noexcept { return image_format_; }
  [[nodiscard]] std::uint32_t extent_width() const noexcept { return extent_width_; }
  [[nodiscard]] std::uint32_t extent_height() const noexcept { return extent_height_; }
  [[nodiscard]] const std::vector<VkImage>& images() const noexcept { return images_; }
  [[nodiscard]] const std::vector<VkImageView>& image_views() const noexcept { return image_views_; }
  [[nodiscard]] VkImageView image_view(std::uint32_t index) const noexcept { return image_views_[index]; }

  [[nodiscard]] static SwapchainSupportDetails query_swapchain_support(
      VkPhysicalDevice device, VkSurfaceKHR surface);
#if OMNICPP_VULKAN_TYPES_AVAILABLE
  [[nodiscard]] static VkSurfaceFormatKHR choose_surface_format(
      const std::vector<VkSurfaceFormatKHR>& available);
  [[nodiscard]] static VkPresentModeKHR choose_present_mode(
      const std::vector<VkPresentModeKHR>& available, bool vsync);
  [[nodiscard]] static VkExtent2D choose_extent(
      const VkSurfaceCapabilitiesKHR& capabilities,
      std::uint32_t width, std::uint32_t height);
#else
  [[nodiscard]] static VkFormat choose_surface_format(
      const std::vector<VkFormat>& available);
  [[nodiscard]] static VkPresentModeKHR choose_present_mode(
      const std::vector<VkPresentModeKHR>& available, bool vsync);
  [[nodiscard]] static VkExtent2D choose_extent(
      const VkSurfaceCapabilitiesKHR& capabilities,
      std::uint32_t width, std::uint32_t height);
#endif

private:
  VkSwapchainKHR swapchain_{nullptr};
  VkFormat image_format_{VK_FORMAT_UNDEFINED};
  std::uint32_t extent_width_{0};
  std::uint32_t extent_height_{0};
  std::vector<VkImage> images_;
  std::vector<VkImageView> image_views_;
};

} // namespace omnicpp::render
