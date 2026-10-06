//! @file test_gpu_lod_occlusion.cpp
//! @brief GPU-driven scale-up: combined frustum + occlusion culling,
//!        distance-band LOD selection with per-instance bias, per-band
//!        indirect commands, and per-frame atomic statistics readback.
//!
//! Scenario A (occlusion): depth-pyramid tiles fully covering a cube's
//!   projection and nearer than it reject the instance — stats report it,
//!   the draw commands stay empty, the frame reads back empty.
//! Scenario B (LOD bands): instances at near/mid/far distances land in the
//!   correct per-band compacted lists and per-band indirect commands; a
//!   per-instance bias forces a coarser band; moving an instance across a
//!   band boundary (animation) moves it between lists.

#include <gtest/gtest.h>

#include <iostream>

#include <cmath>
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
constexpr std::uint32_t kTilesX = kSize / kTile;  // 8
constexpr std::uint32_t kTilesY = kSize / kTile;  // 8
constexpr std::uint32_t kInstances = 8U;

// Word layout of binding 0 (mirrors cull_lod_occlude.comp).
constexpr std::uint32_t kWordInstanceCount = 0U;
constexpr std::uint32_t kWordCursor = 1U;
constexpr std::uint32_t kWordTilesX = 2U;
constexpr std::uint32_t kWordTilesY = 3U;
constexpr std::uint32_t kWordStatsAccepted = 4U;
constexpr std::uint32_t kWordStatsFrustum = 5U;
constexpr std::uint32_t kWordStatsOccluded = 6U;
constexpr std::uint32_t kWordPyramid = 10U;
constexpr std::uint32_t kSphereOff = kWordPyramid + kTilesX * kTilesY;  // 74
constexpr std::uint32_t kLodOff = kSphereOff + 4U * kInstances;
constexpr std::uint32_t kComp0Off = kLodOff + kInstances;
constexpr std::uint32_t kComp1Off = kComp0Off + kInstances;
constexpr std::uint32_t kComp2Off = kComp1Off + kInstances;
constexpr std::uint32_t kCullWords = kComp2Off + kInstances;

struct DrawCmd {
  std::uint32_t vertex_count;
  std::uint32_t instance_count;
  std::uint32_t first_vertex;
  std::uint32_t first_instance;
};

std::uint32_t bits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4U);
  return u;
}


void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      float* m16) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m16[0] = f / aspect; m16[1] = 0; m16[2] = 0; m16[3] = 0;
  m16[4] = 0; m16[5] = f; m16[6] = 0; m16[7] = 0;
  m16[8] = 0; m16[9] = 0; m16[10] = zfar * zn; m16[11] = -1.0f;
  m16[12] = 0; m16[13] = 0; m16[14] = znear * zfar * zn; m16[15] = 0;
}

}  // namespace

