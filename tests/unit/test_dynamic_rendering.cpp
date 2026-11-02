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

#include "warploom/render/vulkan_compose_chain.hpp"
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/render/vulkan_render_graph.hpp"
#include "warploom/render/vulkan_render_pass.hpp"
#include "warploom/render/vulkan_transient_store.hpp"
#include "vulkan_test_readback.hpp"

#include <string>

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

namespace {

using warploom::render::ComposeChainConfig;
using warploom::render::RendererConfig;
using warploom::render::VulkanComposeChain;
using warploom::render::VulkanContext;
using warploom::render::VulkanMemoryAllocator;
using warploom::render::VulkanOffscreenTarget;
using warploom::render::VulkanPipeline;
using warploom::render::VulkanRenderer;
using warploom::render::VulkanRenderPass;

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

#if defined(WARPLOOM_TEST_SHADER_DIR)

// Blade stages 1+2: the compose bloom downsample AND the tonemap migrate to
// dynamic rendering when requested AND advertised and the present image/view
// ride along. Proves the routing (both stages land in dyn_passes, none
// baked), the node-list gate (mixed nodes verify; an unknown image fails
// them, and the present image verifies as its own planned kind), and the
// recording (the mixed graph executes with zero validation diagnostics).
// Sabotage-checked: requesting dynamic on a device without the feature, not
// requesting it at all, or withholding the present image/view keeps every
// stage baked (dyn_passes empty).
TEST(DynamicRendering, ComposeBloomMigratesToDynamic) {
  DynamicRenderingHarness h;
  if (!h.initialize()) GTEST_SKIP() << "no Vulkan device";
  if (!h.context.has_dynamic_rendering()) {
    h.cleanup();
    GTEST_SKIP() << "dynamicRendering not advertised";
  }

  constexpr std::uint32_t kSize = 256U;
  VulkanOffscreenTarget ldr;
  ASSERT_TRUE(ldr
                  .create(h.context.device(), h.context.physical_device(),
                          VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &h.allocator,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                  .is_ok());
  const VkFormat depth_format =
      VulkanRenderPass::find_supported_depth_format(h.context.physical_device());
  VulkanRenderPass present_pass;
  ASSERT_TRUE(present_pass.create(h.context.device(), VK_FORMAT_B8G8R8A8_UNORM,
                                  depth_format)
                  .is_ok());
  ASSERT_TRUE(present_pass
                  .create_depth_resources(h.context.device(),
                                          h.context.physical_device(),
                                          depth_format, kSize, kSize)
                  .is_ok());
  ASSERT_TRUE(present_pass
                  .create_framebuffers(h.context.device(), {ldr.image_view()},
                                       kSize, kSize)
                  .is_ok());

  ComposeChainConfig cfg{};
  cfg.enable_bloom = true;
  cfg.compose_shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.enable_dynamic_rendering = true;
  VulkanComposeChain chain;
  ASSERT_TRUE(chain
                  .ensure(h.context.device(), h.context.physical_device(),
                          h.context.graphics_queue(), cfg,
                          present_pass.render_pass(), VK_FORMAT_B8G8R8A8_UNORM,
                          kSize, kSize)
                  .is_ok());
  ASSERT_TRUE(chain.ready());
  // Negotiation: requested and advertised, so the bloom stage goes dynamic.
  // (bloom_available() proves the bloom shaders existed; without them there
  // is no bloom stage in either framing.)
  ASSERT_TRUE(chain.bloom_available());
  EXPECT_TRUE(chain.dynamic_bloom());
  // enable_bloom in the config only pre-allocates the bloom resources;
  // the live toggle decides what records (same as the golden test's
  // renderer.set_bloom_enabled(true)).
  ASSERT_TRUE(chain.set_bloom_enabled(true));

  std::vector<warploom::render::GraphPass> passes;
  std::vector<warploom::render::GraphDynamicPass> dyn_passes;
  std::vector<warploom::render::ComposeStage> stages;
  VkClearValue clear[2]{};
  // Blade stage 2: the present image/view ride along, so the tonemap goes
  // dynamic too -- both compose stages in dyn_passes, none baked.
  chain.build_graph_passes(present_pass.render_pass(),
                           present_pass.framebuffer(0U), ldr.image(),
                           ldr.image_view(), kSize, kSize, passes, dyn_passes,
                           stages, clear);
  ASSERT_EQ(stages.size(), 2U);
  EXPECT_TRUE(stages[0].dynamic) << "bloom downsample must go dynamic";
  EXPECT_TRUE(stages[1].dynamic) << "tonemap must go dynamic (present given)";
  ASSERT_EQ(dyn_passes.size(), 2U);
  EXPECT_TRUE(passes.empty());

  // Mixed node list in stage order, exactly as the renderer builds it: the
  // scene node first (it owns the HDR/depth first-use, so the skeleton
  // intervals the gate compares against line up), then bloom, then tonemap.
  // The scene callback is null here, so the scene pass only clears -- the
  // point is the framing/barriers, not the pixels.
  warploom::render::GraphPass scene{};
  scene.name = "scene";
  scene.render_pass = chain.scene_pass();
  scene.framebuffer = chain.scene_framebuffer();
  scene.width = kSize;
  scene.height = kSize;
  scene.clear_values = clear;
  scene.clear_value_count = 2U;
  scene.attachments = {
      warploom::render::color_attachment(chain.scene_image(),
                                          chain.scene_image_view(),
                                          chain.scene_image_format(),
                                          chain.scene_color_final_layout()),
      warploom::render::depth_attachment(chain.scene_depth_image(),
                                          chain.scene_depth_view(),
                                          chain.scene_depth_format(),
                                          chain.scene_depth_final_layout())};
  std::vector<warploom::render::GraphNode> nodes;
  nodes.push_back(warploom::render::GraphNode::from_render(scene));
  nodes.push_back(warploom::render::GraphNode::from_dynamic(dyn_passes[0]));
  nodes.push_back(warploom::render::GraphNode::from_dynamic(dyn_passes[1]));
  EXPECT_TRUE(chain.verify_frame_nodes(nodes));

  // Sabotage: a sampled image the plan never issued fails the gate instead
  // of rendering from an image the store does not cover.
  {
    warploom::render::GraphDynamicPass forged = dyn_passes[1];
    warploom::render::GraphSampledImage unknown{};
    unknown.image = reinterpret_cast<VkImage>(0xDEADBEEF);
    unknown.used_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    forged.sampled_images.push_back(unknown);
    std::vector<warploom::render::GraphNode> bad_nodes;
    bad_nodes.push_back(warploom::render::GraphNode::from_render(scene));
    bad_nodes.push_back(warploom::render::GraphNode::from_dynamic(dyn_passes[0]));
    bad_nodes.push_back(warploom::render::GraphNode::from_dynamic(forged));
    EXPECT_FALSE(chain.verify_frame_nodes(bad_nodes));
  }

  // Record the mixed graph on a real command buffer. The scene node owns the
  // HDR/depth first-use (UNDEFINED -> attachment), so no manual seeding: the
  // compiler's barriers match the fresh store images exactly.
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
  const warploom::render::CompiledGraph compiled =
      warploom::render::compile_graph(nodes);
  warploom::render::execute_graph(
      cb, nodes, compiled, &warploom::render::record_compose_stage, nullptr,
      0U, &warploom::render::record_compose_dynamic_stage);
  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1U;
  submit.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(h.context.graphics_queue(), 1U, &submit,
                          VK_NULL_HANDLE),
            VK_SUCCESS);
  ASSERT_EQ(vkQueueWaitIdle(h.context.graphics_queue()), VK_SUCCESS);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);
  vkFreeCommandBuffers(h.context.device(), h.pool, 1U, &cb);

