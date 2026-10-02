#include "warploom/render/vulkan_hiz_pyramid.hpp"

#include <algorithm>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

VulkanHiZPyramid::~VulkanHiZPyramid() { cleanup(device_); }

::warploom::core::Result<void> VulkanHiZPyramid::create(
    VkDevice device, VkPhysicalDevice physical_device,
    std::uint32_t width, std::uint32_t height, std::uint32_t levels,
    VulkanMemoryAllocator* allocator) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device || width == 0 || height == 0 ||
      image_ != VK_NULL_HANDLE) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  if (levels == 0) levels = mip_levels_for_extent(width, height);
  if (levels == 0 || levels > mip_levels_for_extent(width, height)) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }
  device_ = device;

  VkFormatProperties format_properties{};
  vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32_SFLOAT,
                                      &format_properties);
  const VkFormatFeatureFlags required =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  if ((format_properties.optimalTilingFeatures & required) != required) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::invalid_config);
  }

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = levels;
  image_info.arrayLayers = 1;
  image_info.format = VK_FORMAT_R32_SFLOAT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;

  if (vkCreateImage(device, &image_info, nullptr, &image_) != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  if (allocator) {
    auto bound = allocator->bind_image(image_, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!bound.is_ok()) {
      cleanup(device);
      return ::warploom::core::Result<void>::error(bound.error());
    }
    allocator_ = allocator;
    allocation_ = bound.value();
    uses_allocator_ = true;
  } else {
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image_, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    std::uint32_t memory_type = UINT32_MAX;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((requirements.memoryTypeBits & (1U << i)) != 0U &&
          (memory_properties.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
        memory_type = i;
        break;
      }
    }
    if (memory_type == UINT32_MAX) {
      cleanup(device);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &alloc_info, nullptr, &allocation_.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image_, allocation_.memory, 0) != VK_SUCCESS) {
      cleanup(device);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
    allocation_.size = requirements.size;
  }

  VkImageViewCreateInfo full_view_info{};
  full_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  full_view_info.image = image_;
  full_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  full_view_info.format = VK_FORMAT_R32_SFLOAT;
  full_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  full_view_info.subresourceRange.levelCount = levels;
  full_view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &full_view_info, nullptr, &full_view_) != VK_SUCCESS) {
    cleanup(device);
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  mip_views_.reserve(levels);
  for (std::uint32_t level = 0; level < levels; ++level) {
    VkImageViewCreateInfo view_info = full_view_info;
    view_info.subresourceRange.baseMipLevel = level;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    if (vkCreateImageView(device, &view_info, nullptr, &mip_views_.emplace_back()) != VK_SUCCESS) {
      cleanup(device);
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
  }

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.minLod = 0.0f;
  sampler_info.maxLod = static_cast<float>(levels - 1U);
  sampler_info.maxAnisotropy = 1.0f;
  if (vkCreateSampler(device, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    cleanup(device);
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  format_ = VK_FORMAT_R32_SFLOAT;
  width_ = width;
  height_ = height;
  levels_ = levels;
  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)physical_device; (void)width; (void)height;
  (void)levels; (void)allocator;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

VkImageView VulkanHiZPyramid::mip_view(std::uint32_t level) const noexcept {
  return level < mip_views_.size() ? mip_views_[level] : VK_NULL_HANDLE;
}

void VulkanHiZPyramid::cleanup(VkDevice device) noexcept {
#ifndef OMNICPP_HAS_VULKAN
  (void)device;
#endif
#ifdef OMNICPP_HAS_VULKAN
  if (!device) device = device_;
  if (device) {
    if (sampler_) vkDestroySampler(device, sampler_, nullptr);
    for (const auto view : mip_views_) {
      if (view) vkDestroyImageView(device, view, nullptr);
    }
    if (full_view_) vkDestroyImageView(device, full_view_, nullptr);
    if (image_) vkDestroyImage(device, image_, nullptr);
    if (uses_allocator_) {
      allocation_.image = VK_NULL_HANDLE;
      allocator_->destroy_allocation(allocation_);
    } else if (allocation_.memory) {
      vkFreeMemory(device, allocation_.memory, nullptr);
    }
  }
#endif
  mip_views_.clear();
  sampler_ = VK_NULL_HANDLE;
  full_view_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
  allocation_ = {};
  allocator_ = nullptr;
  uses_allocator_ = false;
  device_ = VK_NULL_HANDLE;
  format_ = VK_FORMAT_UNDEFINED;
  width_ = 0;
  height_ = 0;
  levels_ = 0;
}

} // namespace warploom::render
