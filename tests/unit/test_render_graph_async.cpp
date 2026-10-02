//! @file test_render_graph_async.cpp
//! @brief Render-graph async compute: mixed compute + render node sequences
//!        synchronized through the graph's buffer edges. Compute generates
//!        animated triangle geometry; a render pass draws it through vertex
//!        pulling. Verified by mapped-buffer readback of the compute output
//!        (analytic rotation) and pixel classification of the render.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "engine/render/vulkan_compute.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_render_graph.hpp"
#include "engine/render/vulkan_renderer.hpp"

#ifdef WARPLOOM_HAS_VULKAN
#include "vulkan_test_readback.hpp"
using omnicpp_test::readback_swapchain_image;
#endif

namespace {

constexpr float kFovY = 1.05f;
constexpr float kAspect = 1.0f;
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr std::uint32_t kSize = 256U;

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

#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)

namespace {

//! Per-frame recording context handed through user_data.
struct GraphCtx {
  VkDescriptorSet dset{VK_NULL_HANDLE};
  VkPipelineLayout comp_layout{VK_NULL_HANDLE};
  VkPipelineLayout gfx_layout{VK_NULL_HANDLE};
  VkPipeline comp_pipe{VK_NULL_HANDLE};
  VkPipeline gfx_pipe{VK_NULL_HANDLE};
  float phase{0.0f};
};

void record_compute_cb(VkCommandBuffer cb,
                       const omnicpp::render::GraphComputePass& pass,
                       void* user) {
  (void)pass;
  auto* c = static_cast<GraphCtx*>(user);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->comp_pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->comp_layout,
                          0, 1, &c->dset, 0, nullptr);
  std::uint32_t phase_bits;
  std::memcpy(&phase_bits, &c->phase, 4U);
  const std::uint32_t push[2] = {3U, phase_bits};  // count, phase
  vkCmdPushConstants(cb, c->comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(push), push);
  vkCmdDispatch(cb, 1, 1, 1);
}

void record_draw_cb(VkCommandBuffer cb, const omnicpp::render::GraphPass& pass,
                    void* user) {
  (void)pass;
  auto* c = static_cast<GraphCtx*>(user);
  VkViewport vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
  vkCmdSetViewport(cb, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {kSize, kSize}};
  vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->gfx_pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->gfx_layout,
                          0, 1, &c->dset, 0, nullptr);
  vkCmdDraw(cb, 3, 1, 0, 0);
}

}  // namespace
#endif

