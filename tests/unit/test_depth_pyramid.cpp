//! @file test_depth_pyramid.cpp
//! @brief Real-depth occlusion: render a scene, copy the ACTUAL depth
//!        attachment to a buffer, reduce it on the GPU to per-tile maxima,
//!        then run the cull shader against that pyramid. A cube hidden
//!        behind a rendered wall must be occlusion-culled by real depth
//!        data (no CPU-side depth knowledge), and a visible cube must
//!        survive.
//!
//! Flow per cycle:
//!   1. Graphics: render wall + occluder into color+depth (D32).
//!   2. Transfer: copy depth image -> linear buffer (D32 -> float words).
//!   3. Compute: depth_reduce.comp packs per-tile maxima into the pyramid.
//!   4. Compute: cull_lod_occlude.comp culls the hidden + visible spheres.
//! The cube BEHIND the wall (greater depth) must be culled; the cube IN
//! FRONT (lesser depth) must be accepted at LOD0.

#include <gtest/gtest.h>

#include <iostream>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#ifdef WARPLOOM_HAS_VULKAN
#include "vulkan_test_readback.hpp"

using omnicpp_test::readback_swapchain_image;
#endif

namespace {

constexpr std::uint32_t kSize = 256U;
constexpr std::uint32_t kTile = 32U;
constexpr std::uint32_t kTilesX = kSize / kTile;
constexpr std::uint32_t kTilesY = kSize / kTile;
constexpr std::uint32_t kInstances = 2U;

constexpr std::uint32_t kWordInstanceCount = 0U;
constexpr std::uint32_t kWordCursor = 1U;
constexpr std::uint32_t kWordTilesX = 2U;
constexpr std::uint32_t kWordTilesY = 3U;
constexpr std::uint32_t kWordStatsAccepted = 4U;
constexpr std::uint32_t kWordStatsFrustum = 5U;
constexpr std::uint32_t kWordStatsOccluded = 6U;
// Pyramid starts at word 12: its byte offset (48) must stay a multiple of the
// 16-byte minStorageBufferOffsetAlignment for the sub-range descriptor write.
constexpr std::uint32_t kWordPyramid = 12U;
constexpr std::uint32_t kSphereOff = kWordPyramid + kTilesX * kTilesY;
constexpr std::uint32_t kLodOff = kSphereOff + 4U * kInstances;
constexpr std::uint32_t kComp0Off = kLodOff + kInstances;
constexpr std::uint32_t kCullWords = kComp0Off + kInstances;

[[maybe_unused]] std::uint32_t bits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4U);
  return u;
}

[[maybe_unused]] float unbits(std::uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4U);
  return f;
}

[[maybe_unused]] void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      float* m16) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m16[0] = f / aspect; m16[1] = 0; m16[2] = 0; m16[3] = 0;
  m16[4] = 0; m16[5] = f; m16[6] = 0; m16[7] = 0;
  m16[8] = 0; m16[9] = 0; m16[10] = (zfar + znear) * zn; m16[11] = -1.0f;
  m16[12] = 0; m16[13] = 0; m16[14] = 2.0f * znear * zfar * zn; m16[15] = 0;
  // Standard GL mapping: ndc(view_d) = (f+n)/(f-n) - 2fn/((f-n)*view_d)
  //                        = 1.002 - 0.2002/view_d for near=0.1, far=100.
}

}  // namespace