  // Fallback sabotage in reverse: dynamic NOT requested keeps every stage
  // baked, on a separate chain so no rebuild path is involved.
  {
    ComposeChainConfig baked_cfg{};
    baked_cfg.enable_bloom = true;
    baked_cfg.compose_shader_dir = WARPLOOM_TEST_SHADER_DIR;
    baked_cfg.enable_dynamic_rendering = false;
    VulkanComposeChain baked;
    ASSERT_TRUE(baked
                    .ensure(h.context.device(), h.context.physical_device(),
                            h.context.graphics_queue(), baked_cfg,
                            present_pass.render_pass(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize)
                    .is_ok());
    ASSERT_TRUE(baked.ready());
    EXPECT_FALSE(baked.dynamic_bloom());
    ASSERT_TRUE(baked.set_bloom_enabled(true));
    std::vector<warploom::render::GraphPass> baked_passes;
    std::vector<warploom::render::GraphDynamicPass> baked_dyn;
    std::vector<warploom::render::ComposeStage> baked_stages;
    VkClearValue baked_clear[2]{};
    baked.build_graph_passes(present_pass.render_pass(),
                             present_pass.framebuffer(0U), VK_NULL_HANDLE,
                             VK_NULL_HANDLE, kSize, kSize, baked_passes,
                             baked_dyn, baked_stages, baked_clear);
    EXPECT_TRUE(baked_dyn.empty());
    EXPECT_EQ(baked_passes.size(), 2U);
    baked.destroy();
  }

  chain.destroy();
  present_pass.cleanup(h.context.device());
  ldr.cleanup();
  h.cleanup();
}

// Blade stage 1 through the production path: the renderer's unified
// scene+compose frame with bloom requested AND dynamic rendering negotiated.
// The bloom node records via record_unified_frame_dynamic_node while the
// scene and tonemap stay baked -- one compile, one execute. Proves the
// renderer wiring end to end: a composed, bloom-additive frame with zero
// validation diagnostics and non-trivial pixels (an unwired dynamic node
// would leave the bloom target cleared, and a miswired gate would fail
// the frame before recording).
TEST(DynamicRendering, UnifiedFrameWithDynamicBloomRecordsClean) {
  DynamicRenderingHarness h;
  if (!h.initialize()) GTEST_SKIP() << "no Vulkan device";
  if (!h.context.has_dynamic_rendering()) {
    h.cleanup();
    GTEST_SKIP() << "dynamicRendering not advertised";
  }

  constexpr std::uint32_t kSize = 256U;
  VulkanOffscreenTarget ldr;
  ASSERT_TRUE(ldr
                  .create(h.context.device(), h.context.physical_device(),
                          VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &h.allocator,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                  .is_ok());
  const VkFormat depth_format =
      VulkanRenderPass::find_supported_depth_format(h.context.physical_device());
  VulkanRenderPass present_pass;
  ASSERT_TRUE(present_pass.create(h.context.device(), VK_FORMAT_B8G8R8A8_UNORM,
                                  depth_format)
                  .is_ok());
  ASSERT_TRUE(present_pass
                  .create_depth_resources(h.context.device(),
                                          h.context.physical_device(),
                                          depth_format, kSize, kSize)
                  .is_ok());
  ASSERT_TRUE(present_pass
                  .create_framebuffers(h.context.device(), {ldr.image_view()},
                                       kSize, kSize)
                  .is_ok());

  RendererConfig config;
  config.enable_hiz = false;
  config.enable_hdr_compose = true;
  config.enable_bloom = true;
  config.enable_dynamic_rendering = true;
  config.compose_shader_dir = WARPLOOM_TEST_SHADER_DIR;

  VulkanRenderer renderer;
  const auto init =
      renderer.initialize_headless(h.context, present_pass, kSize, kSize, config,
                                   VK_FORMAT_B8G8R8A8_UNORM, ldr.image(),
                                   ldr.image_view());
  ASSERT_TRUE(init.is_ok());
  ASSERT_TRUE(renderer.hdr_compose_active());
  // Bloom stays OFF for the first frame: with exposure 1.0 and the plain
  // triangle scene this must reproduce the baked-tonemap golden hash
  // byte-for-byte (Blade stage 2 changes framing, never fragment math).

  VulkanPipeline pipeline;
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline
                  .load_shader_file(h.context.device(),
                                    shader_dir + "/triangle.vert.spv")
                  .is_ok());
  ASSERT_TRUE(pipeline
                  .load_shader_file(h.context.device(),
                                    shader_dir + "/triangle.frag.spv")
                  .is_ok());
  ASSERT_TRUE(pipeline
                  .create_graphics_pipeline(
                      h.context.device(), renderer.hdr_render_pass(),
                      renderer.hdr_format(), VK_NULL_HANDLE, true, true, false)
                  .is_ok());
  renderer.set_pipeline(pipeline.pipeline());

  auto image = renderer.begin_frame();
  ASSERT_TRUE(image.is_ok());
  ASSERT_TRUE(renderer
                  .record_commands(image.value(), present_pass.framebuffer(0U),
                                   kSize, kSize)
                  .is_ok());
  ASSERT_TRUE(renderer.submit_frame().is_ok());
  renderer.wait_idle();
  const auto readback_baked_parity = omnicpp_test::readback_swapchain_image(
      h.context.physical_device(), h.context.device(),
      h.context.graphics_queue(),
      static_cast<std::uint32_t>(h.context.queue_families().graphics_family),
      ldr.image(), VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize);
  ASSERT_GT(readback_baked_parity.non_clear_pixels, 0U)
      << "dynamic-tonemap composed frame came back empty";
  // Pixel A/B against the baked path: the golden hash pinned by
  // VulkanHardware.HeadlessComposeFrameGoldenHash (same scene, exposure,
  // size, no bloom). A dynamic tonemap that shades differently fails here.
  switch (readback_baked_parity.canonical_hash) {
    case 12508388375569283214ULL:  // NVIDIA (RTX 2060), fullscreen coverage fixed
      SUCCEED();
      break;
    default:
      ADD_FAILURE() << "dynamic tonemap diverged from the baked golden: "
                    << "canonical_hash="
                    << readback_baked_parity.canonical_hash;
      break;
  }

  // Second frame with bloom ON (dynamic bloom + dynamic tonemap together):
  // must compose non-clear with zero diagnostics.
  ASSERT_TRUE(renderer.set_bloom_enabled(true));
  auto image2 = renderer.begin_frame();
  ASSERT_TRUE(image2.is_ok());
  ASSERT_TRUE(renderer
                  .record_commands(image2.value(), present_pass.framebuffer(0U),
                                   kSize, kSize)
                  .is_ok());
  ASSERT_TRUE(renderer.submit_frame().is_ok());
  renderer.wait_idle();
  const auto readback = omnicpp_test::readback_swapchain_image(
      h.context.physical_device(), h.context.device(),
      h.context.graphics_queue(),
      static_cast<std::uint32_t>(h.context.queue_families().graphics_family),
      ldr.image(), VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize,
      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
  EXPECT_GT(readback.non_clear_pixels, 0U)
      << "dynamic-bloom composed frame came back empty";
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  renderer.cleanup(h.context.device());
  pipeline.cleanup(h.context.device());
  present_pass.cleanup(h.context.device());
  ldr.cleanup();
  h.cleanup();
}

#endif  // defined(WARPLOOM_TEST_SHADER_DIR)

#endif  // defined(WARPLOOM_HAS_VULKAN)
