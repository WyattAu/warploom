//! @file test_ray_query_first_contact.cpp
//! @brief E1 GPU proof: acceleration-structure builders + ray queries.
//!
//! Builds two box BLASes and a TLAS with two instances, then dispatches a
//! ray-query compute shader over an 8x8 grid of +Z rays from z=-4 and
//! asserts the committed instance custom index per ray:
//!   - Instance A (custom 10): identity transform, footprint x in
//!     [-1.05,-0.15], y in [-0.8,0.8]  -> all 8 rows of columns 0..3 hit.
//!   - Instance B (custom 20): authored hx=1.2/hz=0.45 box, rotated 90
//!     degrees about Y and translated x=+0.6, footprint x in [0.15,1.05],
//!     y in [-0.35,0.35] -> only the |oy|<=0.35 rows of columns 4..7 hit.
//!     The rotation is load-bearing: without it B's wide axis would overlap
//!     A's corridor and every expectation would be ambiguous.
//!   - The remaining rays (outer rows of columns 4..7) miss everything.
//! Exact expected totals: 32 hits on A, 16 hits on B, 16 misses.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "engine/render/vulkan_acceleration_structure.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_rt_query.hpp"

#if defined(OMNICPP_HAS_VULKAN) && defined(OMNICPP_TEST_SHADER_DIR)

namespace {

using omnicpp::render::BlasBuildInput;
using omnicpp::render::TlasInstance;

constexpr std::uint32_t kGrid = 8U;

//! Axis-aligned box, 12 triangles, 9 floats per vertex, CCW outward.
std::vector<float> make_box_triangles(float hx, float hy, float hz) {
  const float x = hx, y = hy, z = hz;
  const float c[8][3] = {
      {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z},      // front  (+Z)
      {x, -y, -z}, {-x, -y, -z}, {-x, y, -z}, {x, y, -z},  // back   (-Z)
  };
  const std::uint32_t t[12][3] = {
      {0, 1, 2}, {0, 2, 3},  // +Z
      {4, 5, 6}, {4, 6, 7},  // -Z
      {1, 4, 7}, {1, 7, 2},  // +X
      {5, 0, 3}, {5, 3, 6},  // -X
      {3, 2, 7}, {3, 7, 6},  // +Y
      {5, 4, 1}, {5, 1, 0},  // -Y
  };
  std::vector<float> out;
  out.reserve(12U * 9U);
  for (const auto& tri : t) {
    for (const auto vi : tri) {
      out.push_back(c[vi][0]);
      out.push_back(c[vi][1]);
      out.push_back(c[vi][2]);
    }
  }
  return out;
}

//! Row-major 3x4 Vulkan instance transform. Rotation of a point p is
//! p' = R*p + t with R = rotY(90 deg) (x -> -z, z -> x).
//! Row-major rows: (0,0,-1,t), (0,1,0,0), (1,0,0,0).
std::array<float, 12> rotated_translated_instance(float tx, float ty,
                                                  float tz) {
  return {0.0f, 0.0f, -1.0f, tx, 0.0f, 1.0f, 0.0f, ty,
          1.0f, 0.0f, 0.0f,  tz};
}

std::array<float, 12> translated_instance(float tx, float ty, float tz) {
  return {1.0f, 0.0f, 0.0f, tx, 0.0f, 1.0f, 0.0f, ty,
          0.0f, 0.0f, 1.0f, tz};
}

std::array<float, 12> identity_instance() {
  return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f};
}

}  // namespace

