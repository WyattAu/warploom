//! @file test_mesh_lod.cpp
//! @brief GPU E2E tests for the LOD selection compute pass. Three instances
//! at different distances must select different LOD levels purely from their
//! projected size, an off-frustum sphere must be culled, and the visible
//! count must match exactly — all computed on the GPU and verified by
//! reading the selection buffer back.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_pipeline.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

constexpr std::uint32_t kInstanceCount = 4;
constexpr std::uint32_t kSphereOffset = 26U;
constexpr std::uint32_t kResultOffset = kSphereOffset + 4U * kInstanceCount;
constexpr std::uint32_t kCullWords = kResultOffset + 2U * kInstanceCount;

// Mirrors the shader's push-constant block (std430-ish packing; all scalars
// and the trailing float array land on 4-byte words).
struct LodPush {
  std::uint32_t sphere_offset;
  std::uint32_t result_offset;
  float tan_half_fov;
  float viewport_h;
  float near_z;
  float far_margin;
  std::uint32_t threshold_count;
  std::uint32_t pad0;
  float thresholds[8];
  float camera_position[4];
  float forward[4];
};

}  // namespace

//! Three instances at 8 / 32 / 128 units with equal radius 1 must select
//! LOD 0 / 1 / 2 respectively; the sphere behind the camera is culled and
//! the GPU-visible count is exactly 3.
TEST(VulkanHardware, LodSelectByDistance) {
#ifdef WARPLOOM_TEST_SHADER_DIR
  if (!warploom::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  warploom::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("WarploomLodTest", true).is_ok());
  warploom::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device()).is_ok());
  warploom::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  // Words: [0..1] header, [2..25] frustum, spheres, results.
  auto lod_buf = allocator.create_buffer(
      kCullWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(lod_buf.is_ok());
  auto* words = static_cast<std::uint32_t*>(lod_buf.value().mapped);

  // Camera at origin looking down -Z, fov 60, viewport 720.
  const float tan_half = 0.57735026f;
  const float aspect = 1.0f;
  const float near_z = 0.1f;
  const float half_h = tan_half * near_z;
  const float half_w = half_h * aspect;
  // 6 planes with camera at origin looking down -Z. Inside = +d:
  // right plane:  -x + half_w >= 0  ->  (-1,0,0, half_w)
  // left plane:    x + half_w >= 0  ->  ( 1,0,0, half_w)
  // top plane:    -y + half_h >= 0  ->  (0,-1,0, half_h)
  // bottom plane:  y + half_h >= 0  ->  (0, 1,0, half_h)
  // near plane (z = -near_z, front = z <= -near_z):  (0,0,-1, near_z)
  // far plane  (z = -100, front = z >= -100):        (0,0, 1, 100)
  const float planes[6][4] = {
      {-1, 0, 0, half_w}, {1, 0, 0, half_w}, {0, -1, 0, half_h},
      {0, 1, 0, half_h},  {0, 0, -1, near_z}, {0, 0, 1, 100.0f},
  };

  // Spheres: three in-frustum at increasing depth, one behind the camera.
  // Projected size = (r/depth) * (720 / (2*tan30°)) = 623/depth px:
  //   depth 2  -> 311 px >= 200 -> LOD 0
  //   depth 8  ->  78 px >=  40 -> LOD 1
  //   depth 64 -> 9.7 px >=   8 -> LOD 2
  const float spheres[kInstanceCount][4] = {
      {0.f, 0.f, -2.f, 1.f},
      {0.f, 0.f, -8.f, 1.f},
      {0.f, 0.f, -64.f, 1.f},
      {0.f, 0.f, 10.f, 1.f},
  };

  words[0] = kInstanceCount;
  words[1] = 0U;
  {
    float plane_words[24];
    std::memcpy(plane_words, planes, sizeof(plane_words));
    std::uint32_t pw[24];
    std::memcpy(pw, plane_words, sizeof(pw));
    for (std::size_t i = 0; i < static_cast<std::size_t>(24); ++i) words[2 + i] = pw[i];
  }
  for (std::uint32_t i = 0; i < kInstanceCount; ++i) {
    std::uint32_t sw[4];
    std::memcpy(sw, spheres[i], sizeof(sw));
    for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) words[kSphereOffset + i * 4U + k] = sw[k];
  }
  for (std::uint32_t i = 0; i < 2U * kInstanceCount; ++i)
    words[kResultOffset + i] = 0xDEADBEEFU;  // poison: shader must overwrite

  // --- Layout + pipeline. ---
  std::vector<warploom::render::ReflectedBinding> bindings(1);
  bindings[0].binding = 0;
  bindings[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].count = 1;
  bindings[0].stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
  auto layout_res = manager.create_layout(bindings, 8);
  ASSERT_TRUE(layout_res.is_ok());
  auto set_res = manager.allocate_set(layout_res.value());
  ASSERT_TRUE(set_res.is_ok());
  ASSERT_TRUE(manager
                  .write_buffer(set_res.value(), 0,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                lod_buf.value().buffer, 0, VK_WHOLE_SIZE)
                  .is_ok());

  warploom::render::VulkanPipeline pipeline;
  ASSERT_TRUE(pipeline
                  .load_shader_stage_file(context.device(),
                                          WARPLOOM_TEST_SHADER_DIR
                                              "/lod_select.comp.spv",
                                          "compute")
                  .is_ok());
  // Push constants need a layout with a range — build it once, correctly.
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LodPush)};
  ASSERT_TRUE(
      pipeline
          .create_pipeline_layout(
              context.device(),
              std::vector<VkDescriptorSetLayout>{layout_res.value()}.data(), 1,
              &range)
          .is_ok());
  ASSERT_TRUE(
      pipeline.create_compute_pipeline(context.device(), pipeline.pipeline_layout())
          .is_ok());

  // Thresholds: LOD0 for screen >= 200px, LOD1 >= 40px, LOD2 >= 8px.
  LodPush push{};
  push.sphere_offset = kSphereOffset;
  push.result_offset = kResultOffset;
  push.tan_half_fov = tan_half;
  push.viewport_h = 720.f;
  push.near_z = near_z;
  push.far_margin = 2.0f;
  push.threshold_count = 3;
  push.pad0 = 0;
  const float thresholds[3] = {200.f, 40.f, 8.f};
  std::memcpy(push.thresholds, thresholds, sizeof(thresholds));
  // Camera at origin looking down -Z (matches the plane set above).
  push.camera_position[0] = 0.f;
  push.camera_position[1] = 0.f;
  push.camera_position[2] = 0.f;
  push.camera_position[3] = 0.f;
  push.forward[0] = 0.f;
  push.forward[1] = 0.f;
  push.forward[2] = -1.f;
  push.forward[3] = 0.f;

  // --- Dispatch. ---
  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.queueFamilyIndex =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);
  VkCommandPool pool;  ASSERT_EQ(vkCreateCommandPool(context.device(), &pci, nullptr, &pool),
            VK_SUCCESS);
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb;
  ASSERT_EQ(vkAllocateCommandBuffers(context.device(), &ai, &cb), VK_SUCCESS);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline());
  VkDescriptorSet set = set_res.value();
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.pipeline_layout(), 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cb, pipeline.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(cb, (kInstanceCount + 63U) / 64U, 1, 1);
  vkEndCommandBuffer(cb);
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;
  ASSERT_EQ(vkCreateFence(context.device(), &fi, nullptr, &fence), VK_SUCCESS);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX),
            VK_SUCCESS);

  // --- Readback + assertions. ---
  auto lod_alloc = lod_buf.value();
  const std::uint32_t visible = words[1];
  EXPECT_EQ(visible, 3U) << "expected exactly 3 visible instances";

  struct Sel {
    std::uint32_t lod;
    std::uint32_t vis;
  };
  const Sel* sel = reinterpret_cast<const Sel*>(words + kResultOffset);
  EXPECT_EQ(sel[0].vis, 1U);
  EXPECT_EQ(sel[0].lod, 0U) << "nearest instance should use LOD 0";
  EXPECT_EQ(sel[1].vis, 1U);
  EXPECT_EQ(sel[1].lod, 1U) << "mid instance should use LOD 1";
  EXPECT_EQ(sel[2].vis, 1U);
  EXPECT_EQ(sel[2].lod, 2U) << "far instance should use LOD 2";
  EXPECT_EQ(sel[3].vis, 0U) << "behind-camera instance must be culled";

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  pipeline.cleanup(context.device());
  manager.cleanup();
  allocator.destroy_allocation(lod_alloc);
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "shader dir unavailable";
#endif
}