TEST(VulkanHardware, GpuLodOcclusionCounters) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppGpuLodTest", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(),
                                   context.physical_device()).is_ok());

  // --- Binding 0: cull payload. ---
  auto cull_buf = allocator.create_buffer(
      kCullWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(cull_buf.is_ok());
  // --- Per-instance data (8 words each) consumed by the vertex shader. ---
  auto inst_buf = allocator.create_buffer(
      (kInstances + kInstances * 8U) * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(inst_buf.is_ok());
  // --- Per-band indirect commands. ---
  auto draw_bufs = allocator.create_buffer(
      3U * sizeof(DrawCmd),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(draw_bufs.is_ok());

  auto* words = static_cast<std::uint32_t*>(cull_buf.value().mapped);
  auto* cmd_words = static_cast<std::uint32_t*>(draw_bufs.value().mapped);

  // Instance data: centers on a ring in front of the camera, distinct colors.
  const float z_of[8] = {-6.0f, -8.0f, -12.0f, -25.0f, -30.0f, -45.0f, -60.0f, -80.0f};
  {
    auto* iw = static_cast<std::uint32_t*>(inst_buf.value().mapped);
    for (std::uint32_t i = 0; i < kInstances; ++i) {
      const std::uint32_t base = kInstances + i * 8U;
      const float xform[4] = {0.0f, 0.0f, z_of[i], 0.7f};
      const float tint[4] = {0.2f + 0.1f * static_cast<float>(i), 0.4f, 0.9f, 0.0f};
      std::uint32_t xb[4], tb[4];
      std::memcpy(xb, xform, 16U);
      std::memcpy(tb, tint, 16U);
      for (std::uint32_t k = 0; k < 4; ++k) {
        iw[base + k] = xb[k];
        iw[base + 4U + k] = tb[k];
      }
    }
  }

  // --- Descriptors: cull set (payload + 3 draw commands), gfx set (instances). ---
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> cull_bindings(4);
  for (auto& b : cull_bindings) {
    b = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  }
  cull_bindings[0].binding = 0;
  cull_bindings[1].binding = 1;
  cull_bindings[2].binding = 2;
  cull_bindings[3].binding = 3;
  auto cull_layout = manager.create_layout(cull_bindings, 1);
  ASSERT_TRUE(cull_layout.is_ok());
  auto cull_set = manager.allocate_set(cull_layout.value());
  ASSERT_TRUE(cull_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(cull_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   cull_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  ASSERT_TRUE(manager.write_buffer(cull_set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   draw_bufs.value().buffer, 0, sizeof(DrawCmd)).is_ok());
  ASSERT_TRUE(manager.write_buffer(cull_set.value(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   draw_bufs.value().buffer, sizeof(DrawCmd),
                                   sizeof(DrawCmd)).is_ok());
  ASSERT_TRUE(manager.write_buffer(cull_set.value(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   draw_bufs.value().buffer, 2U * sizeof(DrawCmd),
                                   sizeof(DrawCmd)).is_ok());

  std::vector<omnicpp::render::ReflectedBinding> gfx_bindings(1);
  gfx_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT};
  auto gfx_layout = manager.create_layout(gfx_bindings, 1);
  ASSERT_TRUE(gfx_layout.is_ok());
  auto gfx_set = manager.allocate_set(gfx_layout.value());
  ASSERT_TRUE(gfx_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(gfx_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   inst_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  // --- Pipelines. ---
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  const VkPushConstantRange comp_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 56U};
  omnicpp::render::VulkanPipeline comp_pipe;
  ASSERT_TRUE(comp_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/cull_lod_occlude.comp.spv", "compute").is_ok());
  ASSERT_TRUE(comp_pipe.create_pipeline_layout(
      context.device(), &cull_layout.value(), 1, &comp_push).is_ok());
  ASSERT_TRUE(comp_pipe.create_compute_pipeline(
      context.device(), comp_pipe.pipeline_layout()).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  constexpr std::uint32_t kGfxPushBytes = 64U + 4U * sizeof(std::uint32_t);
  const VkPushConstantRange gfx_push{VK_SHADER_STAGE_VERTEX_BIT, 0, kGfxPushBytes};
  omnicpp::render::VulkanPipeline gfx_pipe;
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(gfx_pipe.create_pipeline_layout(
      context.device(), &gfx_layout.value(), 1, &gfx_push).is_ok());
  ASSERT_TRUE(gfx_pipe.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      gfx_pipe.pipeline_layout(), false, false, false).is_ok());

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

  //! CPU-side reset of all GPU-owned state before a frame.
  const auto reset_frame_state = [&](std::uint32_t count) {
    words[kWordInstanceCount] = count;
    words[kWordCursor] = 0U;
    words[kWordTilesX] = kTilesX;
    words[kWordTilesY] = kTilesY;
    words[kWordStatsAccepted] = 0U;
    words[kWordStatsFrustum] = 0U;
    words[kWordStatsOccluded] = 0U;
    for (std::uint32_t i = 0; i < kInstances; ++i) {
      words[kLodOff + i] = 0U;  // no LOD bias by default
    }
    for (std::uint32_t b = 0; b < 3; ++b) {
      auto* c = reinterpret_cast<DrawCmd*>(cmd_words + b * 4U);
      c->vertex_count = 36U;
      c->instance_count = 0U;
      c->first_vertex = 0U;
      c->first_instance = 0U;
    }
  };

  //! Records + submits one cull-only frame; returns readback (for layout).
  const auto run_cull = [&]() {
    ASSERT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);

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

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            comp_pipe.pipeline_layout(), 0, 1, &cull_set.value(), 0, nullptr);
    const std::uint32_t push[14] = {kSphereOff, kLodOff, kComp0Off, kComp1Off,
                                    kComp2Off, 0U, 0U, kSize, kSize, kTile,
                                    bits(0.1f), bits(100.0f), 10U, 0U};
    vkCmdPushConstants(cb, comp_pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), push);
    vkCmdDispatch(cb, (kInstances + 63U) / 64U, 1, 1);

    // Cull writes -> host read visibility.
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

    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    ASSERT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
  };

  //! Writes spheres + pyramid for the current scenario, runs cull, returns
  //! per-band instance counts (from the indirect commands) and stats.
  struct CullResult {
    std::uint32_t band[3];
    std::uint32_t accepted;
    std::uint32_t frustum_culled;
    std::uint32_t occluded;
  };
  const auto read_result = [&]() {
    CullResult r{};
    for (std::size_t b = 0; b < static_cast<std::size_t>(3); ++b) {
      auto* c = reinterpret_cast<const DrawCmd*>(cmd_words + b * 4U);
      r.band[b] = c->instance_count;
    }
    r.accepted = words[kWordStatsAccepted];
    r.frustum_culled = words[kWordStatsFrustum];
    r.occluded = words[kWordStatsOccluded];
    return r;
  };

  // =====================================================================
  // Scenario A: occlusion. Wall at z=-20 covers tiles rows 2..5 fully with
  // depth ~0.8; cubes at z=-25..-80 (proxy depth > 0.8) behind it must be
  // occluded; cubes nearer than the wall survive.
  // =====================================================================
  {
    // Spheres: 3 in front of the wall (z=-6,-8,-12), 5 behind (z=-25..-80).
    for (std::uint32_t i = 0; i < kInstances; ++i) {
      const float sphere[4] = {0.0f, 0.0f, z_of[i], 0.7f};
      words[kSphereOff + i * 4U + 0] = bits(sphere[0]);
      words[kSphereOff + i * 4U + 1] = bits(sphere[1]);
      words[kSphereOff + i * 4U + 2] = bits(sphere[2]);
      words[kSphereOff + i * 4U + 3] = bits(sphere[3]);
    }
    // Pyramid: all tiles at clear depth 1.0 except the central wall block
    // (rows/cols 2..5) holding the wall's depth. The wall sits at z=-20;
    // the cull shader compares against exact GL-projection NDC depth
    // (1.002 - 0.2002/d), so the tile holds 1.002 - 0.2002/20 ~= 0.992
    // (nearer = smaller). Front spheres (|z| < 20) have nearest-point depth
    // < 0.992 -> visible; back spheres -> occluded.
    const float wall_depth = 1.002f - 0.2002f / 20.0f;
    for (std::uint32_t t = 0; t < kTilesX * kTilesY; ++t) {
      words[kWordPyramid + t] = bits(1.0f);
    }
    for (std::uint32_t ty = 2U; ty <= 5U; ++ty) {
      for (std::uint32_t tx = 2U; tx <= 5U; ++tx) {
        words[kWordPyramid + ty * kTilesX + tx] = bits(wall_depth);
      }
    }
    reset_frame_state(kInstances);
    run_cull();
    const auto r = read_result();
    std::cout << "[lod] A: band0=" << r.band[0] << " frustum=" << r.frustum_culled
              << " occluded=" << r.occluded << " accepted=" << r.accepted << "\n";
    // 3 near cubes (z=-6,-8,-12) accepted at LOD0; 5 behind the wall occluded.
    EXPECT_EQ(r.band[0], 3U);
    EXPECT_EQ(r.occluded, 5U);
    EXPECT_EQ(r.accepted, 3U);
    EXPECT_EQ(r.frustum_culled, 0U);
  }

  // =====================================================================
  // Scenario B: LOD bands without occlusion (pyramid all far). Instances at
  // z=-6,-8,-12 (dist<18) -> band0; -25,-30,-45 (18<dist<40) -> band1;
  // -60,-80 -> band2. A bias of 1 on instance 0 (z=-6) forces it to band1;
  // moving instance 3 (z=-25) to z=-10 animates it into band0.
  // =====================================================================
  {
    for (std::uint32_t i = 0; i < kInstances; ++i) {
      words[kSphereOff + i * 4U + 0] = bits(0.0f);
      words[kSphereOff + i * 4U + 1] = bits(0.0f);
      words[kSphereOff + i * 4U + 2] = bits(z_of[i]);
      words[kSphereOff + i * 4U + 3] = bits(0.7f);
    }
    for (std::uint32_t t = 0; t < kTilesX * kTilesY; ++t) {
      words[kWordPyramid + t] = bits(1.0f);  // nothing occludes
    }
    reset_frame_state(kInstances);
    words[kLodOff + 0] = 1U;  // bias instance 0 into band >= 1
    run_cull();
    auto r = read_result();
    std::cout << "[lod] B1: band0=" << r.band[0] << " band1=" << r.band[1]
              << " band2=" << r.band[2] << "\n";
    // Distances: 6,8,12 -> band0 (3); 25,30 -> band1 (2); 45,60,80 -> band2
    // (3). Bias on instance 0 moves it from band0 to band1: 2/3/3.
    EXPECT_EQ(r.band[0], 2U);
    EXPECT_EQ(r.band[1], 3U);
    EXPECT_EQ(r.band[2], 3U);
    EXPECT_EQ(r.accepted, 8U);
    EXPECT_EQ(r.occluded, 0U);

    // Animation: instance 3 (z=-25, band1) moves to z=-10 -> band0.
    words[kSphereOff + 3U * 4U + 2] = bits(-10.0f);
    reset_frame_state(kInstances);
    words[kLodOff + 0] = 1U;
    run_cull();
    r = read_result();
    std::cout << "[lod] B2: band0=" << r.band[0] << " band1=" << r.band[1]
              << " band2=" << r.band[2] << "\n";
    // band0: -8,-10,-12 (i0 biased out) = 3; band1: biased -6, -30 = 2;
    // band2: -45,-60,-80 = 3.
    EXPECT_EQ(r.band[0], 3U);
    EXPECT_EQ(r.band[1], 2U);
    EXPECT_EQ(r.band[2], 3U);
  }

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  gfx_pipe.cleanup(context.device());
  comp_pipe.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation b1 = cull_buf.value();
  omnicpp::render::Allocation b2 = inst_buf.value();
  omnicpp::render::Allocation b3 = draw_bufs.value();
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
