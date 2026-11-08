//! @file test_hiz_concurrent_sharing.cpp
//! @brief Loop28 step 1: a CONCURRENT-sharing H-Z pyramid reduces
//! bit-identically to an EXCLUSIVE one.
//!
//! Step 1 makes the H-Z images (pyramids, HDR depth, swapchain depth)
//! VK_SHARING_MODE_CONCURRENT over {graphics, compute} when a dedicated
//! COMPUTE-only family exists, with zero behaviour change: the same queue
//! still records the same work. This test executes the new creation path
//! and proves it: one scene-depth clear reduced into both pyramids on the
//! graphics queue must yield identical level-0 words, and the absolute
//! value must equal the cleared depth (a mis-wired dispatch in BOTH legs
//! cannot hide behind A==B). 0 validation diagnostics proves the
//! concurrent creation itself is legal on this device.
//!
//! Skips without a dedicated compute family: CONCURRENT over one family is
//! meaningless, and cross_queue_sharing() correctly reports exclusive there
//! (covered by the skip path, not by a device assertion).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_hiz_pyramid.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"

namespace {

constexpr std::uint32_t kSize = 256U;
constexpr std::uint32_t kTile = 32U;
[[maybe_unused]] constexpr std::uint32_t kPyramidWidth = kSize / kTile;  // 8
[[maybe_unused]] constexpr float kClearDepth = 0.5f;

#ifdef WARPLOOM_HAS_VULKAN

// Bit-pattern comparison of two float images: only a Vulkan build reads an
// image back, so this helper lives inside the guard that can use it.
static std::uint32_t bits(float value) {
  std::uint32_t out = 0U;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

void transition_image(VkCommandBuffer cb, VkImage image,
                      VkImageLayout old_layout, VkImageLayout new_layout,
                      VkAccessFlags src_access, VkAccessFlags dst_access,
                      VkPipelineStageFlags src_stage,
                      VkPipelineStageFlags dst_stage,
                      VkImageAspectFlags aspect, std::uint32_t level) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.baseMipLevel = level;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                       1, &barrier);
}

#endif

}  // namespace

