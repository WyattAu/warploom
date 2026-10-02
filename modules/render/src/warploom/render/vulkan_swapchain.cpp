#include "warploom/render/vulkan_swapchain.hpp"
#include <algorithm>
#include <limits>

namespace warploom::render {

VulkanSwapchain::~VulkanSwapchain() { cleanup(VK_NULL_HANDLE); }

::warploom::core::Result<void> VulkanSwapchain::query_support(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
#ifdef OMNICPP_HAS_VULKAN
  if (!physical_device || !surface) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  return query_swapchain_support(physical_device, surface).is_valid()
      ? ::warploom::core::Result<void>::ok()
      : ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#else
  (void)physical_device; (void)surface;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanSwapchain::create(
    VkDevice device, VkPhysicalDevice physical_device,
    VkSurfaceKHR surface, const SwapchainConfig& config) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device || !surface || config.width == 0 || config.height == 0) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }

  const auto support = query_swapchain_support(physical_device, surface);
  if (!support.is_valid()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  const auto surface_format = choose_surface_format(support.formats);
  const auto present_mode = choose_present_mode(support.present_modes, config.vsync);
  const auto extent = choose_extent(support.capabilities, config.width, config.height);

  std::uint32_t image_count = support.capabilities.minImageCount + 1;
  if (support.capabilities.maxImageCount > 0 && image_count > support.capabilities.maxImageCount) {
    image_count = support.capabilities.maxImageCount;
  }

  std::uint32_t graphics_family = UINT32_MAX;
  std::uint32_t present_family = UINT32_MAX;
  std::uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
  for (std::uint32_t i = 0; i < family_count; ++i) {
    if (graphics_family == UINT32_MAX && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      graphics_family = i;
    }
    VkBool32 present_support = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &present_support) == VK_SUCCESS && present_support) {
      if (present_family == UINT32_MAX) present_family = i;
    }
  }
  if (graphics_family == UINT32_MAX || present_family == UINT32_MAX) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = surface;
  info.minImageCount = image_count;
  info.imageFormat = surface_format.format;
  info.imageColorSpace = surface_format.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  const VkImageUsageFlags required_usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if ((support.capabilities.supportedUsageFlags & required_usage) != required_usage) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  info.imageUsage = required_usage;

  const std::uint32_t queue_indices[] = {graphics_family, present_family};
  if (graphics_family != present_family) {
    info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = 2;
    info.pQueueFamilyIndices = queue_indices;
  } else {
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  info.preTransform = support.capabilities.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;

  VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
  const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &new_swapchain);
  if (result != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
  swapchain_ = new_swapchain;
  image_format_ = surface_format.format;
  extent_width_ = extent.width;
  extent_height_ = extent.height;

  std::uint32_t count = 0;
  vkGetSwapchainImagesKHR(device, swapchain_, &count, nullptr);
  images_.resize(count);
  vkGetSwapchainImagesKHR(device, swapchain_, &count, images_.data());
  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)physical_device; (void)surface; (void)config;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanSwapchain::create_image_views(VkDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || images_.empty()) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }
  // Rebuilding views is valid only after the caller has retired any
  // framebuffers that reference the previous views.
  if (!image_views_.empty()) {
    for (auto view : image_views_) {
      if (view) vkDestroyImageView(device, view, nullptr);
    }
    image_views_.clear();
  }
  image_views_.resize(images_.size(), VK_NULL_HANDLE);
  for (std::size_t i = 0; i < images_.size(); ++i) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = images_[i];
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = image_format_;
    info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    const VkResult result = vkCreateImageView(device, &info, nullptr, &image_views_[i]);
    if (result != VK_SUCCESS) {
      cleanup(device);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
  }
  return ::warploom::core::Result<void>::ok();
#else
  (void)device;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanSwapchain::cleanup([[maybe_unused]] VkDevice device) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device) {
    for (auto view : image_views_) if (view) vkDestroyImageView(device, view, nullptr);
    if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
  }
#endif
  image_views_.clear();
  images_.clear();
  swapchain_ = VK_NULL_HANDLE;
  image_format_ = VK_FORMAT_UNDEFINED;
  extent_width_ = 0;
  extent_height_ = 0;
}

::warploom::core::Result<void> VulkanSwapchain::recreate(
    VkDevice device, VkPhysicalDevice physical_device,
    VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device || !surface) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (device) {
    for (auto view : image_views_) if (view) vkDestroyImageView(device, view, nullptr);
  }
#endif
  image_views_.clear();
  SwapchainConfig config;
  config.width = width;
  config.height = height;
  auto result = create(device, physical_device, surface, config);
  if (!result.is_ok()) return result;
  return create_image_views(device);
}

SwapchainSupportDetails VulkanSwapchain::query_swapchain_support(
    VkPhysicalDevice device, VkSurfaceKHR surface) {
  SwapchainSupportDetails details;
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !surface) return details;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
  std::uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
  details.formats.resize(format_count);
  if (format_count) vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, details.formats.data());
  std::uint32_t mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, nullptr);
  details.present_modes.resize(mode_count);
  if (mode_count) vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &mode_count, details.present_modes.data());
#else
  (void)device; (void)surface;
#endif
  return details;
}

#if OMNICPP_VULKAN_TYPES_AVAILABLE
VkSurfaceFormatKHR VulkanSwapchain::choose_surface_format(
    const std::vector<VkSurfaceFormatKHR>& available) {
  for (const auto& format : available) {
    if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return format;
  }
  for (const auto& format : available) {
    if (format.format == VK_FORMAT_B8G8R8A8_UNORM) return format;
  }
  return available.empty() ? VkSurfaceFormatKHR{} : available.front();
}

VkPresentModeKHR VulkanSwapchain::choose_present_mode(
    const std::vector<VkPresentModeKHR>& available, bool vsync) {
  if (!vsync) {
    for (auto mode : available) if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
  }
  for (auto mode : available) if (mode == VK_PRESENT_MODE_FIFO_KHR) return mode;
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::choose_extent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    std::uint32_t width, std::uint32_t height) {
  if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) return capabilities.currentExtent;
  return {std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
          std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
}
#else
VkFormat VulkanSwapchain::choose_surface_format(const std::vector<VkFormat>& available) {
  return available.empty() ? VK_FORMAT_UNDEFINED : available.front();
}

VkPresentModeKHR VulkanSwapchain::choose_present_mode(
    const std::vector<VkPresentModeKHR>& available, bool vsync) {
  if (!vsync) for (auto mode : available) if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
  for (auto mode : available) if (mode == VK_PRESENT_MODE_FIFO_KHR) return mode;
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::choose_extent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    std::uint32_t width, std::uint32_t height) {
  if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) return capabilities.currentExtent;
  return {std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
          std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
}
#endif

} // namespace warploom::render
