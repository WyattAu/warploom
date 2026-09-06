#include "engine/render/vulkan_offscreen.hpp"

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#include <cstring>
#endif

namespace omnicpp::render {

VulkanOffscreenTarget::~VulkanOffscreenTarget() { cleanup(VK_NULL_HANDLE); }

omnicpp::core::Result<void> VulkanOffscreenTarget::create(
    VkDevice device, VkPhysicalDevice physical_device,
    VkFormat format, std::uint32_t width, std::uint32_t height,
    VulkanMemoryAllocator* allocator) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device || format == VK_FORMAT_UNDEFINED || width == 0 || height == 0) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = format;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;

  VkResult result = vkCreateImage(device, &image_info, nullptr, &image_);
  if (result != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  if (allocator) {
    // Sub-allocated path: image memory comes from a shared device-local block.
    auto allocation = allocator->bind_image(image_, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!allocation.is_ok()) {
      cleanup(device);
      return omnicpp::core::Result<void>::error(allocation.error());
    }
    allocator_ = allocator;
    allocator_allocation_ = allocation.value();
    uses_allocator_ = true;
  } else {
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image_, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    std::uint32_t memory_type = 0;
    bool found_memory_type = false;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((requirements.memoryTypeBits & (1U << i)) != 0U &&
          (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
        memory_type = i;
        found_memory_type = true;
        break;
      }
    }
    if (!found_memory_type) {
      cleanup(device);
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
    }

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(device, &allocation, nullptr, &memory_);
    if (result != VK_SUCCESS) {
      cleanup(device);
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
    }
    result = vkBindImageMemory(device, image_, memory_, 0);
    if (result != VK_SUCCESS) {
      cleanup(device);
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
    }
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  result = vkCreateImageView(device, &view_info, nullptr, &image_view_);
  if (result != VK_SUCCESS) {
    cleanup(device);
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  format_ = format;
  width_ = width;
  height_ = height;
  return omnicpp::core::Result<void>::ok();
#else
  (void)device; (void)physical_device; (void)format; (void)width; (void)height;
  (void)allocator;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanOffscreenTarget::create_depth(
    VkDevice device, VkPhysicalDevice physical_device, VkFormat depth_format) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device || depth_format == VK_FORMAT_UNDEFINED) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  // Format must actually support depth attachment usage.
  VkImageFormatProperties fmt_props{};
  constexpr VkImageUsageFlags base_depth_usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (vkGetPhysicalDeviceImageFormatProperties(
          physical_device, depth_format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
          base_depth_usage, 0, &fmt_props) != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {width_, height_, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = depth_format;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormatProperties format_properties{};
  vkGetPhysicalDeviceFormatProperties(physical_device, depth_format,
                                      &format_properties);
  depth_sampleable_ =
      (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
  image_info.usage = base_depth_usage;
  if (depth_sampleable_) {
    VkImageFormatProperties sampled_props{};
    if (vkGetPhysicalDeviceImageFormatProperties(
            physical_device, depth_format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            base_depth_usage | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &sampled_props) == VK_SUCCESS) {
      image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    } else {
      depth_sampleable_ = false;
    }
  }
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  if (vkCreateImage(device, &image_info, nullptr, &depth_image_) != VK_SUCCESS) {
    depth_sampleable_ = false;
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }

  if (allocator_) {
    auto allocation = allocator_->bind_image(depth_image_, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!allocation.is_ok()) {
      vkDestroyImage(device, depth_image_, nullptr);
      depth_image_ = VK_NULL_HANDLE;
      return omnicpp::core::Result<void>::error(allocation.error());
    }
    depth_allocation_ = allocation.value();
    depth_uses_allocator_ = true;
  } else {
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, depth_image_, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    bool found = false;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((requirements.memoryTypeBits & (1U << i)) != 0U &&
          (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = i;
        if (vkAllocateMemory(device, &alloc, nullptr, &depth_memory_) == VK_SUCCESS &&
            vkBindImageMemory(device, depth_image_, depth_memory_, 0) == VK_SUCCESS) {
          found = true;
          break;
        }
      }
    }
    if (!found) {
      vkDestroyImage(device, depth_image_, nullptr);
      depth_image_ = VK_NULL_HANDLE;
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
    }
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = depth_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = depth_format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &view_info, nullptr, &depth_view_) != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  depth_format_ = depth_format;
  return omnicpp::core::Result<void>::ok();
#else
  (void)device; (void)physical_device; (void)depth_format;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanOffscreenTarget::create_render_pass(VkDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !image_ || render_pass_) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }

  VkAttachmentDescription color_attachment{};
  color_attachment.format = format_;
  color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  VkAttachmentDescription depth_attachment{};
  depth_attachment.format = depth_format_;
  depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  // Store, don't discard: the depth pyramid path copies this attachment out
  // after the pass (TRANSFER_SRC usage), so the resolved depth must survive.
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference color_reference{};
  color_reference.attachment = 0;
  color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depth_reference{};
  depth_reference.attachment = 1;
  depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  if (depth_image_ != VK_NULL_HANDLE) {
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;
  } else {
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
  }

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            (depth_image_ != VK_NULL_HANDLE
                                 ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                 : 0);
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             (depth_image_ != VK_NULL_HANDLE
                                  ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                  : 0);

  VkAttachmentDescription attachments[2] = {color_attachment, depth_attachment};

  VkRenderPassCreateInfo render_pass_info{};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  if (depth_image_ != VK_NULL_HANDLE) {
    render_pass_info.attachmentCount = 2;
    render_pass_info.pAttachments = attachments;
  } else {
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
  }
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  render_pass_info.dependencyCount = 1;
  render_pass_info.pDependencies = &dependency;

  const VkResult result = vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass_);
  if (result != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  return omnicpp::core::Result<void>::ok();
#else
  (void)device;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

omnicpp::core::Result<void> VulkanOffscreenTarget::create_framebuffer(VkDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !image_view_ || !render_pass_ || framebuffer_) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
  }
  VkImageView attachment_views[2] = {image_view_, depth_view_};
  VkFramebufferCreateInfo framebuffer_info{};
  framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_info.renderPass = render_pass_;
  if (depth_view_ != VK_NULL_HANDLE) {
    framebuffer_info.attachmentCount = 2;
    framebuffer_info.pAttachments = attachment_views;
  } else {
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = attachment_views;
  }
  framebuffer_info.width = width_;
  framebuffer_info.height = height_;
  framebuffer_info.layers = 1;
  const VkResult result = vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer_);
  if (result != VK_SUCCESS) {
    return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
  }
  return omnicpp::core::Result<void>::ok();
#else
  (void)device;
  return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanOffscreenTarget::cleanup(VkDevice device) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device) {
    if (framebuffer_) vkDestroyFramebuffer(device, framebuffer_, nullptr);
    if (render_pass_) vkDestroyRenderPass(device, render_pass_, nullptr);
    if (depth_view_) vkDestroyImageView(device, depth_view_, nullptr);
    if (depth_image_) vkDestroyImage(device, depth_image_, nullptr);
    if (depth_uses_allocator_) {
      depth_allocation_.image = VK_NULL_HANDLE;
      allocator_->destroy_allocation(depth_allocation_);
      depth_allocation_ = {};
      depth_uses_allocator_ = false;
    } else if (depth_memory_) {
      vkFreeMemory(device, depth_memory_, nullptr);
    }
    if (image_view_) vkDestroyImageView(device, image_view_, nullptr);
    if (image_) vkDestroyImage(device, image_, nullptr);
    if (uses_allocator_) {
      // Returns the range to the allocator's free list (no vkFreeMemory).
      allocator_allocation_.image = VK_NULL_HANDLE;
      allocator_->destroy_allocation(allocator_allocation_);
      allocator_allocation_ = {};
      uses_allocator_ = false;
    } else if (memory_) {
      vkFreeMemory(device, memory_, nullptr);
    }
  }
#else
  (void)device;
#endif
  framebuffer_ = VK_NULL_HANDLE;
  render_pass_ = VK_NULL_HANDLE;
  image_view_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  depth_image_ = VK_NULL_HANDLE;
  depth_view_ = VK_NULL_HANDLE;
  depth_memory_ = VK_NULL_HANDLE;
  depth_format_ = VK_FORMAT_UNDEFINED;
  depth_sampleable_ = false;
  format_ = VK_FORMAT_UNDEFINED;
  width_ = 0;
  height_ = 0;
}

} // namespace omnicpp::render
