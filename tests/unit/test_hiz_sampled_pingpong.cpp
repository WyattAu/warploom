//! @file test_hiz_sampled_pingpong.cpp
//! @brief Sampled-image H-Z reduction with previous-frame ping-pong.
//!
//! Each cycle renders real depth, reduces it into one of two R32F mip chains,
//! then culls against the completed opposite chain. No CPU depth values are
//! supplied to the culler. Every mip transition is explicit and validation
//! checks the descriptor layouts.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_hiz_pyramid.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"

namespace {

constexpr std::uint32_t kSize = 256U;
constexpr std::uint32_t kTile = 32U;
constexpr std::uint32_t kPyramidWidth = kSize / kTile;
constexpr std::uint32_t kLevels = 4U;
constexpr std::uint32_t kSceneCount = 3U;
constexpr std::uint32_t kCullCount = 2U;
constexpr std::uint32_t kSphereOffset = 8U;
constexpr std::uint32_t kCompactOffset = kSphereOffset + kCullCount * 4U;
constexpr std::uint32_t kCullWords = kCompactOffset + kCullCount;

std::uint32_t bits(float value) {
  std::uint32_t out = 0;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      float* m16) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m16[0] = f / aspect; m16[1] = 0.0f; m16[2] = 0.0f; m16[3] = 0.0f;
  m16[4] = 0.0f; m16[5] = f; m16[6] = 0.0f; m16[7] = 0.0f;
  m16[8] = 0.0f; m16[9] = 0.0f; m16[10] = (zfar + znear) * zn; m16[11] = -1.0f;
  m16[12] = 0.0f; m16[13] = 0.0f; m16[14] = 2.0f * znear * zfar * zn; m16[15] = 0.0f;
}

struct ScenePush {
  float view_proj[16];
  std::uint32_t data_offset;
  std::uint32_t comp_offset;
  std::uint32_t lod_scale;
  std::uint32_t pad0;
};

#ifdef WARPLOOM_HAS_VULKAN

void transition_hiz(VkCommandBuffer command_buffer, VkImage image,
                   std::uint32_t level, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkAccessFlags src_access,
                   VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                   VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = level;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);
}

#endif

} // namespace

