//! @file test_dynamic_rendering.cpp
//! @brief Blade stage 0: dynamic-rendering negotiation + clear-only proof.
//!
//! The device enables `dynamicRendering` (Vulkan 1.3 core, no extension)
//! when the physical device advertises it, and the flag gates migration of
//! the fullscreen passes in later stages. This test proves the path end to
//! end on a real device: begin/end a `VkRenderingInfo` with a CLEAR load
//! op over a pre-filled image and read back the cleared pixels — no render
//! pass object anywhere. Skips (with reason) where the feature is absent.
//! Sabotage-checked both directions: skipping begin/end leaves the
//! pre-fill color (fails the red assertion); a DONT_CARE load op does too.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace {

using warploom::render::VulkanContext;
using warploom::render::VulkanMemoryAllocator;

#if defined(WARPLOOM_HAS_VULKAN)

struct DynamicRenderingHarness {
  VulkanContext context;
  VulkanMemoryAllocator allocator;
  VkCommandPool pool{VK_NULL_HANDLE};

  bool initialize() {
    if (!VulkanContext::is_available()) return false;
    if (!context.initialize("DynamicRenderingTest", true).is_ok()) {
      return false;
    }
    if (!allocator
             .initialize(context.device(), context.physical_device())
             .is_ok()) {
      context.cleanup();
      return false;
    }
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = static_cast<std::uint32_t>(
        context.queue_families().graphics_family);
    if (vkCreateCommandPool(context.device(), &pool_info, nullptr, &pool) !=
        VK_SUCCESS) {
      allocator.cleanup();
      context.cleanup();
      return false;
    }
    return true;
  }

  void cleanup() {
    if (pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(context.device(), pool, nullptr);
      pool = VK_NULL_HANDLE;
    }
    allocator.cleanup();
    context.cleanup();
  }
};

void transition_image(VkCommandBuffer cb, VkImage image,
                      VkImageLayout old_layout, VkImageLayout new_layout,
                      VkAccessFlags src_access, VkAccessFlags dst_access,
                      VkPipelineStageFlags src_stage,
                      VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0U;
  barrier.subresourceRange.levelCount = 1U;
  barrier.subresourceRange.baseArrayLayer = 0U;
  barrier.subresourceRange.layerCount = 1U;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0U, 0U, nullptr, 0U,
                       nullptr, 1U, &barrier);
}

#endif  // defined(WARPLOOM_HAS_VULKAN)

}  // namespace

#if defined(WARPLOOM_HAS_VULKAN)

// Clear-only dynamic-rendering pass: pre-fill blue, then a VkRenderingInfo
// with loadOp=CLEAR red must overwrite every pixel. No VkRenderPass object
// exists in this test by construction.
TEST(DynamicRendering, ClearOnlyPassWritesAttachment) {
  DynamicRenderingHarness h;
  if (!h.initialize()) GTEST_SKIP() << "no Vulkan device";
  if (!h.context.has_dynamic_rendering()) {
    h.cleanup();
    GTEST_SKIP() << "dynamicRendering not advertised";
  }

  constexpr std::uint32_t kW = 64U;
  constexpr std::uint32_t kH = 64U;
  VkImage image{VK_NULL_HANDLE};
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {kW, kH, 1U};
  image_info.mipLevels = 1U;
  image_info.arrayLayers = 1U;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ASSERT_EQ(vkCreateImage(h.context.device(), &image_info, nullptr, &image),
            VK_SUCCESS);
  auto binding =
      h.allocator.bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(binding.is_ok());

  VkImageView view{VK_NULL_HANDLE};
  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view_info.subresourceRange.levelCount = 1U;
  view_info.subresourceRange.layerCount = 1U;
  ASSERT_EQ(vkCreateImageView(h.context.device(), &view_info, nullptr, &view),
            VK_SUCCESS);

  VkCommandBufferAllocateInfo alloc_info{};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = h.pool;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1U;
  VkCommandBuffer cb{VK_NULL_HANDLE};
  ASSERT_EQ(vkAllocateCommandBuffers(h.context.device(), &alloc_info, &cb),
            VK_SUCCESS);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);

  // Pre-fill blue: a skipped or DONT_CARE dynamic pass leaves blue behind.
  transition_image(cb, image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0U,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkClearColorValue blue{};
  blue.float32[2] = 1.0F;
  blue.float32[3] = 1.0F;
  VkImageSubresourceRange range{};
  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  range.levelCount = 1U;
  range.layerCount = 1U;
  vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue,
                       1U, &range);
  transition_image(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  VkClearValue clear_red{};
  clear_red.color.float32[0] = 1.0F;
  clear_red.color.float32[3] = 1.0F;
  VkRenderingAttachmentInfo color_attachment{};
  color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color_attachment.imageView = view;
  color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color_attachment.clearValue = clear_red;
  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea = {{0, 0}, {kW, kH}};
  rendering.layerCount = 1U;
  rendering.colorAttachmentCount = 1U;
  rendering.pColorAttachments = &color_attachment;
  vkCmdBeginRendering(cb, &rendering);
  vkCmdEndRendering(cb);

  transition_image(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
  auto buf = h.allocator.create_buffer(
      static_cast<VkDeviceSize>(kW * kH * 4U),
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(buf.is_ok());
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
  region.imageExtent = {kW, kH, 1U};
  vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         buf.value().buffer, 1U, &region);
  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(h.context.graphics_queue(), 1U, &submit,
                          VK_NULL_HANDLE),
            VK_SUCCESS);
  ASSERT_EQ(vkQueueWaitIdle(h.context.graphics_queue()), VK_SUCCESS);

  const auto* mapped =
      static_cast<const std::uint8_t*>(buf.value().mapped);
  ASSERT_NE(mapped, nullptr);
  for (std::uint32_t i = 0U; i < kW * kH; ++i) {
    EXPECT_EQ(mapped[i * 4U + 0U], 255U) << "pixel " << i << " R";
    EXPECT_EQ(mapped[i * 4U + 1U], 0U) << "pixel " << i << " G";
    EXPECT_EQ(mapped[i * 4U + 2U], 0U) << "pixel " << i << " B";
    EXPECT_EQ(mapped[i * 4U + 3U], 255U) << "pixel " << i << " A";
  }
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  warploom::render::Allocation live = buf.value();
  h.allocator.destroy_allocation(live);
  vkDestroyImageView(h.context.device(), view, nullptr);
  warploom::render::Allocation img_live = binding.value();
  h.allocator.destroy_allocation(img_live);
  vkDestroyImage(h.context.device(), image, nullptr);
  vkFreeCommandBuffers(h.context.device(), h.pool, 1U, &cb);
  h.cleanup();
}

#endif  // defined(WARPLOOM_HAS_VULKAN)
