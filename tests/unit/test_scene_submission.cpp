//! @file test_scene_submission.cpp
//! @brief End-to-end coverage for VulkanRenderer::record_scene().

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_scene.hpp"
#include "engine/core/ecs.hpp"

#ifdef WARPLOOM_HAS_VULKAN
#include "vulkan_test_readback.hpp"
#endif

namespace {

using omnicpp::render::SceneMatrix;

void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      SceneMatrix& m) {
  m.fill(0.0f);
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = zfar * zn;
  m[11] = -1.0f;
  m[14] = znear * zfar * zn;
}

void make_translation(float x, float y, float z, SceneMatrix& m) {
  m.fill(0.0f);
  m[0] = m[5] = m[10] = m[15] = 1.0f;
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

void make_scale_rotation_y_translation(float angle, float x, float y, float z,
                                        SceneMatrix& m) {
  m.fill(0.0f);
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  m[0] = c;
  m[2] = -s;
  m[5] = 1.0f;
  m[8] = s;
  m[10] = c;
  m[12] = x;
  m[13] = y;
  m[14] = z;
  m[15] = 1.0f;
}

//! Write a cube in the canonical eleven-float layout: position.xyz,
//! color.rgb, normal.xyz (radial, so every face is lit from any direction),
//! uv.xy.
void write_cube(float* vertices, float r, float g, float b) {
  constexpr float positions[8][3] = {
      {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
      {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
      {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}};
  for (std::uint32_t i = 0; i < 8U; ++i) {
    float* v = vertices + i * 11U;
    v[0] = positions[i][0];
    v[1] = positions[i][1];
    v[2] = positions[i][2];
    v[3] = r;
    v[4] = g;
    v[5] = b;
    const float length =
        std::sqrt(positions[i][0] * positions[i][0] +
                  positions[i][1] * positions[i][1] +
                  positions[i][2] * positions[i][2]);
    v[6] = positions[i][0] / length;
    v[7] = positions[i][1] / length;
    v[8] = positions[i][2] / length;
    v[9] = 0.0f;
    v[10] = 0.0f;
  }
}

constexpr std::uint32_t kCubeIndices[36] = {
    0, 3, 1, 1, 3, 2,  // -z
    4, 5, 7, 5, 6, 7,  // +z
    0, 1, 4, 1, 5, 4,  // -y
    3, 7, 2, 2, 7, 6,  // +y
    0, 4, 3, 3, 4, 7,  // -x
    1, 2, 5, 2, 6, 5}; // +x

}  // namespace

TEST(VulkanHardware, RendererIndexedSceneSubmission) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppSceneSubmission", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  constexpr VkDeviceSize kVertexBytes = 8U * 11U * sizeof(float);
  constexpr VkDeviceSize kIndexBytes = sizeof(kCubeIndices);
  auto red_vertices = allocator.create_buffer(
      kVertexBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto green_vertices = allocator.create_buffer(
      kVertexBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto indices = allocator.create_buffer(
      kIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(red_vertices.is_ok());
  ASSERT_TRUE(green_vertices.is_ok());
  ASSERT_TRUE(indices.is_ok());
  write_cube(static_cast<float*>(red_vertices.value().mapped), 1.0f, 0.05f, 0.05f);
  write_cube(static_cast<float*>(green_vertices.value().mapped), 0.05f, 1.0f, 0.05f);
  std::memcpy(indices.value().mapped, kCubeIndices, kIndexBytes);

  omnicpp::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  const std::vector<omnicpp::render::ReflectedBinding> bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT}};
  auto layout = descriptors.create_layout(bindings, 2U);
  ASSERT_TRUE(layout.is_ok());
  auto red_set = descriptors.allocate_set(layout.value());
  auto green_set = descriptors.allocate_set(layout.value());
  ASSERT_TRUE(red_set.is_ok());
  ASSERT_TRUE(green_set.is_ok());
  ASSERT_TRUE(descriptors.write_buffer(
      red_set.value(), 0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      red_vertices.value().buffer, 0U, VK_WHOLE_SIZE).is_ok());
  ASSERT_TRUE(descriptors.write_buffer(
      green_set.value(), 0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      green_vertices.value().buffer, 0U, VK_WHOLE_SIZE).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, 256U, 256U, &allocator).is_ok());
  ASSERT_TRUE(target.create_depth(context.device(), context.physical_device(),
                                  VK_FORMAT_D32_SFLOAT).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());

  omnicpp::render::VulkanPipeline pipeline;
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/indexed_scene.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipeline.load_shader_stage_file(
      context.device(), shader_dir + "/indexed_scene.frag.spv", "fragment").is_ok());
  const VkPushConstantRange push_range{
      VK_SHADER_STAGE_VERTEX_BIT, 0U, 128U};
  ASSERT_TRUE(pipeline.create_pipeline_layout(
      context.device(), &layout.value(), 1U, &push_range).is_ok());
  ASSERT_TRUE(pipeline.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      pipeline.pipeline_layout(), true, true, false).is_ok());

  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool_result.value());
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer cb = cb_result.value();
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);

  omnicpp::render::SceneMesh red_mesh{
      red_vertices.value().buffer, indices.value().buffer, red_set.value(), 0U, 36U};
  omnicpp::render::SceneMesh green_mesh{
      green_vertices.value().buffer, indices.value().buffer, green_set.value(), 0U, 36U};
  omnicpp::render::VulkanRenderer renderer;
  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto red_entity = world.create_entity();
  const auto green_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f,
                   camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(
      camera_entity, camera_component);
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      red_entity, {&red_mesh, true});
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      green_entity, {&green_mesh, true});
  omnicpp::render::SceneTransformComponent red_transform;
  omnicpp::render::SceneTransformComponent green_transform;
  make_translation(0.0f, 0.0f, -4.0f, red_transform.model);
  make_translation(0.0f, 0.0f, -8.0f, green_transform.model);
  world.add_component<omnicpp::render::SceneTransformComponent>(
      red_entity, red_transform);
  world.add_component<omnicpp::render::SceneTransformComponent>(
      green_entity, green_transform);
  auto scene = omnicpp::render::extract_vulkan_scene(
      world, pipeline.pipeline(), pipeline.pipeline_layout());

  const auto render = [&](const omnicpp::render::VulkanScene& snapshot)
      -> omnicpp_test::ReadbackResult {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {256U, 256U};
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0U};
    rb.clearValueCount = 2U;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    EXPECT_TRUE(renderer.record_scene(cb, snapshot, 256U, 256U).is_ok());
    vkCmdEndRenderPass(cb);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    EXPECT_EQ(vkResetFences(context.device(), 1U, &fence), VK_SUCCESS);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(context.graphics_queue(), 1U, &submit, fence), VK_SUCCESS);
    EXPECT_EQ(vkWaitForFences(context.device(), 1U, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
    return omnicpp_test::readback_swapchain_image(
        context.physical_device(), context.device(), context.graphics_queue(),
        static_cast<std::uint32_t>(context.queue_families().graphics_family),
        target.image(), target.format(), 256U, 256U,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  };

  const auto first = render(scene);
  ASSERT_TRUE(first.submitted);
  EXPECT_GT(first.red_dominant_pixels, 5000U);
  EXPECT_EQ(first.green_dominant_pixels, 0U);

  make_translation(0.0f, 0.0f, -8.0f,
                   world.get_component<omnicpp::render::SceneTransformComponent>(red_entity).model);
  make_scale_rotation_y_translation(
      0.55f, 0.0f, 0.0f, -4.0f,
      world.get_component<omnicpp::render::SceneTransformComponent>(green_entity).model);
  scene = omnicpp::render::extract_vulkan_scene(
      world, pipeline.pipeline(), pipeline.pipeline_layout());
  const auto second = render(scene);
  ASSERT_TRUE(second.submitted);
  EXPECT_EQ(second.red_dominant_pixels, 0U);
  EXPECT_GT(second.green_dominant_pixels, 2500U);
  EXPECT_NE(first.hash, second.hash);

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool_result.value(), nullptr);
  pipeline.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation rv = red_vertices.value();
  omnicpp::render::Allocation gv = green_vertices.value();
  omnicpp::render::Allocation ib = indices.value();
  allocator.destroy_allocation(rv);
  allocator.destroy_allocation(gv);
  allocator.destroy_allocation(ib);
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
