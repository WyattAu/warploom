//! @file test_async_compute_spike.cpp
//! @brief Async-compute spike (§8.2 of the long-term roadmap): can the H-Z
//! style reduce chain overlap graphics work on a second queue?
//!
//! The RTX 2060 exposes a COMPUTE-only family (family 2, 8 queues), so true
//! async compute exists on this hardware. This spike asks the only question
//! that matters before rewiring the renderer: does submitting the
//! production depth_reduce kernel (buffer path, the real H-Z workload:
//! per-tile maxima over a depth word buffer) on the compute queue while the
//! graphics queue renders actually beat doing both sequentially on the
//! graphics queue?
//!
//! Arrangement A (status quo): render + reduce, sequential, graphics queue.
//! Arrangement B (candidate): render on graphics, reduce on compute, no
//! cross-waits. The two legs touch DISJOINT resources (the render pass
//! writes only the offscreen target; the reduce reads only the static depth
//! buffer and writes only the pyramid buffer), so B needs no handshake even
//! in a real design -- it measures the overlap ceiling, and any production
//! handshake would only ADD cost to B. The verdict rule is therefore
//! one-directional and conservative: adopt only if B beats A clearly;
//! otherwise drop, with numbers. Either way this file stays as the
//! measurement record.
//!
//! Sharing design assumed: the depth/pyramid buffers come from
//! VulkanMemoryAllocator::create_buffer, which already marks buffers
//! VK_SHARING_MODE_CONCURRENT over {compute, graphics} when a dedicated
//! compute family exists -- no release/acquire ping-pong needed, on the
//! exact path production code would use. (The IMAGE-based sampled path,
//! VulkanHiZPyramid, is still EXCLUSIVE; moving it across queues would need
//! ownership transfers. That migration is a follow-up ONLY if this spike
//! shows a win -- concurrent-sharing already shows the ceiling.)
//!
//! Load-bearing assertions (not timing): the pyramid is bit-identical
//! whichever queue reduced it, and every tile word equals floatBitsToUint
//! (0.5) -- the constant-depth input makes the kernel output fully
//! determined, so a mis-wired dispatch cannot hide behind A==B. Zero
//! validation diagnostics proves no queue-ownership violation on the shared
//! buffers.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace {

#if defined(WARPLOOM_HAS_VULKAN)

// Spike frame size: 1280x768 at tile 32 gives 40x24 tiles, and 40/8=5,
// 24/8=3 workgroups -- the depth_reduce kernel dispatches tiles/8, so both
// tile counts must be multiples of 8. (720p fails this: 720/32=22.5.)
constexpr std::uint32_t kW = 1280U;
constexpr std::uint32_t kH = 768U;
constexpr std::uint32_t kTile = 32U;
constexpr std::uint32_t kTilesX = kW / kTile;  // 40
constexpr std::uint32_t kTilesY = kH / kTile;  // 24
// Graphics partner work per frame: fullscreen-triangle draws (vertex-bound,
// real graphics-engine work to overlap against).
constexpr std::uint32_t kDraws = 200U;
constexpr std::uint32_t kWarmup = 5U;
constexpr std::uint32_t kIters = 30U;

std::uint32_t bits(float f) {
  std::uint32_t u = 0U;
  std::memcpy(&u, &f, 4U);
  return u;
}

#endif  // defined(WARPLOOM_HAS_VULKAN)

}  // namespace

#if defined(WARPLOOM_HAS_VULKAN)

