//! @file test_gpu_driven.cpp
//! @brief GPU-driven rendering: compute-side frustum culling compacts
//!        accepted instances and maintains the indirect draw command;
//!        graphics executes it — the CPU never touches per-instance draw
//!        data. Verified by readback: an all-visible frame is populated, a
//!        fully-culled frame is empty.

#include <gtest/gtest.h>

#include <iostream>

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "warploom/core/latency_telemetry.hpp"
#include "warploom/render/vulkan_compute.hpp"
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

struct DrawCmd {
  std::uint32_t vertex_count;
  std::uint32_t instance_count;
  std::uint32_t first_vertex;
  std::uint32_t first_instance;
};

constexpr std::uint32_t kSphereOffset = 26U;  // word index of spheres in binding 0

//! Perspective projection, Vulkan clip space (y down, z into [0,1]).
void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      float* m16) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m16[0] = f / aspect; m16[1] = 0; m16[2] = 0; m16[3] = 0;
  m16[4] = 0; m16[5] = f; m16[6] = 0; m16[7] = 0;
  m16[8] = 0; m16[9] = 0; m16[10] = zfar * zn; m16[11] = -1.0f;
  m16[12] = 0; m16[13] = 0; m16[14] = znear * zfar * zn; m16[15] = 0;
}

//! 6 frustum planes for a camera at the origin looking down -z.
//! Packing: (nx, ny, nz, d) with inside = dot(n, p) + d >= 0.
//! For view-space depth w = -z > 0:
//!   near  inside: z <= -znear  -> plane (0,0,-1), d = -znear   (dist = -z-znear)
//!   far   inside: z >= -zfar   -> plane (0,0, 1), d = +zfar    (dist = z+zfar)
//!   left  inside: x >= z*(r/znear)   (z<0) -> plane (znear, 0, -r)
//!   right inside: x <= -z*(r/znear)  (z<0) -> plane (-znear, 0, -r)
//!   top   inside: y >= z*(t/znear)   (z<0) -> plane (0, znear, -t)
//!   bottom inside: y <= -z*(t/znear) (z<0) -> plane (0, -znear, -t)
void make_frustum_planes(float fov_y, float aspect, float znear, float zfar,
                         float* p24) {
  const float t = znear * std::tan(fov_y * 0.5f);  // near half-height
  const float r = t * aspect;                       // near half-width
  const auto plane = [](float x, float y, float z, float w, float* out) {
    const float len = std::sqrt(x * x + y * y + z * z);
    out[0] = x / len; out[1] = y / len; out[2] = z / len; out[3] = w / len;
  };
  plane(0, 0, -1, -znear, p24 + 0);
  plane(0, 0, 1, zfar, p24 + 4);
  plane(znear, 0, -r, 0, p24 + 8);
  plane(-znear, 0, -r, 0, p24 + 12);
  plane(0, znear, -t, 0, p24 + 16);
  plane(0, -znear, -t, 0, p24 + 20);
}

std::vector<std::uint8_t> load_spv(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

}  // namespace