TEST(ray_query_first_contact, blas_tlas_build_and_ray_query) {
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppRayQueryFirst", true).is_ok());
  if (!context.has_ray_tracing()) {
    GTEST_SKIP() << "device lacks ray tracing";
  }

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device()).is_ok());
  omnicpp::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);

  // ---- SBT sizing rules (E3 groundwork sanity) ---------------------------
  omnicpp::render::RtSbtProperties sbt{};
  omnicpp::render::VulkanRtQuery::get_properties(context.physical_device(),
                                                 sbt);
  EXPECT_GE(sbt.handle_size, 16U);
  EXPECT_EQ(sbt.raygen_stride % 32U, 0U);

  // ---- Geometry ------------------------------------------------------------
  // A authored hx=0.45, placed at x=-0.6 -> footprint x in [-1.05,-0.15].
  const std::vector<float> box_a = make_box_triangles(0.45f, 0.8f, 0.5f);
  // B: authored hx=1.2 (wide), hz=0.45 (narrow); the 90-degree instance
  // rotation is what narrows its x footprint to [0.6-0.45, 0.6+0.45].
  const std::vector<float> box_b = make_box_triangles(1.2f, 0.35f, 0.45f);
  ASSERT_EQ(box_a.size(), 12U * 9U);
  ASSERT_EQ(box_b.size(), 12U * 9U);

  auto geom_a = allocator.create_buffer(
      box_a.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto geom_b = allocator.create_buffer(
      box_b.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geom_a.is_ok() && geom_b.is_ok());
  std::memcpy(geom_a.value().mapped, box_a.data(), box_a.size() * sizeof(float));
  std::memcpy(geom_b.value().mapped, box_b.data(), box_b.size() * sizeof(float));

  // Device addresses (core 1.2 entry point, exported by the loader).
  auto address_of = [&](VkBuffer buffer) -> std::uint64_t {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return static_cast<std::uint64_t>(
        vkGetBufferDeviceAddress(context.device(), &info));
  };

  BlasBuildInput in_a{};
  in_a.vertex_buffer_address = address_of(geom_a.value().buffer);
  in_a.triangle_count = 12U;
  // Non-indexed: maxVertex is the highest referenced vertex index
  // (primitiveCount * 3 - 1; VUID 10775).
  in_a.max_vertex = 12U * 3U - 1U;
  BlasBuildInput in_b{};
  in_b.vertex_buffer_address = address_of(geom_b.value().buffer);
  in_b.triangle_count = 12U;
  in_b.max_vertex = 12U * 3U - 1U;
  ASSERT_NE(in_a.vertex_buffer_address, 0U);
  ASSERT_NE(in_b.vertex_buffer_address, 0U);

  omnicpp::render::VulkanAccelerationStructureBuilder builder;
  auto blas_a = builder.create_blas(context.device(), allocator, in_a);
  auto blas_b = builder.create_blas(context.device(), allocator, in_b);
  ASSERT_TRUE(blas_a.is_ok());
  ASSERT_TRUE(blas_b.is_ok());
  EXPECT_NE(blas_a.value().device_address, 0U);
  EXPECT_NE(blas_b.value().device_address, 0U);
  EXPECT_GT(blas_a.value().storage_bytes, 0U);
  EXPECT_GT(blas_b.value().storage_bytes, 0U);

  // ---- TLAS: two instances -------------------------------------------------
  auto tlas = builder.create_tlas(context.device(), allocator, 2U);
  ASSERT_TRUE(tlas.is_ok());
  std::array<TlasInstance, 2> instances{};
  {
    const std::array<float, 12> xf_a = translated_instance(-0.6f, 0.0f, 0.0f);
    std::memcpy(instances[0].transform, xf_a.data(), sizeof(xf_a));
  }
  instances[0].instance_custom_index = 10U;
  instances[0].blas_device_address = blas_a.value().device_address;
  const std::array<float, 12> xf_b =
      rotated_translated_instance(0.6f, 0.0f, 0.0f);
  std::memcpy(instances[1].transform, xf_b.data(), sizeof(xf_b));
  instances[1].instance_custom_index = 20U;
  instances[1].blas_device_address = blas_b.value().device_address;

  // ---- Results buffer --------------------------------------------------------
  auto results = allocator.create_buffer(
      kGrid * kGrid * 16U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(results.is_ok());
  // 0xEE pre-fill: any untouched u32 proves the shader never wrote.
  std::memset(results.value().mapped, 0xEE, kGrid * kGrid * 16U);

  // ---- Descriptor set: AS + results -----------------------------------------
  const std::vector<omnicpp::render::ReflectedBinding> bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
       VK_SHADER_STAGE_COMPUTE_BIT},
      {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_COMPUTE_BIT}};
  auto layout = descriptors.create_layout(bindings, 1U);
  ASSERT_TRUE(layout.is_ok());
  auto set = descriptors.allocate_set(layout.value());
  ASSERT_TRUE(set.is_ok());

  VkWriteDescriptorSetAccelerationStructureKHR as_write{};
  as_write.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
  VkAccelerationStructureKHR as_handle = tlas.value().handle;
  as_write.accelerationStructureCount = 1U;
  as_write.pAccelerationStructures = &as_handle;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.pNext = &as_write;
  write.dstSet = set.value();
  write.dstBinding = 0U;
  write.descriptorCount = 1U;
  write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  vkUpdateDescriptorSets(context.device(), 1U, &write, 0U, nullptr);
  ASSERT_TRUE(descriptors
                  .write_buffer(set.value(), 1U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                results.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());

  // ---- Compute pipeline -------------------------------------------------------
  omnicpp::render::VulkanPipeline pipe;
  const std::string sd = OMNICPP_TEST_SHADER_DIR;
  ASSERT_TRUE(pipe
                  .load_shader_stage_file(context.device(),
                                          sd + "/ray_query_first.comp.spv",
                                          "compute")
                  .is_ok());
  const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 8U};
  const VkDescriptorSetLayout set_layout = layout.value();
  ASSERT_TRUE(pipe
                  .create_pipeline_layout(context.device(), &set_layout, 1U,
                                          &push_range)
                  .is_ok());
  ASSERT_TRUE(pipe
                  .create_compute_pipeline(context.device(),
                                           pipe.pipeline_layout())
                  .is_ok());

  // ---- Command buffer: BLAS builds -> TLAS build -> barrier -> dispatch ----
  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool.value());
  ASSERT_TRUE(cbr.is_ok());
  VkCommandBuffer cb = cbr.value();
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(context.device(), &fi, nullptr, &fence);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);

  // Scratch: sized to the largest single build; reuse requires barriers.
  const auto sizes_a =
      omnicpp::render::VulkanAccelerationStructureBuilder::query_blas_sizes(
          context.device(), in_a);
  const auto sizes_b =
      omnicpp::render::VulkanAccelerationStructureBuilder::query_blas_sizes(
          context.device(), in_b);
  const std::uint64_t blas_scratch =
      std::max(sizes_a.buildScratchSize, sizes_b.buildScratchSize);
  const auto tlas_sizes =
      omnicpp::render::VulkanAccelerationStructureBuilder::query_tlas_sizes(
          context.device(), 2U);
  const std::uint64_t scratch_bytes =
      std::max(blas_scratch, tlas_sizes.buildScratchSize);
  omnicpp::render::VulkanScratchPool scratch;
  auto scratch_addr =
      scratch.acquire(allocator, context.device(), scratch_bytes);
  ASSERT_TRUE(scratch_addr.is_ok());

  ASSERT_TRUE(builder
                  .cmd_build_blas(cb, context.device(), blas_a.value(), in_a,
                                  scratch_addr.value())
                  .is_ok());
  // Same scratch region for build B: memory dependency required. Only
  // acceleration-structure accesses are legal in the AS-build stage.
  VkMemoryBarrier scratch_bar{};
  scratch_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  scratch_bar.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  scratch_bar.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  vkCmdPipelineBarrier(
      cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U,
      &scratch_bar, 0U, nullptr, 0U, nullptr);
  ASSERT_TRUE(builder
                  .cmd_build_blas(cb, context.device(), blas_b.value(), in_b,
                                  scratch_addr.value())
                  .is_ok());
  // Reuse the same scratch for the TLAS build.
  vkCmdPipelineBarrier(
      cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U,
      &scratch_bar, 0U, nullptr, 0U, nullptr);
  ASSERT_TRUE(builder
                  .cmd_build_tlas(cb, context.device(), tlas.value(),
                                  instances.data(), 2U, scratch_addr.value())
                  .is_ok());

  // AS writes -> ray-query reads. Ray queries execute inside the COMPUTE
  // stage (the ray-tracing-shader stage requires the pipeline feature).
  VkMemoryBarrier as_bar{};
  as_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  as_bar.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  as_bar.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  vkCmdPipelineBarrier(
      cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0U, 1U, &as_bar, 0U,
      nullptr, 0U, nullptr);

  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipe.pipeline_layout(), 0U, 1U, &set.value(), 0U,
                          nullptr);
  const std::uint32_t push[2] = {kGrid, kGrid};
  vkCmdPushConstants(cb, pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                     0U, sizeof(push), push);
  vkCmdDispatch(cb, 1U, 1U, 1U);

  VkMemoryBarrier host_bar{};
  host_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  host_bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  host_bar.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0U, 1U, &host_bar, 0U,
                       nullptr, 0U, nullptr);

  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  VkSubmitInfo sub{};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1U;
  sub.pCommandBuffers = &cb;
  vkQueueSubmit(context.graphics_queue(), 1U, &sub, fence);
  ASSERT_EQ(vkWaitForFences(context.device(), 1U, &fence, VK_TRUE,
                            1'000'000'000ULL),
            VK_SUCCESS);

  // ---- Assertions --------------------------------------------------------------
  const auto* px = static_cast<const std::uint32_t*>(results.value().mapped);
  std::size_t hits_a = 0U;
  std::size_t hits_b = 0U;
  std::size_t misses = 0U;
  for (std::uint32_t i = 0; i < kGrid * kGrid; ++i) {
    const std::uint32_t* p = px + i * 4U;
    if (p[0] == 0xEEEEEEEEU) {
      ADD_FAILURE() << "ray " << i << " never written by shader";
    } else if (p[0] == 0xFFU) {
      if (p[1] == 11U) {
        ++hits_a;
      } else if (p[1] == 21U) {
        ++hits_b;
      } else {
        ADD_FAILURE() << "ray " << i << " unexpected custom index "
                      << (p[1] - 1U);
      }
    } else if (p[0] == 0U) {
      ++misses;
    } else {
      ADD_FAILURE() << "ray " << i << " unexpected word " << std::hex << p[0];
    }
  }
  // Exact expectation table (ox = -1.0 + 0.3*col, oy = -0.7 + 0.2*row+0.1):
  //   A x-range [-1.05,-0.15]: cols ox=-1.0,-0.7,-0.4 hit (-0.1 is RIGHT of
  //   the -0.15 edge) -> 3 cols x 8 rows = 24.
  //   B x-range [0.15,1.05] (rotated), |oy|<=0.35: cols ox=0.2,0.5,0.8 x 4
  //   rows = 12 (ox=1.1 is right of the 1.05 edge).
  //   Misses: col ox=-0.1 (8) + out-of-band rows of cols 0.2/0.5/0.8 (12)
  //   + all rows of col ox=1.1 (8) = 28.
  std::printf("hits_a=%zu hits_b=%zu misses=%zu\n", hits_a, hits_b, misses);
  EXPECT_EQ(hits_a, 24U);
  EXPECT_EQ(hits_b, 12U);
  EXPECT_EQ(misses, 28U);

  vkDestroyFence(context.device(), fence, nullptr);
  pipe.cleanup(context.device());
  vkDestroyCommandPool(context.device(), pool.value(), nullptr);
  omnicpp::render::BottomLevelAS out_a = std::move(blas_a.value());
  omnicpp::render::BottomLevelAS out_b = std::move(blas_b.value());
  omnicpp::render::TopLevelAS out_t = std::move(tlas.value());
  scratch.cleanup(allocator);
  builder.destroy_blas(context.device(), allocator, out_a);
  builder.destroy_blas(context.device(), allocator, out_b);
  builder.destroy_tlas(context.device(), allocator, out_t);
}

#endif  // OMNICPP_HAS_VULKAN && OMNICPP_TEST_SHADER_DIR