//! A host count above the shader's threshold capacity must not read past
//! the array. thresholds[8] aliases camera_position[0] in the push block
//! (both 0.0 here): unclamped, a 6.5 px sphere below every real threshold
//! still satisfies `6.5 >= 0.0` at t=8, so the shader reports visible with
//! a bogus LOD 8. Clamped, the sentinel survives and the sphere is culled.
TEST(VulkanHardware, LodSelectClampsThresholdCountToArrayBound) {
#ifdef WARPLOOM_TEST_SHADER_DIR
  if (!warploom::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  warploom::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("WarploomLodClampTest", true).is_ok());
  warploom::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device()).is_ok());
  warploom::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());

  constexpr std::uint32_t kN = 1;
  constexpr std::uint32_t kSphOff = 26U;
  constexpr std::uint32_t kResOff = kSphOff + 4U * kN;
  constexpr std::uint32_t kWords = kResOff + 2U * kN;
  auto lod_buf = allocator.create_buffer(
      kWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(lod_buf.is_ok());
  auto* words = static_cast<std::uint32_t*>(lod_buf.value().mapped);

  const float tan_half = 0.57735026f;
  const float near_z = 0.1f;
  const float half_h = tan_half * near_z;
  const float planes[6][4] = {
      {-1, 0, 0, half_h}, {1, 0, 0, half_h}, {0, -1, 0, half_h},
      {0, 1, 0, half_h},  {0, 0, -1, near_z}, {0, 0, 1, 100.0f},
  };
  // Depth 96, radius 1 -> (1/96) * (720 / 2*tan30) ~= 6.5 px, below the
  // smallest real threshold (8 px) but above the aliased 0.0 at index 8.
  const float sphere[4] = {0.f, 0.f, -96.f, 1.f};

  words[0] = kN;
  words[1] = 0U;
  {
    float plane_words[24];
    std::memcpy(plane_words, planes, sizeof(plane_words));
    std::uint32_t pw[24];
    std::memcpy(pw, plane_words, sizeof(pw));
    for (std::size_t i = 0; i < static_cast<std::size_t>(24); ++i) words[2 + i] = pw[i];
  }
  {
    std::uint32_t sw[4];
    std::memcpy(sw, sphere, sizeof(sw));
    for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k)
      words[kSphOff + k] = sw[k];
  }
  words[kResOff + 0] = 0xDEADBEEFU;
  words[kResOff + 1] = 0xDEADBEEFU;

  std::vector<warploom::render::ReflectedBinding> bindings(1);
  bindings[0].binding = 0;
  bindings[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].count = 1;
  bindings[0].stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;
  auto layout_res = manager.create_layout(bindings, 8);
  ASSERT_TRUE(layout_res.is_ok());
  auto set_res = manager.allocate_set(layout_res.value());
  ASSERT_TRUE(set_res.is_ok());
  ASSERT_TRUE(manager
                  .write_buffer(set_res.value(), 0,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                lod_buf.value().buffer, 0, VK_WHOLE_SIZE)
                  .is_ok());

  warploom::render::VulkanPipeline pipeline;
  ASSERT_TRUE(pipeline
                  .load_shader_stage_file(context.device(),
                                          WARPLOOM_TEST_SHADER_DIR
                                              "/lod_select.comp.spv",
                                          "compute")
                  .is_ok());
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LodPush)};
  ASSERT_TRUE(
      pipeline
          .create_pipeline_layout(
              context.device(),
              std::vector<VkDescriptorSetLayout>{layout_res.value()}.data(), 1,
              &range)
          .is_ok());
  ASSERT_TRUE(
      pipeline.create_compute_pipeline(context.device(), pipeline.pipeline_layout())
          .is_ok());

  LodPush push{};
  push.sphere_offset = kSphOff;
  push.result_offset = kResOff;
  push.tan_half_fov = tan_half;
  push.viewport_h = 720.f;
  push.near_z = near_z;
  push.far_margin = 2.0f;
  push.threshold_count = 10U;  // hostile: capacity is 8
  push.pad0 = 0;
  const float thresholds[8] = {200.f, 150.f, 100.f, 80.f,
                               60.f, 40.f, 20.f, 8.f};
  std::memcpy(push.thresholds, thresholds, sizeof(thresholds));
  push.camera_position[0] = 0.f;
  push.camera_position[1] = 0.f;
  push.camera_position[2] = 0.f;
  push.camera_position[3] = 0.f;
  push.forward[0] = 0.f;
  push.forward[1] = 0.f;
  push.forward[2] = -1.f;
  push.forward[3] = 0.f;

  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.queueFamilyIndex =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);
  VkCommandPool pool;
  ASSERT_EQ(vkCreateCommandPool(context.device(), &pci, nullptr, &pool),
            VK_SUCCESS);
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cb;
  ASSERT_EQ(vkAllocateCommandBuffers(context.device(), &ai, &cb), VK_SUCCESS);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline());
  VkDescriptorSet set = set_res.value();
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline.pipeline_layout(), 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cb, pipeline.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  vkCmdDispatch(cb, 1, 1, 1);
  vkEndCommandBuffer(cb);
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;
  ASSERT_EQ(vkCreateFence(context.device(), &fi, nullptr, &fence), VK_SUCCESS);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX),
            VK_SUCCESS);

  EXPECT_EQ(words[1], 0U) << "nothing may be counted visible";
  const std::uint32_t out_lod = words[kResOff + 0];
  const std::uint32_t out_vis = words[kResOff + 1];
  EXPECT_EQ(out_vis, 0U) << "6.5 px is below every real threshold: culled";
  EXPECT_EQ(out_lod, 0U) << "culled instances must not carry LOD 8";

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  pipeline.cleanup(context.device());
  manager.cleanup();
  auto lod_alloc = lod_buf.value();
  allocator.destroy_allocation(lod_alloc);
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "shader dir unavailable";
#endif
}

#endif  // WARPLOOM_HAS_VULKAN