TEST(VulkanHardware, RenderGraphComputeThenDraw) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppRenderGraphTest", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(),
                                   context.physical_device()).is_ok());

  // --- Vertex buffer (compute output, vertex-pull input). ---
  constexpr VkDeviceSize kVertexBytes = 6U * 16U;  // 6 vec4
  auto vbuf = allocator.create_buffer(
      kVertexBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(vbuf.is_ok());

  // --- Descriptors + pipelines. ---
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> bindings(1);
  bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT};
  auto layout = manager.create_layout(bindings, 1);
  ASSERT_TRUE(layout.is_ok());
  auto dset = manager.allocate_set(layout.value());
  ASSERT_TRUE(dset.is_ok());
  ASSERT_TRUE(manager.write_buffer(dset.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   vbuf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  const VkPushConstantRange comp_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
  omnicpp::render::VulkanPipeline comp_pipe;
  ASSERT_TRUE(comp_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/gen_triangle.comp.spv", "compute").is_ok());
  ASSERT_TRUE(comp_pipe.create_pipeline_layout(
      context.device(), &layout.value(), 1, &comp_push).is_ok());
  ASSERT_TRUE(comp_pipe.create_compute_pipeline(
      context.device(), comp_pipe.pipeline_layout()).is_ok());

  const VkPushConstantRange gfx_push{VK_SHADER_STAGE_VERTEX_BIT, 0, 4U};
  omnicpp::render::VulkanPipeline gfx_pipe;
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/vertexpull_triangle.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/vertexpull_triangle.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(gfx_pipe.create_pipeline_layout(
      context.device(), &layout.value(), 1, &gfx_push).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize,
                            &allocator).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());
  ASSERT_TRUE(gfx_pipe.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      gfx_pipe.pipeline_layout(), false, false, false).is_ok());

  // --- Graph: compute generates, then a render pass draws. ---
  GraphCtx ctx{};
  ctx.dset = dset.value();
  ctx.comp_layout = comp_pipe.pipeline_layout();
  ctx.gfx_layout = gfx_pipe.pipeline_layout();
  ctx.comp_pipe = comp_pipe.pipeline();
  ctx.gfx_pipe = gfx_pipe.pipeline();

  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

  omnicpp::render::GraphComputePass compute_pass{};
  compute_pass.name = "generate_triangle";
  compute_pass.group_count_x = 1;
  compute_pass.user_data = &ctx;

  omnicpp::render::GraphPass draw_pass{};
  draw_pass.name = "draw_triangle";
  draw_pass.render_pass = target.render_pass();
  draw_pass.framebuffer = target.framebuffer();
  draw_pass.attachments = {omnicpp::render::color_attachment(
      target.image(), target.image_view(), target.format(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)};
  // Consumer-declared buffer edge: compute writes -> vertex-stage reads.
  omnicpp::render::GraphBufferEdge edge{};
  edge.buffer = vbuf.value().buffer;
  edge.producer_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  edge.consumer_stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
  edge.consumer_access = VK_ACCESS_SHADER_READ_BIT;
  draw_pass.buffer_edges = {edge};
  draw_pass.clear_values = &clear;
  draw_pass.clear_value_count = 1;
  draw_pass.width = kSize;
  draw_pass.height = kSize;
  draw_pass.user_data = &ctx;

  const std::vector<omnicpp::render::GraphNode> nodes = {
      omnicpp::render::GraphNode::from_compute(compute_pass),
      omnicpp::render::GraphNode::from_render(draw_pass),
  };
  const auto compiled = omnicpp::render::compile_graph(nodes);

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

  //! Records + submits one graph frame; returns the readback (submitted=false
  //! signals a recording failure; EXPECTs already fired).
  const auto run_frame = [&](float phase) -> omnicpp_test::ReadbackResult {
    ctx.phase = phase;
    if (vkResetCommandBuffer(cb, 0) != VK_SUCCESS) {
      EXPECT_TRUE(false);
      return {};
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);
    omnicpp::render::execute_graph(cb, nodes, compiled, &record_draw_cb,
                           &record_compute_cb);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    EXPECT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    EXPECT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
    return readback_swapchain_image(
        context.physical_device(), context.device(), context.graphics_queue(),
        static_cast<std::uint32_t>(context.queue_families().graphics_family),
        target.image(), target.format(), kSize, kSize,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  };

  // --- Frame 1: phase 0. Vertex 0 points "up" pre-rotation; hues make the
  // upper region green-dominant, lower region blue-dominant. ---
  const auto frame1 = run_frame(0.0f);
  ASSERT_TRUE(frame1.submitted);
  EXPECT_GT(frame1.non_clear_pixels, 500U);
  EXPECT_GT(frame1.green_dominant_pixels, 100U);

  // Compute output verified analytically: vertex 0 sits at angle 0 (radius
  // 0.85) for phase 0 — x = +0.85, y = 0.
  {
    const auto* data = static_cast<const float*>(vbuf.value().mapped);
    EXPECT_NEAR(data[0], 0.85f, 1e-4f);
    EXPECT_NEAR(data[1], 0.0f, 1e-4f);
  }

  // --- Frame 2: rotate by ~1 radian; image and buffer must both change. ---
  const auto frame2 = run_frame(1.0f);
  ASSERT_TRUE(frame2.submitted);
  EXPECT_NE(frame2.hash, frame1.hash);
  {
    const auto* data = static_cast<const float*>(vbuf.value().mapped);
    EXPECT_NEAR(data[0], 0.85f * std::cos(1.0f), 1e-4f);
    EXPECT_NEAR(data[1], 0.85f * std::sin(1.0f), 1e-4f);
  }

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  gfx_pipe.cleanup(context.device());
  comp_pipe.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation v = vbuf.value();
  allocator.destroy_allocation(v);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