TEST(VulkanHardware, SampledHiZPreviousFramePingPong) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppSampledHiZ", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator).is_ok());
  ASSERT_TRUE(target.create_depth(context.device(), context.physical_device(),
                                  VK_FORMAT_D32_SFLOAT).is_ok());
  if (!target.depth_is_sampleable()) {
    GTEST_SKIP() << "D32 depth is not sampleable on this device";
  }
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanHiZPyramid hiz_a;
  omnicpp::render::VulkanHiZPyramid hiz_b;
  ASSERT_TRUE(hiz_a.create(context.device(), context.physical_device(),
                           kPyramidWidth, kPyramidWidth, kLevels, &allocator).is_ok());
  ASSERT_TRUE(hiz_b.create(context.device(), context.physical_device(),
                           kPyramidWidth, kPyramidWidth, kLevels, &allocator).is_ok());

  // Identity compaction list followed by wall, front cube, back cube.
  constexpr std::uint32_t kDataOffset = kSceneCount;
  auto scene_buffer = allocator.create_buffer(
      (kSceneCount + kSceneCount * 8U) * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(scene_buffer.is_ok());
  auto* scene_words = static_cast<std::uint32_t*>(scene_buffer.value().mapped);
  ASSERT_NE(scene_words, nullptr);
  for (std::uint32_t i = 0; i < kSceneCount; ++i) scene_words[i] = i;
  const float scene[kSceneCount][8] = {
      {0.0f, 0.0f, -20.0f, 5.0f, 0.4f, 0.4f, 0.5f, 1.0f},
      {0.0f, 0.0f, -10.0f, 1.0f, 0.2f, 0.9f, 0.3f, 0.0f},
      {0.0f, 0.0f, -30.0f, 1.0f, 0.9f, 0.3f, 0.2f, 0.0f},
  };
  std::memcpy(scene_words + kDataOffset, scene, sizeof(scene));

  auto cull_buffer = allocator.create_buffer(
      kCullWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(cull_buffer.is_ok());
  auto draw_buffer = allocator.create_buffer(
      4U * sizeof(std::uint32_t),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(draw_buffer.is_ok());
  auto* cull_words = static_cast<std::uint32_t*>(cull_buffer.value().mapped);
  auto* draw_words = static_cast<std::uint32_t*>(draw_buffer.value().mapped);
  ASSERT_NE(cull_words, nullptr);
  ASSERT_NE(draw_words, nullptr);

  const auto reset_cull = [&]() {
    cull_words[0] = 0U; // accepted
    cull_words[1] = 0U; // compaction cursor
    cull_words[2] = 0U; // frustum culled
    cull_words[3] = 0U; // occlusion culled
    for (std::uint32_t i = 4U; i < kSphereOffset; ++i) cull_words[i] = 0U;
    const float spheres[kCullCount][4] = {
        {0.0f, 0.0f, -10.0f, 0.7f},
        {0.0f, 0.0f, -30.0f, 0.7f},
    };
    for (std::uint32_t i = 0; i < kCullCount; ++i) {
      for (std::uint32_t j = 0; j < 4U; ++j) {
        cull_words[kSphereOffset + i * 4U + j] = bits(spheres[i][j]);
      }
    }
    cull_words[kCompactOffset] = 0U;
    cull_words[kCompactOffset + 1U] = 0U;
    draw_words[0] = 36U;
    draw_words[1] = 0U;
    draw_words[2] = 0U;
    draw_words[3] = 0U;
  };

  omnicpp::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;

  // Scene pipeline.
  std::vector<omnicpp::render::ReflectedBinding> scene_bindings = {
      {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT}};
  auto scene_layout = descriptors.create_layout(scene_bindings, 1);
  ASSERT_TRUE(scene_layout.is_ok());
  auto scene_set = descriptors.allocate_set(scene_layout.value());
  ASSERT_TRUE(scene_set.is_ok());
  ASSERT_TRUE(descriptors.write_buffer(scene_set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       scene_buffer.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  const VkPushConstantRange scene_push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ScenePush)};
  omnicpp::render::VulkanPipeline scene_pipeline;
  ASSERT_TRUE(scene_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(scene_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/gpu_objects.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(scene_pipeline.create_pipeline_layout(
      context.device(), &scene_layout.value(), 1, &scene_push_range).is_ok());
  ASSERT_TRUE(scene_pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      scene_pipeline.pipeline_layout(), true, true, true).is_ok());

  // Reduction layout and two descriptor-set families: one family writes A,
  // the other writes B. Binding 2 is the preceding single-mip view; at level
  // 0 it points at depth because that binding is not read in that branch.
  std::vector<omnicpp::render::ReflectedBinding> reduce_bindings = {
      {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT},
      {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT},
      {0, 2, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT},
  };
  auto reduce_layout = descriptors.create_layout(reduce_bindings, 2U * kLevels);
  ASSERT_TRUE(reduce_layout.is_ok());

  const auto make_reduce_sets = [&](omnicpp::render::VulkanHiZPyramid& destination)
      -> std::vector<VkDescriptorSet> {
    std::vector<VkDescriptorSet> sets;
    for (std::uint32_t level = 0; level < kLevels; ++level) {
      auto set = descriptors.allocate_set(reduce_layout.value());
      if (!set.is_ok()) {
        ADD_FAILURE() << "Could not allocate reduction descriptor set";
        return {};
      }
      if (!descriptors.write_image(
              set.value(), 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              destination.sampler(), target.depth_view(),
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).is_ok() ||
          !descriptors.write_image(
              set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
              destination.mip_view(level), VK_IMAGE_LAYOUT_GENERAL).is_ok() ||
          !descriptors.write_image(
              set.value(), 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              destination.sampler(), level == 0U ? target.depth_view()
                                                 : destination.mip_view(level - 1U),
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).is_ok()) {
        ADD_FAILURE() << "Could not write reduction descriptor set";
        return {};
      }
      sets.push_back(set.value());
    }
    return sets;
  };
  const auto reduce_sets_a = make_reduce_sets(hiz_a);
  const auto reduce_sets_b = make_reduce_sets(hiz_b);
  const VkPushConstantRange reduce_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16U};
  omnicpp::render::VulkanPipeline reduce_pipeline;
  ASSERT_TRUE(reduce_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/depth_reduce_image.comp.spv", "compute").is_ok());
  ASSERT_TRUE(reduce_pipeline.create_pipeline_layout(
      context.device(), &reduce_layout.value(), 1, &reduce_push).is_ok());
  ASSERT_TRUE(reduce_pipeline.create_compute_pipeline(
      context.device(), reduce_pipeline.pipeline_layout()).is_ok());

  // Culler has one descriptor set per completed pyramid so switching the
  // previous-frame source never mutates a descriptor in flight.
  std::vector<omnicpp::render::ReflectedBinding> cull_bindings = {
      {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT},
      {0, 1, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT},
      {0, 2, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT},
  };
  auto cull_layout = descriptors.create_layout(cull_bindings, 2);
  ASSERT_TRUE(cull_layout.is_ok());
  const auto make_cull_set = [&](omnicpp::render::VulkanHiZPyramid& previous)
      -> VkDescriptorSet {
    auto set = descriptors.allocate_set(cull_layout.value());
    if (!set.is_ok()) {
      ADD_FAILURE() << "Could not allocate cull descriptor set";
      return VK_NULL_HANDLE;
    }
    if (!descriptors.write_buffer(
            set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            cull_buffer.value().buffer, 0, VK_WHOLE_SIZE).is_ok() ||
        !descriptors.write_image(
            set.value(), 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            previous.sampler(), previous.view(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).is_ok() ||
        !descriptors.write_buffer(
            set.value(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            draw_buffer.value().buffer, 0, VK_WHOLE_SIZE).is_ok()) {
      ADD_FAILURE() << "Could not write cull descriptor set";
      return VK_NULL_HANDLE;
    }
    return set.value();
  };
  const VkDescriptorSet cull_set_a = make_cull_set(hiz_a);
  const VkDescriptorSet cull_set_b = make_cull_set(hiz_b);
  const VkPushConstantRange cull_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 44U};
  omnicpp::render::VulkanPipeline cull_pipeline;
  ASSERT_TRUE(cull_pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/cull_hiz_sampled.comp.spv", "compute").is_ok());
  ASSERT_TRUE(cull_pipeline.create_pipeline_layout(
      context.device(), &cull_layout.value(), 1, &cull_push).is_ok());
  ASSERT_TRUE(cull_pipeline.create_compute_pipeline(
      context.device(), cull_pipeline.pipeline_layout()).is_ok());

  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
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

  float matrix[16];
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, matrix);
  ScenePush scene_push{};
  std::memcpy(scene_push.view_proj, matrix, sizeof(matrix));
  scene_push.data_offset = kDataOffset;
  scene_push.comp_offset = 0U;
  scene_push.lod_scale = bits(1.0f);

  bool depth_initialized = false;
  bool hiz_a_initialized = false;
  bool hiz_b_initialized = false;

  const auto record_frame = [&](bool destination_is_a, bool run_cull,
                               bool previous_is_a) {
    auto& destination = destination_is_a ? hiz_a : hiz_b;
    const auto& reduce_sets = destination_is_a ? reduce_sets_a : reduce_sets_b;
    const bool destination_initialized = destination_is_a ? hiz_a_initialized : hiz_b_initialized;
    reset_cull();
    ASSERT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);

    if (run_cull) {
      VkBufferMemoryBarrier host_ready[2]{};
      for (auto& barrier : host_ready) {
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.size = VK_WHOLE_SIZE;
      }
      host_ready[0].buffer = cull_buffer.value().buffer;
      host_ready[1].buffer = draw_buffer.value().buffer;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 2, host_ready, 0, nullptr);
    }

    // Render real wall/cube depth for this frame.
    if (depth_initialized) {
      VkImageMemoryBarrier depth_to_attachment{};
      depth_to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      depth_to_attachment.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      depth_to_attachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      depth_to_attachment.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      depth_to_attachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      depth_to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      depth_to_attachment.image = target.depth_image();
      depth_to_attachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      depth_to_attachment.subresourceRange.levelCount = 1;
      depth_to_attachment.subresourceRange.layerCount = 1;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &depth_to_attachment);
    }

    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = target.render_pass();
    render_begin.framebuffer = target.framebuffer();
    render_begin.renderArea.extent = {kSize, kSize};
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    render_begin.clearValueCount = 2;
    render_begin.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(kSize),
                        static_cast<float>(kSize), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {kSize, kSize}};
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipeline.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            scene_pipeline.pipeline_layout(), 0, 1,
                            &scene_set.value(), 0, nullptr);
    vkCmdPushConstants(cb, scene_pipeline.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(scene_push), &scene_push);
    vkCmdDraw(cb, 36U, kSceneCount, 0, 0);
    vkCmdEndRenderPass(cb);
    depth_initialized = true;

    // The reduction samples the actual depth attachment, then writes one
    // destination mip at a time. The preceding level becomes read-only before
    // the next dispatch; no inter-workgroup assumption is made.
    VkImageMemoryBarrier depth_to_sample{};
    depth_to_sample.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depth_to_sample.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depth_to_sample.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth_to_sample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_to_sample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_to_sample.image = target.depth_image();
    depth_to_sample.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_to_sample.subresourceRange.levelCount = 1;
    depth_to_sample.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &depth_to_sample);

    const std::uint32_t dims[kLevels] = {8U, 4U, 2U, 1U};
    for (std::uint32_t level = 0; level < kLevels; ++level) {
      transition_hiz(
          cb, destination.image(), level,
          destination_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_GENERAL,
          destination_initialized ? VK_ACCESS_SHADER_READ_BIT
                                   : static_cast<VkAccessFlags>(0),
          VK_ACCESS_SHADER_WRITE_BIT,
          destination_initialized ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      if (level > 0U) {
        transition_hiz(cb, destination.image(), level - 1U,
                       VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      }
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, reduce_pipeline.pipeline());
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                              reduce_pipeline.pipeline_layout(), 0, 1,
                              &reduce_sets[level], 0, nullptr);
      const std::uint32_t pc[4] = {kSize, kSize, kTile, level};
      vkCmdPushConstants(cb, reduce_pipeline.pipeline_layout(),
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
      vkCmdDispatch(cb, (dims[level] + 7U) / 8U,
                    (dims[level] + 7U) / 8U, 1);
    }
    transition_hiz(cb, destination.image(), kLevels - 1U,
                   VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    if (destination_is_a) hiz_a_initialized = true;
    else hiz_b_initialized = true;

    if (run_cull) {
      const VkDescriptorSet previous_set = previous_is_a ? cull_set_a : cull_set_b;
      const std::uint32_t cull_pc[11] = {
          kSphereOffset, kCompactOffset, kPyramidWidth, kPyramidWidth,
          kCullCount, bits(0.1f), bits(100.0f), kSize, kSize, kTile, kLevels};
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline.pipeline());
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                              cull_pipeline.pipeline_layout(), 0, 1,
                              &previous_set, 0, nullptr);
      vkCmdPushConstants(cb, cull_pipeline.pipeline_layout(),
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cull_pc), cull_pc);
      vkCmdDispatch(cb, 1, 1, 1);
      VkBufferMemoryBarrier result_ready[2]{};
      for (auto& barrier : result_ready) {
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.size = VK_WHOLE_SIZE;
      }
      result_ready[0].buffer = cull_buffer.value().buffer;
      result_ready[1].buffer = draw_buffer.value().buffer;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0,
                           0, nullptr, 2, result_ready, 0, nullptr);
    }
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  };

  const auto submit = [&]() {
    ASSERT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cb;
    ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &submit_info, fence), VK_SUCCESS);
    ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
  };

  // Frame 0 builds A only. There is no previous-frame pyramid yet.
  record_frame(true, false, false);
  submit();

  // Frame 1 builds B while culling samples completed A.
  record_frame(false, true, true);
  submit();
  EXPECT_EQ(cull_words[0], 1U);
  EXPECT_EQ(cull_words[2], 0U);
  EXPECT_EQ(cull_words[3], 1U);
  EXPECT_EQ(cull_words[kCompactOffset], 0U);
  EXPECT_EQ(draw_words[1], 1U);

  // Frame 2 rebuilds A while culling samples completed B. This is the actual
  // ping-pong proof: neither reduction nor culling reads a currently-written
  // pyramid image.
  record_frame(true, true, false);
  submit();
  EXPECT_EQ(cull_words[0], 1U);
  EXPECT_EQ(cull_words[2], 0U);
  EXPECT_EQ(cull_words[3], 1U);
  EXPECT_EQ(cull_words[kCompactOffset], 0U);
  EXPECT_EQ(draw_words[1], 1U);

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  cull_pipeline.cleanup(context.device());
  reduce_pipeline.cleanup(context.device());
  scene_pipeline.cleanup(context.device());
  hiz_b.cleanup(context.device());
  hiz_a.cleanup(context.device());
  target.cleanup(context.device());
  auto scene_allocation = scene_buffer.value();
  auto cull_allocation = cull_buffer.value();
  auto draw_allocation = draw_buffer.value();
  allocator.destroy_allocation(scene_allocation);
  allocator.destroy_allocation(cull_allocation);
  allocator.destroy_allocation(draw_allocation);
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