TEST(VulkanHardware, ConcurrentSharingMatchesExclusive) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!warploom::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  warploom::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("WarploomHizSharing", true).is_ok());
  if (!context.has_dedicated_compute()) {
    context.cleanup();
    GTEST_SKIP() << "no COMPUTE-only family: nothing to share across";
  }
  // The helper the renderer and the viewport both build from: concurrent
  // over two distinct families exactly when sharing is meaningful.
  const warploom::render::ImageSharing sharing = context.cross_queue_sharing();
  ASSERT_TRUE(sharing.concurrent_ok());

  warploom::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(),
                                   context.physical_device()).is_ok());

  // Depth target cleared to a constant: every tile maximum is then exactly
  // that constant, known on the CPU before anything runs.
  warploom::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator).is_ok());
  ASSERT_TRUE(target.create_depth(context.device(), context.physical_device(),
                                  VK_FORMAT_D32_SFLOAT).is_ok());
  if (!target.depth_is_sampleable()) {
    target.cleanup(context.device());
    allocator.cleanup();
    context.cleanup();
    GTEST_SKIP() << "D32 depth is not sampleable on this device";
  }
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  // Same pyramid twice: the old path (exclusive) and the loop28 path
  // (concurrent over graphics + compute). Level 0 only: that is the tile
  // maxima image, and higher levels derive from it.
  warploom::render::VulkanHiZPyramid pyramid_exclusive;
  warploom::render::VulkanHiZPyramid pyramid_concurrent;
  ASSERT_TRUE(pyramid_exclusive.create(context.device(),
                                       context.physical_device(),
                                       kPyramidWidth, kPyramidWidth, 1U,
                                       &allocator)
                  .is_ok());
  ASSERT_TRUE(pyramid_concurrent.create(context.device(),
                                       context.physical_device(),
                                       kPyramidWidth, kPyramidWidth, 1U,
                                       &allocator, sharing)
                  .is_ok());

  // One reduction set per pyramid: depth sampler (binding 0), level-0
  // storage destination (binding 1), preceding level (binding 2, depth at
  // level 0 -- the branch that reads it is not taken).
  warploom::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  std::vector<warploom::render::ReflectedBinding> reduce_bindings = {
      {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_COMPUTE_BIT},
      {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
       VK_SHADER_STAGE_COMPUTE_BIT},
      {0, 2, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_COMPUTE_BIT},
  };
  auto reduce_layout = descriptors.create_layout(reduce_bindings, 2);
  ASSERT_TRUE(reduce_layout.is_ok());
  const auto make_reduce_set =
      [&](warploom::render::VulkanHiZPyramid& destination) -> VkDescriptorSet {
    auto set = descriptors.allocate_set(reduce_layout.value());
    if (!set.is_ok()) {
      ADD_FAILURE() << "Could not allocate reduction descriptor set";
      return VK_NULL_HANDLE;
    }
    if (!descriptors.write_image(set.value(), 0,
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 destination.sampler(), target.depth_view(),
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
             .is_ok() ||
        !descriptors.write_image(set.value(), 1,
                                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                 VK_NULL_HANDLE, destination.mip_view(0U),
                                 VK_IMAGE_LAYOUT_GENERAL)
             .is_ok() ||
        !descriptors.write_image(set.value(), 2,
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 destination.sampler(), target.depth_view(),
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
             .is_ok()) {
      ADD_FAILURE() << "Could not write reduction descriptor set";
      return VK_NULL_HANDLE;
    }
    return set.value();
  };
  const VkDescriptorSet set_exclusive = make_reduce_set(pyramid_exclusive);
  const VkDescriptorSet set_concurrent = make_reduce_set(pyramid_concurrent);
  ASSERT_NE(set_exclusive, VK_NULL_HANDLE);
  ASSERT_NE(set_concurrent, VK_NULL_HANDLE);

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  const VkPushConstantRange reduce_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16U};
  warploom::render::VulkanPipeline reduce_pipeline;
  ASSERT_TRUE(reduce_pipeline.load_shader_stage_file(
                                     context.device(), shader_dir + "/depth_reduce_image.comp.spv",
                                     "compute").is_ok());
  ASSERT_TRUE(reduce_pipeline.create_pipeline_layout(
                                     context.device(), &reduce_layout.value(),
                                     1, &reduce_push).is_ok());
  ASSERT_TRUE(reduce_pipeline.create_compute_pipeline(
                                     context.device(),
                                     reduce_pipeline.pipeline_layout()).is_ok());

  // Readback buffer: exclusive words at [0, 64), concurrent at [64, 128).
  constexpr std::uint32_t kTiles = kPyramidWidth * kPyramidWidth;
  auto readback_buf = allocator.create_buffer(
      2U * kTiles * 4U, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(readback_buf.is_ok());

  const auto pool_result = warploom::render::VulkanRenderer::create_command_pool(
      context.device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool pool = pool_result.value();
  const auto cb_result = warploom::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer cb = cb_result.value();
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence),
            VK_SUCCESS);

  ASSERT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);

  // Clear depth to the constant (no draws): the only depth content.
  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = target.render_pass();
  render_begin.framebuffer = target.framebuffer();
  render_begin.renderArea.extent = {kSize, kSize};
  VkClearValue clears[2]{};
  clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clears[1].depthStencil = {kClearDepth, 0};
  render_begin.clearValueCount = 2;
  render_begin.pClearValues = clears;
  vkCmdBeginRenderPass(cb, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdEndRenderPass(cb);

  // Depth attachment -> sampled by the reduction.
  transition_image(cb, target.depth_image(),
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT, 0);

  // Reduce into both pyramids on the graphics queue: identical work, only
  // the images' sharing modes differ.
  const std::uint32_t pc[4] = {kSize, kSize, kTile, 0U};
  for (int leg = 0; leg < 2; ++leg) {
    auto& pyramid = leg == 0 ? pyramid_exclusive : pyramid_concurrent;
    const VkDescriptorSet set = leg == 0 ? set_exclusive : set_concurrent;
    transition_image(cb, pyramid.image(), VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL, VkAccessFlags{},
                     VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      reduce_pipeline.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            reduce_pipeline.pipeline_layout(), 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(cb, reduce_pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cb, (kPyramidWidth + 7U) / 8U, (kPyramidWidth + 7U) / 8U, 1);
    transition_image(cb, pyramid.image(), VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                     0);
  }

  // Copy both level-0 images to the readback buffer at disjoint offsets.
  for (int leg = 0; leg < 2; ++leg) {
    auto& pyramid = leg == 0 ? pyramid_exclusive : pyramid_concurrent;
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {kPyramidWidth, kPyramidWidth, 1};
    copy.bufferOffset =
        static_cast<VkDeviceSize>(leg) * kTiles * 4U;
    vkCmdCopyImageToBuffer(cb, pyramid.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buf.value().buffer, 1, &copy);
  }
  {
    VkBufferMemoryBarrier ready{};
    ready.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ready.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.buffer = readback_buf.value().buffer;
    ready.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &ready,
                         0, nullptr);
  }
  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  ASSERT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit, fence),
            VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX),
            VK_SUCCESS);

  // The verdict: identical words in both legs AND the absolute constant.
  const auto* words =
      static_cast<const std::uint32_t*>(readback_buf.value().mapped);
  ASSERT_NE(words, nullptr);
  const std::uint32_t kHalf = bits(kClearDepth);
  for (std::uint32_t i = 0U; i < kTiles; ++i) {
    EXPECT_EQ(words[i], kHalf) << "tile " << i << " (exclusive leg)";
    EXPECT_EQ(words[kTiles + i], kHalf) << "tile " << i << " (concurrent leg)";
    EXPECT_EQ(words[i], words[kTiles + i]) << "tile " << i << " A-vs-B";
    if (words[i] != kHalf || words[kTiles + i] != kHalf) break;
  }
  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  reduce_pipeline.cleanup(context.device());
  pyramid_concurrent.cleanup(context.device());
  pyramid_exclusive.cleanup(context.device());
  target.cleanup(context.device());
  auto live = readback_buf.value();
  allocator.destroy_allocation(live);
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