TEST(VulkanHardware, GpuDrivenCullIndirectDraw) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppGpuDrivenTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  constexpr float kFovY = 1.05f;
  constexpr float kAspect = 1.0f;
  constexpr float kNear = 0.1f;
  constexpr float kFar = 100.0f;
  constexpr std::uint32_t kInstanceCount = 4;
  constexpr std::uint32_t kCompactOffset = kSphereOffset + 4U * kInstanceCount;

  // 3 cubes inside the frustum, 1 behind the camera. The compute pass must
  // accept exactly the first three.
  const float positions[4][3] = {
      {-2.0f, 0, -8.0f}, {2.0f, 0, -12.0f}, {0, 1.5f, -20.0f}, {0, 0, 10.0f}};
  const float colors[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}};

  // --- Buffers. ---
  // Binding 0 words: [0..1] header, [2..25] frustum, spheres, compacted.
  constexpr VkDeviceSize kCullWords =
      static_cast<VkDeviceSize>(kCompactOffset + kInstanceCount);
  auto cull_buf = allocator.create_buffer(
      kCullWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(cull_buf.is_ok());

  auto inst_buf = allocator.create_buffer(
      (kInstanceCount + kInstanceCount * 8U) * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(inst_buf.is_ok());
  {
    // Layout: [0..N-1] compacted indices (compute output, zeros now), then
    // 8 words of data per instance.
    std::vector<std::uint32_t> inst(kInstanceCount * 9U, 0U);
    std::uint32_t* words = static_cast<std::uint32_t*>(inst_buf.value().mapped);
    for (std::uint32_t i = 0; i < kInstanceCount; ++i) {
      const std::uint32_t base = kInstanceCount + i * 8U;
      float pf[4] = {positions[i][0], positions[i][1], positions[i][2], 0.5f};
      float cf[4] = {colors[i][0], colors[i][1], colors[i][2], 0.0f};
      std::uint32_t pw[4], cw[4];
      std::memcpy(pw, pf, 16U);
      std::memcpy(cw, cf, 16U);
      for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) {
        words[base + k] = pw[k];
        words[base + 4U + k] = cw[k];
      }
    }
  }

  auto draw_buf = allocator.create_buffer(
      sizeof(DrawCmd),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(draw_buf.is_ok());

  // CPU fills static input once (instance data + frustum); the GPU owns the
  // dynamic accepted-count and draw-command state from here on.
  {
    auto* words = static_cast<std::uint32_t*>(cull_buf.value().mapped);
    words[0] = kInstanceCount;
    words[1] = 0;
    float planes[24];
    make_frustum_planes(kFovY, kAspect, kNear, kFar, planes);
    std::uint32_t plane_words[24];
    std::memcpy(plane_words, planes, sizeof(plane_words));
    for (std::size_t i = 0; i < static_cast<std::size_t>(24); ++i) words[2 + i] = plane_words[i];
    for (std::uint32_t i = 0; i < kInstanceCount; ++i) {
      const float sphere[4] = {positions[i][0], positions[i][1], positions[i][2], 0.9f};
      std::uint32_t sw[4];
      std::memcpy(sw, sphere, sizeof(sw));
      for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) words[kSphereOffset + i * 4U + k] = sw[k];
    }
  }
  {
    DrawCmd cmd{36U, 0U, 0U, 0U};
    std::memcpy(draw_buf.value().mapped, &cmd, sizeof(cmd));
  }

  // --- Descriptor sets. ---
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  std::vector<omnicpp::render::ReflectedBinding> comp_bindings(2);
  comp_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  comp_bindings[1] = {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  auto comp_layout = manager.create_layout(comp_bindings, 1);
  ASSERT_TRUE(comp_layout.is_ok());
  auto comp_set = manager.allocate_set(comp_layout.value());
  ASSERT_TRUE(comp_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(comp_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   cull_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  ASSERT_TRUE(manager.write_buffer(comp_set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   draw_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

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
  const VkPushConstantRange comp_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
  omnicpp::render::VulkanPipeline comp_pipe;
  ASSERT_TRUE(comp_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/cull_and_draw.comp.spv", "compute").is_ok());
  ASSERT_TRUE(comp_pipe.create_pipeline_layout(
      context.device(), &comp_layout.value(), 1, &comp_push).is_ok());
  ASSERT_TRUE(comp_pipe.create_compute_pipeline(
      context.device(), comp_pipe.pipeline_layout()).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &allocator).is_ok());
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

  // Returns the instance count read back from the draw command (0 on any
  // failed assertion inside; the EXPECT_* failures still fail the test).
  // The CPU resets the GPU-owned counters after every submission, so a
  // host-write -> device-read visibility barrier is recorded every frame
  // (covering the cull payload AND the indirect command buffer).
  auto submit_frame = [&]() -> std::uint32_t {
    EXPECT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) return 0;

    {
      // Mapped-memory CPU writes (counter resets + sphere updates) must be
      // made visible to the GPU before shaders/indirect-fetch read them.
      VkBufferMemoryBarrier host_vis[2] = {};
      host_vis[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      host_vis[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      host_vis[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      host_vis[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      host_vis[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      host_vis[0].buffer = cull_buf.value().buffer;
      host_vis[0].size = VK_WHOLE_SIZE;
      host_vis[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      host_vis[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      host_vis[1].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
      host_vis[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      host_vis[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      host_vis[1].buffer = draw_buf.value().buffer;
      host_vis[1].size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                           0, 0, nullptr, 2, host_vis, 0, nullptr);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe.pipeline());
    const VkDescriptorSet cds = comp_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe.pipeline_layout(),
                            0, 1, &cds, 0, nullptr);
    const std::uint32_t push[2] = {kSphereOffset, kCompactOffset};
    vkCmdPushConstants(cb, comp_pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), push);
    vkCmdDispatch(cb, (kInstanceCount + 63U) / 64U, 1, 1);

    // cull writes -> indirect read barrier (counter + command buffer).
    VkBufferMemoryBarrier barriers[2] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].buffer = draw_buf.value().buffer;
    barriers[0].size = VK_WHOLE_SIZE;
    barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].buffer = cull_buf.value().buffer;
    barriers[1].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 0, nullptr, 2, barriers, 0, nullptr);

    // Graphics: indirect draw of the compute-compacted instances.
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {256, 256};
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, 256, 256, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {256, 256}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline());
    const VkDescriptorSet gds = gfx_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline_layout(),
                            0, 1, &gds, 0, nullptr);
    struct {
      float view_proj[16];
      std::uint32_t data_offset;
      std::uint32_t comp_offset;
      std::uint32_t lod_scale;
      std::uint32_t pad0;
    } gfx_push_data{};
    make_perspective(kFovY, kAspect, kNear, kFar, gfx_push_data.view_proj);
    gfx_push_data.data_offset = kInstanceCount;  // compacted list precedes data
    gfx_push_data.comp_offset = 0U;              // list starts at word 0
    {
      const float one = 1.0f;
      std::memcpy(&gfx_push_data.lod_scale, &one, 4U);
    }
    vkCmdPushConstants(cb, gfx_pipe.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT,
                       0, kGfxPushBytes, &gfx_push_data);
    vkCmdDrawIndirect(cb, draw_buf.value().buffer, 0, 1, sizeof(DrawCmd));
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return 0;

    if (vkResetFences(context.device(), 1, &fence) != VK_SUCCESS) return 0;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(context.graphics_queue(), 1, &si, fence) != VK_SUCCESS) return 0;
    if (vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return 0;

    // Reset GPU-owned counters for the next frame. Both buffers are
    // HOST_VISIBLE|HOST_COHERENT; the next submission's host-vis barrier
    // publishes these resets (and any sphere updates) to the device.
    auto* words = static_cast<std::uint32_t*>(cull_buf.value().mapped);
    words[1] = 0;
    auto* cmd = static_cast<DrawCmd*>(draw_buf.value().mapped);
    const std::uint32_t count = cmd->instance_count;
    cmd->instance_count = 0;
    return count;
  };

  // --- Frame 1: 3 accepted (4th is behind the camera). ---
  const std::uint32_t accepted = submit_frame();
  EXPECT_EQ(accepted, 3U);

  const auto readback = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), 256, 256,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback.submitted);
  EXPECT_GT(readback.non_clear_pixels, 500U);

  // --- Frame 2: move every sphere behind the camera (CPU updates the static
  // input; culling itself stays GPU-driven) — expect 0 accepted, empty frame. ---
  {
    auto* words = static_cast<std::uint32_t*>(cull_buf.value().mapped);
    for (std::uint32_t i = 0; i < kInstanceCount; ++i) {
      const float sphere[4] = {0.0f, 0.0f, 10.0f, 0.9f};
      std::uint32_t sw[4];
      std::memcpy(sw, sphere, sizeof(sw));
      for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) words[kSphereOffset + i * 4U + k] = sw[k];
    }
  }
  const std::uint32_t accepted2 = submit_frame();
  EXPECT_EQ(accepted2, 0U);

  const auto readback2 = readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family),
      target.image(), target.format(), 256, 256,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ASSERT_TRUE(readback2.submitted);
  EXPECT_EQ(readback2.non_clear_pixels, 0U);

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  gfx_pipe.cleanup(context.device());
  comp_pipe.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation b1 = cull_buf.value();
  omnicpp::render::Allocation b2 = inst_buf.value();
  omnicpp::render::Allocation b3 = draw_buf.value();
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

//! Sustained GPU-driven frame loop: 300 frames of compute culling +
//! indirect draw with animated spheres (oscillating in/out of the frustum
//! so both accept and reject paths run). Frame CPU time from record to
//! fence-signaled is captured per frame and summarized with windowed
//! percentiles (p50/p90/p99/p99.9/max) from LatencyTracker.
TEST(VulkanHardware, SustainedGpuDrivenFrameBenchmark) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppGpuDrivenBench", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;

  omnicpp::render::VulkanPipeline comp_pipe;
  ASSERT_TRUE(comp_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/cull_and_draw.comp.spv", "compute").is_ok());
  omnicpp::render::VulkanPipeline gfx_pipe;
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.frag.spv", "fragment").is_ok());

  constexpr std::uint32_t kInstanceCount = 64;
  constexpr std::uint32_t kSphereOffset = 26U;
  constexpr std::uint32_t kCompactOffset = kSphereOffset + 4U * kInstanceCount;
  constexpr std::uint32_t kFrames = 300;
  constexpr float kFovY = 1.05f, kAspect = 1.0f, kNear = 0.1f, kFar = 100.0f;

  auto cull_buf = allocator.create_buffer(
      (kCompactOffset + kInstanceCount) * 4U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(cull_buf.is_ok());
  auto inst_buf = allocator.create_buffer(
      (kInstanceCount + kInstanceCount * 8U) * 4U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(inst_buf.is_ok());
  auto draw_buf = allocator.create_buffer(
      sizeof(DrawCmd),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(draw_buf.is_ok());

  // Static per-instance colors (positions/spheres are animated per frame).
  {
    auto* words = static_cast<std::uint32_t*>(inst_buf.value().mapped);
    for (std::uint32_t i = 0; i < kInstanceCount; ++i) {
      const std::uint32_t base = kInstanceCount + i * 8U;
      const float r = static_cast<float>(i) / static_cast<float>(kInstanceCount);
      float cf[4] = {0.15f + 0.7f * r, 0.3f, 1.0f - 0.7f * r, 0.0f};
      std::uint32_t cw[4];
      std::memcpy(cw, cf, 16U);
      for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) words[base + 4U + k] = cw[k];
    }
  }

  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> comp_bindings(2);
  comp_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  comp_bindings[1] = {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT};
  auto comp_layout = manager.create_layout(comp_bindings, 1);
  ASSERT_TRUE(comp_layout.is_ok());
  auto comp_set = manager.allocate_set(comp_layout.value());
  ASSERT_TRUE(comp_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(comp_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   cull_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  ASSERT_TRUE(manager.write_buffer(comp_set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   draw_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> gfx_bindings(1);
  gfx_bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT};
  auto gfx_layout = manager.create_layout(gfx_bindings, 1);
  ASSERT_TRUE(gfx_layout.is_ok());
  auto gfx_set = manager.allocate_set(gfx_layout.value());
  ASSERT_TRUE(gfx_set.is_ok());
  ASSERT_TRUE(manager.write_buffer(gfx_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   inst_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  const VkPushConstantRange comp_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
  ASSERT_TRUE(comp_pipe.create_pipeline_layout(
      context.device(), &comp_layout.value(), 1, &comp_push).is_ok());
  ASSERT_TRUE(comp_pipe.create_compute_pipeline(
      context.device(), comp_pipe.pipeline_layout()).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  constexpr std::uint32_t kGfxPushBytes = 64U + 4U * sizeof(std::uint32_t);
  const VkPushConstantRange gfx_push{VK_SHADER_STAGE_VERTEX_BIT, 0, kGfxPushBytes};
  ASSERT_TRUE(gfx_pipe.create_pipeline_layout(
      context.device(), &gfx_layout.value(), 1, &gfx_push).is_ok());
  ASSERT_TRUE(gfx_pipe.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      gfx_pipe.pipeline_layout(), false, false, false).is_ok());

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

  omnicpp::core::LatencyTracker<> latency;
  std::uint64_t accepted_total = 0;

  for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
    const auto frame_start = std::chrono::steady_clock::now();

    // CPU: animate spheres on two orbit rings; half the instances drift
    // behind the camera and back every 120 frames (accept+reject churn).
    {
      auto* words = static_cast<std::uint32_t*>(cull_buf.value().mapped);
      words[0] = kInstanceCount;
      words[1] = 0;
      float planes[24];
      make_frustum_planes(kFovY, kAspect, kNear, kFar, planes);
      std::uint32_t plane_words[24];
      std::memcpy(plane_words, planes, sizeof(plane_words));
      for (std::size_t i = 0; i < static_cast<std::size_t>(24); ++i) words[2 + i] = plane_words[i];
      const float t = static_cast<float>(frame) * (1.0f / 60.0f);
      for (std::uint32_t i = 0; i < kInstanceCount; ++i) {
        const float ring = (i % 2 == 0) ? 10.0f : 22.0f;
        const float ang = t * (0.3f + 0.1f * (i % 5)) + static_cast<float>(i);
        const float bob = (i % 4 == 0) ? std::sin(t * 2.0f) * 14.0f : 0.0f;
        const float z = -ring + bob;  // z<0 in front; bob>14 pushes behind camera
        const float sphere[4] = {std::cos(ang) * 3.0f, std::sin(ang) * 3.0f, z, 0.9f};
        std::uint32_t sw[4];
        std::memcpy(sw, sphere, sizeof(sw));
        for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) words[kSphereOffset + i * 4U + k] = sw[k];
      }
      auto* cmd = static_cast<DrawCmd*>(draw_buf.value().mapped);
      cmd->instance_count = 0;
    }

    ASSERT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);

    // Host writes -> device visibility (cull payload + indirect command).
    VkBufferMemoryBarrier host_vis[2] = {};
    host_vis[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host_vis[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_vis[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    host_vis[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_vis[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_vis[0].buffer = cull_buf.value().buffer;
    host_vis[0].size = VK_WHOLE_SIZE;
    host_vis[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host_vis[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_vis[1].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    host_vis[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_vis[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_vis[1].buffer = draw_buf.value().buffer;
    host_vis[1].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 0, nullptr, 2, host_vis, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe.pipeline());
    const VkDescriptorSet cds = comp_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp_pipe.pipeline_layout(),
                            0, 1, &cds, 0, nullptr);
    const std::uint32_t push[2] = {kSphereOffset, kCompactOffset};
    vkCmdPushConstants(cb, comp_pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), push);
    vkCmdDispatch(cb, (kInstanceCount + 63U) / 64U, 1, 1);

    VkBufferMemoryBarrier barriers[2] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].buffer = draw_buf.value().buffer;
    barriers[0].size = VK_WHOLE_SIZE;
    barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].buffer = cull_buf.value().buffer;
    barriers[1].size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 0, nullptr, 2, barriers, 0, nullptr);

    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {256, 256};
    VkClearValue clear{};
    clear.color = {{0.02f, 0.02f, 0.05f, 1.0f}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, 256, 256, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {256, 256}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline());
    const VkDescriptorSet gds = gfx_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline_layout(),
                            0, 1, &gds, 0, nullptr);
    struct {
      float view_proj[16];
      std::uint32_t data_offset;
      std::uint32_t comp_offset;
      std::uint32_t lod_scale;
      std::uint32_t pad0;
    } gfx_push_data{};
    make_perspective(kFovY, kAspect, kNear, kFar, gfx_push_data.view_proj);
    gfx_push_data.data_offset = kInstanceCount;
    gfx_push_data.comp_offset = 0U;
    {
      const float one = 1.0f;
      std::memcpy(&gfx_push_data.lod_scale, &one, 4U);
    }
    vkCmdPushConstants(cb, gfx_pipe.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT,
                       0, kGfxPushBytes, &gfx_push_data);
    vkCmdDrawIndirect(cb, draw_buf.value().buffer, 0, 1, sizeof(DrawCmd));
    vkCmdEndRenderPass(cb);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);

    ASSERT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

    const auto frame_end = std::chrono::steady_clock::now();
    latency.record(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(frame_end - frame_start).count()));

    auto* cmd = static_cast<DrawCmd*>(draw_buf.value().mapped);
    accepted_total += cmd->instance_count;
  }

  const auto stats = latency.percentiles();
  std::cout << "\n[benchmark] GPU-driven sustained loop (" << kFrames
            << " frames, " << kInstanceCount << " instances, 256x256 offscreen)\n";
  std::cout << "  accepted-instance total: " << accepted_total << "\n";
  std::cout << "  frame CPU p50: " << stats.p50_ns / 1000 << " us\n"
            << "  frame CPU p90: " << stats.p90_ns / 1000 << " us\n"
            << "  frame CPU p99: " << stats.p99_ns / 1000 << " us\n"
            << "  frame CPU p99.9: " << stats.p999_ns / 1000 << " us\n"
            << "  frame CPU max: " << stats.max_ns / 1000 << " us\n";
  std::cout << std::flush;

  // Sanity: most frames must accept a nonzero number of instances (the
  // animation never empties the scene for more than a few frames), and the
  // loop must have actually recorded kFrames samples.
  EXPECT_GT(accepted_total, 0U);
  EXPECT_EQ(stats.total_count, static_cast<std::uint64_t>(kFrames));
  EXPECT_GT(stats.p50_ns, 0U);

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  gfx_pipe.cleanup(context.device());
  comp_pipe.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation b1 = cull_buf.value();
  omnicpp::render::Allocation b2 = inst_buf.value();
  omnicpp::render::Allocation b3 = draw_buf.value();
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
