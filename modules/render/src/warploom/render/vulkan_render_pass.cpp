/**
 * @file vulkan_render_pass.cpp
 * @brief Vulkan render pass and framebuffer implementation.
 */

#include "warploom/render/vulkan_render_pass.hpp"
#include <cstring>

#ifdef OMNICPP_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace warploom::render {

VulkanRenderPass::~VulkanRenderPass() { cleanup(device_); }

::warploom::core::Result<void> VulkanRenderPass::create(
    VkDevice device, VkFormat color_format, VkFormat depth_format,
    VkImageLayout final_layout) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  device_ = device;  // For the destructor; see owning_device().
  depth_format_ = depth_format;

  // Color attachment
  VkAttachmentDescription color_attachment{};
  color_attachment.format = color_format;
  color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color_attachment.finalLayout = final_layout;

  // Depth attachment
  VkAttachmentDescription depth_attachment{};
  depth_attachment.format = depth_format;
  depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  // Color attachment reference
  VkAttachmentReference color_ref{};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  // Depth attachment reference
  VkAttachmentReference depth_ref{};
  depth_ref.attachment = 1;
  depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  // Subpass
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;
  subpass.pDepthStencilAttachment = &depth_ref;

  // Subpass dependency (implicit external → subpass transition)
  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  // Create render pass
  VkAttachmentDescription attachments[] = {color_attachment, depth_attachment};

  VkRenderPassCreateInfo render_pass_info{};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = 2;
  render_pass_info.pAttachments = attachments;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  render_pass_info.dependencyCount = 1;
  render_pass_info.pDependencies = &dependency;

  VkResult result = vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass_);
  if (result != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)color_format; (void)depth_format; (void)final_layout;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderPass::create_framebuffers(
    VkDevice device, const std::vector<VkImageView>& swapchain_views,
    std::uint32_t width, std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || swapchain_views.empty() || !render_pass_) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  framebuffers_.resize(swapchain_views.size());

  for (std::size_t i = 0; i < swapchain_views.size(); ++i) {
    VkImageView attachments[] = {swapchain_views[i], depth_view_};

    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass_;
    fb_info.attachmentCount = 2;
    fb_info.pAttachments = attachments;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;

    VkResult result = vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffers_[i]);
    if (result != VK_SUCCESS) {
      // Clean up already-created framebuffers
      for (std::size_t j = 0; j < i; ++j) {
        if (framebuffers_[j]) {
          vkDestroyFramebuffer(device, framebuffers_[j], nullptr);
        }
      }
      framebuffers_.clear();
      return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
    }
  }

  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)swapchain_views; (void)width; (void)height;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

::warploom::core::Result<void> VulkanRenderPass::create_depth_resources(
    VkDevice device, VkPhysicalDevice physical_device,
    VkFormat format, std::uint32_t width, std::uint32_t height) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device || !physical_device) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  depth_format_ = format;

  // Create depth image
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.format = format;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  VkFormatProperties format_properties{};
  vkGetPhysicalDeviceFormatProperties(physical_device, format,
                                      &format_properties);
  depth_sampleable_ = false;
  VkImageFormatProperties usage_properties{};
  const VkImageUsageFlags sampled_usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
      vkGetPhysicalDeviceImageFormatProperties(
          physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
          sampled_usage, 0, &usage_properties) == VK_SUCCESS) {
    image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    depth_sampleable_ = true;
  }
  const VkImageUsageFlags transfer_usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (vkGetPhysicalDeviceImageFormatProperties(
          physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
          transfer_usage, 0, &usage_properties) == VK_SUCCESS) {
    image_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;

  VkResult result = vkCreateImage(device, &image_info, nullptr, &depth_image_);
  if (result != VK_SUCCESS) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  // Allocate memory for depth image
  VkMemoryRequirements mem_requirements;
  vkGetImageMemoryRequirements(device, depth_image_, &mem_requirements);

  // Find memory type
  VkPhysicalDeviceMemoryProperties mem_properties;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

  std::uint32_t memory_type_index = 0;
  bool found = false;
  for (std::uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
    if ((mem_properties.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
        (mem_requirements.memoryTypeBits & (1 << i))) {
      memory_type_index = i;
      found = true;
      break;
    }
  }
  if (!found) {
    vkDestroyImage(device, depth_image_, nullptr);
    depth_image_ = nullptr;
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  VkMemoryAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc_info.allocationSize = mem_requirements.size;
  alloc_info.memoryTypeIndex = memory_type_index;

  result = vkAllocateMemory(device, &alloc_info, nullptr, &depth_memory_);
  if (result != VK_SUCCESS) {
    vkDestroyImage(device, depth_image_, nullptr);
    depth_image_ = nullptr;
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

  result = vkBindImageMemory(device, depth_image_, depth_memory_, 0);
  if (result != VK_SUCCESS) {
    vkFreeMemory(device, depth_memory_, nullptr);
    vkDestroyImage(device, depth_image_, nullptr);
    depth_image_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::vulkan_not_available);
  }

  // Create depth image view
  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = depth_image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view_info.subresourceRange.baseMipLevel = 0;
  view_info.subresourceRange.levelCount = 1;
  view_info.subresourceRange.baseArrayLayer = 0;
  view_info.subresourceRange.layerCount = 1;

  result = vkCreateImageView(device, &view_info, nullptr, &depth_view_);
  if (result != VK_SUCCESS) {
    vkFreeMemory(device, depth_memory_, nullptr);
    vkDestroyImage(device, depth_image_, nullptr);
    depth_image_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
    depth_sampleable_ = false;
    return ::warploom::core::Result<void>::error(
        ::warploom::core::RuntimeError::vulkan_not_available);
  }

  return ::warploom::core::Result<void>::ok();
#else
  (void)device; (void)physical_device; (void)format; (void)width; (void)height;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

void VulkanRenderPass::cleanup([[maybe_unused]] VkDevice device) noexcept {
#ifdef OMNICPP_HAS_VULKAN
  if (device) {
    for (auto fb : framebuffers_) {
      if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    if (depth_view_) vkDestroyImageView(device, depth_view_, nullptr);
    if (depth_image_) vkDestroyImage(device, depth_image_, nullptr);
    if (depth_memory_) vkFreeMemory(device, depth_memory_, nullptr);
    if (render_pass_) vkDestroyRenderPass(device, render_pass_, nullptr);
  }
  framebuffers_.clear();
  render_pass_ = nullptr;
  depth_image_ = nullptr;
  depth_memory_ = nullptr;
  depth_view_ = nullptr;
  depth_format_ = VK_FORMAT_UNDEFINED;
  depth_sampleable_ = false;
  device_ = VK_NULL_HANDLE;
#endif
}

VkFormat VulkanRenderPass::find_supported_depth_format([[maybe_unused]] VkPhysicalDevice device) {
#ifdef OMNICPP_HAS_VULKAN
  if (!device) return VK_FORMAT_UNDEFINED;

  VkFormat candidates[] = {
      VK_FORMAT_D32_SFLOAT,
      VK_FORMAT_D32_SFLOAT_S8_UINT,
      VK_FORMAT_D24_UNORM_S8_UINT};

  for (auto format : candidates) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      return format;
    }
  }
#endif
  return VK_FORMAT_UNDEFINED;
}

} // namespace warploom::render