TEST(VulkanHardware, RealDepthPyramidOcclusion) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppDepthPyramid", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(),
                                   context.physical_device()).is_ok());

  // --- Depth-capable target. ---
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator).is_ok());
  ASSERT_TRUE(target.create_depth(context.device(), context.physical_device(),
                                  VK_FORMAT_D32_SFLOAT).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  // --- Scene: wall slab at z=-20 covering the center, cube in front at
  // z=-10, cube behind at z=-30. The wall and cubes share the scene shader
  // instances; the cull operates on two spheres matching the cubes. ---
  constexpr std::uint32_t kSceneCount = 3U;
  // Layout: words [0..kSceneCount) = identity compaction list (the vertex
  // shader always resolves through it), then 8 data words per instance.
  constexpr std::uint32_t kInstDataOff = kSceneCount;
  auto inst_buf = allocator.create_buffer(
      (kSceneCount + kSceneCount * 8U) * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(inst_buf.is_ok());
  {
    auto* w = static_cast<std::uint32_t*>(inst_buf.value().mapped);
    for (std::uint32_t i = 0; i < kSceneCount; ++i) { w[i] = i; }
    const float scene[kSceneCount][8] = {
        // pos.xyz, scale, color.rgb, shape (1 = slab). Scale 5 keeps the wall
        // wide enough to cover the back sphere's whole 2x2 tile footprint.
        {0.0f, 0.0f, -20.0f, 5.0f, 0.5f, 0.5f, 0.55f, 1U},   // wall
        {0.0f, 0.0f, -10.0f, 1.0f, 0.2f, 0.9f, 0.3f, 0U},    // front cube
        {0.0f, 0.0f, -30.0f, 1.0f, 0.9f, 0.3f, 0.2f, 0U},    // back cube
    };
    std::memcpy(w + kInstDataOff, scene, sizeof(scene));
  }

  // --- Cull payload + draw command buffers. ---
  auto cull_buf = allocator.create_buffer(
      kCullWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(cull_buf.is_ok());
  auto depth_buf = allocator.create_buffer(
      static_cast<VkDeviceSize>(kSize) * kSize * 4U,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(depth_buf.is_ok());

  auto* words = static_cast<std::uint32_t*>(cull_buf.value().mapped);
  const auto reset_cull = [&]() {
    words[kWordInstanceCount] = kInstances;
    words[kWordCursor] = 0U;
    words[kWordTilesX] = kTilesX;
    words[kWordTilesY] = kTilesY;
    words[kWordStatsAccepted] = 0U;
    words[kWordStatsFrustum] = 0U;
    words[kWordStatsOccluded] = 0U;
    words[kLodOff + 0] = 0U;
    words[kLodOff + 1] = 0U;
    // Spheres match the front/back cubes' positions (radius 0.7).
    const float spheres[kInstances][4] = {
        {0.0f, 0.0f, -10.0f, 0.7f},
        {0.0f, 0.0f, -30.0f, 0.7f},
    };
    for (std::uint32_t i = 0; i < kInstances; ++i) {
      for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) {
        words[kSphereOff + i * 4U + static_cast<std::uint32_t>(k)] =
            bits(spheres[i][k]);
      }
    }
  };

  // --- Pipelines. ---
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  // Reduce layout: src depth words + dst pyramid (both in one descriptor set
  // backed by two buffers).
  std::vector<omnicpp::render::ReflectedBinding> reduce_bindings(2);
  reduce_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  reduce_bindings[1] = {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  auto reduce_layout = manager.create_layout(reduce_bindings, 1);
  ASSERT_TRUE(reduce_layout.is_ok());
  auto reduce_set = manager.allocate_set(reduce_layout.value());
  ASSERT_TRUE(reduce_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(reduce_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   depth_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  // Pyramid dst lives INSIDE cull_buf at word offset kWordPyramid.
  ASSERT_TRUE(manager.write_buffer(reduce_set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   cull_buf.value().buffer,
                                   static_cast<VkDeviceSize>(kWordPyramid) * 4U,
                                   static_cast<VkDeviceSize>(kTilesX * kTilesY) * 4U).is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  const VkPushConstantRange reduce_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16U};
  omnicpp::render::VulkanPipeline reduce_pipe;
  ASSERT_TRUE(reduce_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/depth_reduce.comp.spv", "compute").is_ok());
  ASSERT_TRUE(reduce_pipe.create_pipeline_layout(
      context.device(), &reduce_layout.value(), 1, &reduce_push).is_ok());
  ASSERT_TRUE(reduce_pipe.create_compute_pipeline(
      context.device(), reduce_pipe.pipeline_layout()).is_ok());

  // Cull layout: payload (binding 0). The shader's draw-command bindings 1..3
  // are unused in this test (counts read from stats), but the shader declares
  // them — bind the payload buffer as a stand-in.
  std::vector<omnicpp::render::ReflectedBinding> cull_bindings(4);
  for (std::size_t i = 0; i < static_cast<std::size_t>(4); ++i) {
    cull_bindings[i] = {0, static_cast<std::uint32_t>(i), 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto cull_layout = manager.create_layout(cull_bindings, 1);
  ASSERT_TRUE(cull_layout.is_ok());
  auto cull_set = manager.allocate_set(cull_layout.value());
  ASSERT_TRUE(cull_set.is_ok());
  for (std::uint32_t b = 0; b < 4; ++b) {
    ASSERT_TRUE(manager.write_buffer(cull_set.value(), b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     cull_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  }

  const VkPushConstantRange cull_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 56U};
  omnicpp::render::VulkanPipeline cull_pipe;
  ASSERT_TRUE(cull_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/cull_lod_occlude.comp.spv", "compute").is_ok());
  ASSERT_TRUE(cull_pipe.create_pipeline_layout(
      context.device(), &cull_layout.value(), 1, &cull_push).is_ok());
  ASSERT_TRUE(cull_pipe.create_compute_pipeline(
      context.device(), cull_pipe.pipeline_layout()).is_ok());

  // Scene raster pipeline (reuse cube.vert/frag: per-instance data pulling).
  std::vector<omnicpp::render::ReflectedBinding> scene_bindings(1);
  scene_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT};
  auto scene_layout = manager.create_layout(scene_bindings, 1);
  ASSERT_TRUE(scene_layout.is_ok());
  auto scene_set = manager.allocate_set(scene_layout.value());
  ASSERT_TRUE(scene_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(scene_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   inst_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  constexpr std::uint32_t kScenePush = 64U + 4U * sizeof(std::uint32_t);
  const VkPushConstantRange scene_push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, kScenePush};
  omnicpp::render::VulkanPipeline scene_pipe;
  ASSERT_TRUE(scene_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(scene_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(scene_pipe.create_pipeline_layout(
      context.device(), &scene_layout.value(), 1, &scene_push_range).is_ok());
  ASSERT_TRUE(scene_pipe.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      scene_pipe.pipeline_layout(), true, true, true).is_ok());

  // --- Command infrastructure. ---
  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer cb = cb_result.value();
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);

  float view_proj[16];
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, view_proj);

  // gpu_objects.vert push: view_proj, data_offset, comp_offset, lod_scale.
  // comp_offset=0 indexes the identity list; data starts at kInstDataOff.
  struct ScenePush {
    float view_proj[16];
    std::uint32_t data_offset;
    std::uint32_t comp_offset;
    std::uint32_t lod_scale;
    std::uint32_t pad0;
  } scene_push{};
  std::memcpy(scene_push.view_proj, view_proj, 64U);
  scene_push.data_offset = kInstDataOff;
  scene_push.comp_offset = 0U;
  scene_push.lod_scale = bits(1.0f);

  // ---- Cycle 1: render scene, copy depth, reduce, cull. ----
  reset_cull();
  ASSERT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);

  // Host writes (cull payload instance count etc.) -> compute visible.
  {
    VkBufferMemoryBarrier host_vis{};
    host_vis.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host_vis.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_vis.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    host_vis.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_vis.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_vis.buffer = cull_buf.value().buffer;
    host_vis.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &host_vis, 0, nullptr);
  }

  // (1) Raster: wall + cubes into color+depth.
  VkRenderPassBeginInfo rb{};
  rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rb.renderPass = target.render_pass();
  rb.framebuffer = target.framebuffer();
  rb.renderArea.extent = {kSize, kSize};
  VkClearValue clears[2]{};
  clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clears[1].depthStencil = {1.0f, 0};
  rb.clearValueCount = 2;
  rb.pClearValues = clears;
  vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
  vkCmdSetViewport(cb, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {kSize, kSize}};
  vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipe.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          scene_pipe.pipeline_layout(), 0, 1, &scene_set.value(), 0, nullptr);
  vkCmdPushConstants(cb, scene_pipe.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT,
                     0, kScenePush, &scene_push);
  vkCmdDraw(cb, 36U, kSceneCount, 0, 0);
  vkCmdEndRenderPass(cb);

  // Depth attachment (DEPTH_ATTACHMENT_OPTIMAL after pass) -> TRANSFER_SRC.
  {
    VkImageMemoryBarrier depth_to_transfer{};
    depth_to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depth_to_transfer.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    depth_to_transfer.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    depth_to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_to_transfer.image = target.depth_image();
    depth_to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_to_transfer.subresourceRange.levelCount = 1;
    depth_to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &depth_to_transfer);
  }

  // (2) Copy depth -> buffer (D32: one float per texel).
  {
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {kSize, kSize, 1};
    vkCmdCopyImageToBuffer(cb, target.depth_image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           depth_buf.value().buffer, 1, &copy);
  }
  // Depth restore to attachment layout for later renders.
  {
    VkImageMemoryBarrier depth_restore{};
    depth_restore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depth_restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    depth_restore.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    depth_restore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_restore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_restore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_restore.image = target.depth_image();
    depth_restore.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_restore.subresourceRange.levelCount = 1;
    depth_restore.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &depth_restore);
  }

  // Depth copy -> reduce visible (transfer write -> shader read).
  {
    VkBufferMemoryBarrier depth_ready{};
    depth_ready.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    depth_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    depth_ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depth_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_ready.buffer = depth_buf.value().buffer;
    depth_ready.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &depth_ready, 0, nullptr);
  }

  // (3) Reduce: one invocation per tile.
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, reduce_pipe.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          reduce_pipe.pipeline_layout(), 0, 1, &reduce_set.value(), 0, nullptr);
  const std::uint32_t reduce_pc[4] = {kSize, kSize, kTilesX, kTile};
  vkCmdPushConstants(cb, reduce_pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(reduce_pc), reduce_pc);
  vkCmdDispatch(cb, kTilesX / 8U, kTilesY / 8U, 1);

  // Reduce writes -> cull reads.
  {
    VkBufferMemoryBarrier pyramid_ready{};
    pyramid_ready.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pyramid_ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pyramid_ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pyramid_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pyramid_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pyramid_ready.buffer = cull_buf.value().buffer;
    pyramid_ready.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &pyramid_ready, 0, nullptr);
  }

  // (4) Cull against the real-depth pyramid.
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipe.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          cull_pipe.pipeline_layout(), 0, 1, &cull_set.value(), 0, nullptr);
  const std::uint32_t cull_pc[14] = {kSphereOff, kLodOff, kComp0Off, 0U, 0U,
                                     0U, 0U, kSize, kSize, kTile,
                                     bits(0.1f), bits(100.0f), kWordPyramid, 0U};
  vkCmdPushConstants(cb, cull_pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(cull_pc), cull_pc);
  vkCmdDispatch(cb, (kInstances + 63U) / 64U, 1, 1);

  // Cull writes -> host read.
  {
    VkBufferMemoryBarrier cull_done{};
    cull_done.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    cull_done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cull_done.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    cull_done.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cull_done.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cull_done.buffer = cull_buf.value().buffer;
    cull_done.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &cull_done, 0, nullptr);
  }

  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  ASSERT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

  // ---- Verify: pyramid tiles covering the wall hold wall depth. ----
  // Tile (3,3) lies inside the wall's footprint; its max is the wall's FAR
  // face (scale 5 -> spans z in [-25,-15], view d = 25):
  // 1.002 - 0.2002/25 ~= 0.99400.
  const float wall_depth = 1.002f - 0.2002f / 25.0f;  // ~0.99400
  float tile_max = unbits(words[kWordPyramid + 3U * kTilesX + 3U]);
  EXPECT_NEAR(tile_max, wall_depth, 0.001f);
  // Corner tile (sky) stays at clear depth 1.0.
  tile_max = unbits(words[kWordPyramid + 0U]);
  EXPECT_NEAR(tile_max, 1.0f, 1e-6f);

  // ---- Cull result: front cube survives, back cube occluded. ----
  // Exact NDC depths (1.002 - 0.2002/d): front sphere nearest point d=9.3
  // -> 0.98047; back sphere nearest point d=29.3 -> 0.99517. Every tile in
  // the back sphere's footprint holds wall depth <= 0.99400 (far face) or
  // 0.98867 (near face) -> all nearer than 0.99517 -> occluded. Front
  // sphere: 0.98047 < every covering tile max -> visible.
  EXPECT_EQ(words[kWordStatsAccepted], 1U);
  EXPECT_EQ(words[kWordStatsOccluded], 1U);
  EXPECT_EQ(words[kWordStatsFrustum], 0U);

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  scene_pipe.cleanup(context.device());
  cull_pipe.cleanup(context.device());
  reduce_pipe.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation b1 = inst_buf.value();
  omnicpp::render::Allocation b2 = cull_buf.value();
  omnicpp::render::Allocation b3 = depth_buf.value();
  allocator.destroy_allocation(b1);
  allocator.destroy_allocation(b2);
  allocator.destroy_allocation(b3);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