TEST(AsyncComputeSpike, ReduceOverlapsGraphics) {
  using warploom::render::VulkanContext;
  if (!VulkanContext::is_available()) GTEST_SKIP() << "no Vulkan device";
  VulkanContext context;
  ASSERT_TRUE(context.initialize("AsyncSpike", true).is_ok());
  if (!context.has_dedicated_compute()) {
    context.cleanup();
    GTEST_SKIP() << "no COMPUTE-only family: nothing to overlap with";
  }
  const std::uint32_t gfx_family =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);
  const std::uint32_t cmp_family = context.compute_family_index();
  ASSERT_NE(gfx_family, cmp_family);

  warploom::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device()).is_ok());

  // ---- Static depth words (0.5f) + pyramid words, host-visible/coherent. --
  // create_buffer marks these CONCURRENT over {compute, graphics} when the
  // dedicated family exists (allocator source) -- the production sharing path.
  auto depth_buf = allocator.create_buffer(
      static_cast<VkDeviceSize>(kW) * kH * 4U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(depth_buf.is_ok());
  auto pyramid_buf = allocator.create_buffer(
      static_cast<VkDeviceSize>(kTilesX) * kTilesY * 4U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(pyramid_buf.is_ok());
  {
    auto* words = static_cast<std::uint32_t*>(depth_buf.value().mapped);
    ASSERT_NE(words, nullptr);
    const std::uint32_t half = bits(0.5F);
    for (std::size_t i = 0U; i < static_cast<std::size_t>(kW) * kH; ++i) {
      words[i] = half;
    }
  }

  // ---- Reduce pipeline: the production depth_reduce kernel. ----
  warploom::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  const std::vector<warploom::render::ReflectedBinding> reduce_bindings = {
      {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_COMPUTE_BIT},
      {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_COMPUTE_BIT},
  };
  auto reduce_layout = descriptors.create_layout(reduce_bindings, 1U);
  ASSERT_TRUE(reduce_layout.is_ok());
  auto reduce_set = descriptors.allocate_set(reduce_layout.value());
  ASSERT_TRUE(reduce_set.is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(reduce_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                depth_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(reduce_set.value(), 1U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                pyramid_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  const VkPushConstantRange reduce_push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 16U};
  warploom::render::VulkanPipeline reduce_pipe;
  ASSERT_TRUE(reduce_pipe
                  .load_shader_stage_file(context.device(),
                                          shader_dir + "/depth_reduce.comp.spv",
                                          "compute")
                  .is_ok());
  ASSERT_TRUE(reduce_pipe
                  .create_pipeline_layout(context.device(),
                                          &reduce_layout.value(), 1U,
                                          &reduce_push)
                  .is_ok());
  ASSERT_TRUE(reduce_pipe
                  .create_compute_pipeline(context.device(),
                                           reduce_pipe.pipeline_layout())
                  .is_ok());

  // ---- Graphics partner: offscreen target + fullscreen triangle. ----
  warploom::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target
                  .create(context.device(), context.physical_device(),
                          VK_FORMAT_B8G8R8A8_UNORM, kW, kH, &allocator)
                  .is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());
  warploom::render::VulkanPipeline tri_pipe;
  ASSERT_TRUE(tri_pipe
                  .load_shader_file(context.device(),
                                    shader_dir + "/triangle.vert.spv")
                  .is_ok());
  ASSERT_TRUE(tri_pipe
                  .load_shader_file(context.device(),
                                    shader_dir + "/triangle.frag.spv")
                  .is_ok());
  ASSERT_TRUE(tri_pipe
                  .create_graphics_pipeline(context.device(),
                                            target.render_pass(),
                                            target.format(), VK_NULL_HANDLE,
                                            false, false, false)
                  .is_ok());

  // ---- Command pools: one per family; one buffer each, reset per iter. ----
  const auto gfx_pool_result =
      warploom::render::VulkanRenderer::create_command_pool(context.device(),
                                                            gfx_family);
  ASSERT_TRUE(gfx_pool_result.is_ok());
  const auto cmp_pool_result =
      warploom::render::VulkanRenderer::create_command_pool(context.device(),
                                                            cmp_family);
  ASSERT_TRUE(cmp_pool_result.is_ok());
  const VkCommandPool gfx_pool = gfx_pool_result.value();
  const VkCommandPool cmp_pool = cmp_pool_result.value();
  const auto gfx_cb_result =
      warploom::render::VulkanRenderer::allocate_command_buffer(
          context.device(), gfx_pool);
  ASSERT_TRUE(gfx_cb_result.is_ok());
  const auto cmp_cb_result =
      warploom::render::VulkanRenderer::allocate_command_buffer(
          context.device(), cmp_pool);
  ASSERT_TRUE(cmp_cb_result.is_ok());
  const VkCommandBuffer gfx_cb = gfx_cb_result.value();
  const VkCommandBuffer cmp_cb = cmp_cb_result.value();

  // ---- Setup submit: host-written depth -> shader-visible, then idle. ----
  // Visibility established once persists across later submits; the timed
  // loops carry no setup barriers, on either arrangement.
  {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(gfx_cb, &bi), VK_SUCCESS);
    VkBufferMemoryBarrier host_vis{};
    host_vis.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host_vis.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_vis.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    host_vis.buffer = depth_buf.value().buffer;
    host_vis.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(gfx_cb, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 0U, nullptr,
                         1U, &host_vis, 0U, nullptr);
    ASSERT_EQ(vkEndCommandBuffer(gfx_cb), VK_SUCCESS);
    VkFence fence{VK_NULL_HANDLE};
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ASSERT_EQ(vkCreateFence(context.device(), &fi, nullptr, &fence),
              VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &gfx_cb;
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1U, &si, fence),
              VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1U, &fence, VK_TRUE,
                              1000000000ULL),
              VK_SUCCESS);
    vkDestroyFence(context.device(), fence, nullptr);
  }

  auto record_renderpass = [&](VkCommandBuffer cb) {
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = target.render_pass();
    rp.framebuffer = target.framebuffer();
    rp.renderArea.extent = {kW, kH};
    VkClearValue clear{};
    clear.color = {{0.0F, 0.0F, 0.0F, 1.0F}};
    rp.clearValueCount = 1U;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{};
    vp.width = static_cast<float>(kW);
    vp.height = static_cast<float>(kH);
    vp.maxDepth = 1.0F;
    vkCmdSetViewport(cb, 0U, 1U, &vp);
    VkRect2D sc{};
    sc.extent = {kW, kH};
    vkCmdSetScissor(cb, 0U, 1U, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tri_pipe.pipeline());
    for (std::uint32_t d = 0U; d < kDraws; ++d) {
      vkCmdDraw(cb, 3U, 1U, 0U, 0U);
    }
    vkCmdEndRenderPass(cb);
  };
  auto record_reduce = [&](VkCommandBuffer cb) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      reduce_pipe.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            reduce_pipe.pipeline_layout(), 0U, 1U,
                            &reduce_set.value(), 0U, nullptr);
    const std::uint32_t push[4] = {kW, kH, kTilesX, kTile};
    vkCmdPushConstants(cb, reduce_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), push);
    vkCmdDispatch(cb, kTilesX / 8U, kTilesY / 8U, 1U);
    // Reduce writes -> host read (coherent mapping; fence wait completes it).
    VkBufferMemoryBarrier to_host{};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.buffer = pyramid_buf.value().buffer;
    to_host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U,
                         &to_host, 0U, nullptr);
  };
  VkFence fence_a{VK_NULL_HANDLE}, fence_b{VK_NULL_HANDLE};
  {
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ASSERT_EQ(vkCreateFence(context.device(), &fi, nullptr, &fence_a),
              VK_SUCCESS);
    ASSERT_EQ(vkCreateFence(context.device(), &fi, nullptr, &fence_b),
              VK_SUCCESS);
  }

  // ---- Arrangement A: render + reduce, sequential, graphics queue. ----
  double wall_a = 0.0;
  for (std::uint32_t i = 0U; i < kWarmup + kIters; ++i) {
    ASSERT_EQ(vkResetCommandBuffer(gfx_cb, 0U), VK_SUCCESS);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(gfx_cb, &bi), VK_SUCCESS);
    record_renderpass(gfx_cb);
    record_reduce(gfx_cb);
    ASSERT_EQ(vkEndCommandBuffer(gfx_cb), VK_SUCCESS);
    ASSERT_EQ(vkResetFences(context.device(), 1U, &fence_a), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &gfx_cb;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1U, &si, fence_a),
              VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1U, &fence_a, VK_TRUE,
                              1000000000ULL),
              VK_SUCCESS);
    const auto t1 = std::chrono::steady_clock::now();
    if (i >= kWarmup) {
      wall_a += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
  }
  std::vector<std::uint32_t> pyramid_a(static_cast<std::size_t>(kTilesX) * kTilesY);
  std::memcpy(pyramid_a.data(), pyramid_buf.value().mapped,
              pyramid_a.size() * 4U);

  // ---- Arrangement B: render on graphics + reduce on compute. ----
  double wall_b = 0.0;
  for (std::uint32_t i = 0U; i < kWarmup + kIters; ++i) {
    ASSERT_EQ(vkResetCommandBuffer(gfx_cb, 0U), VK_SUCCESS);
    ASSERT_EQ(vkResetCommandBuffer(cmp_cb, 0U), VK_SUCCESS);
    {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      ASSERT_EQ(vkBeginCommandBuffer(gfx_cb, &bi), VK_SUCCESS);
      record_renderpass(gfx_cb);
      ASSERT_EQ(vkEndCommandBuffer(gfx_cb), VK_SUCCESS);
    }
    {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      ASSERT_EQ(vkBeginCommandBuffer(cmp_cb, &bi), VK_SUCCESS);
      record_reduce(cmp_cb);
      ASSERT_EQ(vkEndCommandBuffer(cmp_cb), VK_SUCCESS);
    }
    ASSERT_EQ(vkResetFences(context.device(), 1U, &fence_a), VK_SUCCESS);
    ASSERT_EQ(vkResetFences(context.device(), 1U, &fence_b), VK_SUCCESS);
    VkSubmitInfo gs{};
    gs.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    gs.commandBufferCount = 1U;
    gs.pCommandBuffers = &gfx_cb;
    VkSubmitInfo cs{};
    cs.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    cs.commandBufferCount = 1U;
    cs.pCommandBuffers = &cmp_cb;
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1U, &gs, fence_a),
              VK_SUCCESS);
    ASSERT_EQ(vkQueueSubmit(context.compute_queue(), 1U, &cs, fence_b),
              VK_SUCCESS);
    const VkFence both[2] = {fence_a, fence_b};
    ASSERT_EQ(vkWaitForFences(context.device(), 2U, both, VK_TRUE,
                              1000000000ULL),
              VK_SUCCESS);
    const auto t1 = std::chrono::steady_clock::now();
    if (i >= kWarmup) {
      wall_b += std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
  }
  std::vector<std::uint32_t> pyramid_b(static_cast<std::size_t>(kTilesX) * kTilesY);
  std::memcpy(pyramid_b.data(), pyramid_buf.value().mapped,
              pyramid_b.size() * 4U);

  wall_a /= static_cast<double>(kIters);
  wall_b /= static_cast<double>(kIters);
  std::printf("[async-spike] arrangement A (sequential, gfx queue): %.3f ms/frame\n",
              wall_a);
  std::printf("[async-spike] arrangement B (gfx + compute queues):   %.3f ms/frame\n",
              wall_b);
  std::printf("[async-spike] delta B-A: %+.3f ms (%.1f%%)\n", wall_b - wall_a,
              100.0 * (wall_b - wall_a) / wall_a);

  // Queue-equivalence AND kernel-correctness: constant 0.5 depth means every
  // tile maximum is exactly floatBitsToUint(0.5), whichever queue reduced.
  // A mis-wired dispatch (wrong push, wrong set) still passes A==B only if
  // both legs share the bug -- the absolute check closes that hole.
  const std::uint32_t kHalf = bits(0.5F);
  ASSERT_EQ(pyramid_a.size(), pyramid_b.size());
  for (std::size_t i = 0U; i < pyramid_a.size(); ++i) {
    EXPECT_EQ(pyramid_a[i], kHalf) << "tile " << i << " (A leg)";
    EXPECT_EQ(pyramid_b[i], kHalf) << "tile " << i << " (B leg)";
    EXPECT_EQ(pyramid_a[i], pyramid_b[i]) << "tile " << i << " A-vs-B";
    if (pyramid_a[i] != kHalf || pyramid_b[i] != kHalf) break;
  }

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence_a, nullptr);
  vkDestroyFence(context.device(), fence_b, nullptr);
  vkDestroyCommandPool(context.device(), gfx_pool, nullptr);
  vkDestroyCommandPool(context.device(), cmp_pool, nullptr);
  tri_pipe.cleanup(context.device());
  target.cleanup();
  reduce_pipe.cleanup(context.device());
  descriptors.cleanup();
  warploom::render::Allocation pyramid_live = pyramid_buf.value();
  warploom::render::Allocation depth_live = depth_buf.value();
  allocator.destroy_allocation(pyramid_live);
  allocator.destroy_allocation(depth_live);
  allocator.cleanup();
  context.cleanup();
}

#endif  // defined(WARPLOOM_HAS_VULKAN)
