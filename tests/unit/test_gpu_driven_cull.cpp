//! @file test_gpu_driven_cull.cpp
//! @brief GPU-driven migration step 3: the cull_and_draw_lod compute pass
//!        writes indirect draw commands from the payload + mesh table, and
//!        the graphics pass draws ONE vkCmdDrawIndexedIndirect for the
//!        whole scene — the CPU never computes visibility, LOD, or draw
//!        parameters inside the frame. Proofs:
//!          1. Command-buffer readback (host-coherent): near bar selects
//!             LOD 0, far bar selects LOD 1 (exact index counts from the
//!             mesh table), the off-frustum bar gets a degenerate command.
//!          2. Visible counter reads exactly 2.
//!          3. Pixel structure: near column shows the full-detail height,
//!             far column shows the low-LOD height, behind-camera column is
//!             clear — the LOD decision is visible in the image itself.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_mesh_table.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/render/vulkan_scene.hpp"
#include "vulkan_test_readback.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

using omnicpp::render::SceneMatrix;

constexpr std::uint32_t kSize = 256U;
constexpr std::uint32_t kObjectCount = 3U;
constexpr std::uint32_t kPayloadWords = 2U + 24U * kObjectCount;
constexpr std::uint32_t kSphereWord = 2U + 18U;
constexpr std::uint32_t kDrawWord = 0U;
constexpr std::uint32_t kVisibleWord = 5U * kObjectCount;

SceneMatrix make_perspective(float fov_y_degrees, float aspect, float znear,
                             float zfar) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  const float f = 1.0f / std::tan(fov_y_degrees * 3.14159265f / 360.0f);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (zfar + znear) / (znear - zfar);
  m[11] = -1.0f;
  m[14] = (2.0f * zfar * znear) / (znear - zfar);
  m[15] = 0.0f;
  return m;
}

SceneMatrix make_translation(float x, float y, float z) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

//! Vertical bar: width 0.5, height h, depth 0.5. Engine vertex layout.
void build_bar(float height, std::vector<float>& vertices,
               std::vector<std::uint32_t>& indices) {
  vertices.clear();
  indices.clear();
  const float hw = 0.25f;
  const float hy = height * 0.5f;
  const float f[6][4][3] = {
      {{-hw, -hy, hw}, {hw, -hy, hw}, {hw, hy, hw}, {-hw, hy, hw}},
      {{hw, -hy, -hw}, {-hw, -hy, -hw}, {-hw, hy, -hw}, {hw, hy, -hw}},
      {{hw, -hy, hw}, {hw, -hy, -hw}, {hw, hy, -hw}, {hw, hy, hw}},
      {{-hw, -hy, -hw}, {-hw, -hy, hw}, {-hw, hy, hw}, {-hw, hy, -hw}},
      {{-hw, hy, hw}, {hw, hy, hw}, {hw, hy, -hw}, {-hw, hy, -hw}},
      {{-hw, -hy, -hw}, {hw, -hy, -hw}, {hw, -hy, hw}, {-hw, -hy, hw}},
  };
  const float n[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0},
                         {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int face = 0; face < 6; ++face) {
    const std::uint32_t base =
        static_cast<std::uint32_t>(vertices.size() / 11U);
    for (int c = 0; c < 4; ++c) {
      vertices.push_back(f[face][c][0]);
      vertices.push_back(f[face][c][1]);
      vertices.push_back(f[face][c][2]);
      vertices.push_back(1.0f);
      vertices.push_back(1.0f);
      vertices.push_back(1.0f);
      vertices.push_back(n[face][0]);
      vertices.push_back(n[face][1]);
      vertices.push_back(n[face][2]);
      vertices.push_back(uv[c][0]);
      vertices.push_back(uv[c][1]);
    }
    // Engine convention: CW in view space so the projection's y-flip lands
    // triangles CCW in framebuffer space (pipeline frontFace = COUNTER_CLOCKWISE).
    const std::uint32_t tri[6] = {0, 2, 1, 0, 3, 2};
    for (int k = 0; k < 6; ++k) indices.push_back(base + tri[k]);
  }
}

//! 1x1 opaque-white R8G8B8A8 texture (bindless element 0 fallback).
bool make_white_texture(VkDevice device, VkPhysicalDevice physical_device,
                        VkQueue queue, std::uint32_t queue_family,
                        omnicpp::render::VulkanMemoryAllocator& allocator,
                        VkImageView& out_view, VkSampler& out_sampler,
                        omnicpp::render::Allocation& out_alloc) {
  VkImage image = VK_NULL_HANDLE;
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {1, 1, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &ii, nullptr, &image) != VK_SUCCESS) return false;
  auto mem = allocator.bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!mem.is_ok()) {
    vkDestroyImage(device, image, nullptr);
    return false;
  }
  omnicpp::render::Allocation image_mem = mem.value();

  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &vi, nullptr, &out_view) != VK_SUCCESS) {
    allocator.destroy_allocation(image_mem);
    vkDestroyImage(device, image, nullptr);
    return false;
  }
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_NEAREST;
  si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device, &si, nullptr, &out_sampler) != VK_SUCCESS) {
    vkDestroyImageView(device, out_view, nullptr);
    allocator.destroy_allocation(image_mem);
    vkDestroyImage(device, image, nullptr);
    return false;
  }
  out_alloc = image_mem;

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(device,
                                                                   queue_family);
  if (!pool.is_ok()) return false;
  auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      device, pool.value());
  if (!cbr.is_ok()) {
    vkDestroyCommandPool(device, pool.value(), nullptr);
    return false;
  }
  VkCommandBuffer cb = cbr.value();
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(device, &fi, nullptr, &fence);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool ok = vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.image = image;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange = range;
  if (ok) {
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkClearColorValue color{};
    color.float32[0] = color.float32[1] = color.float32[2] =
        color.float32[3] = 1.0f;
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
  }
  VkSubmitInfo sub{};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1;
  sub.pCommandBuffers = &cb;
  ok = ok && vkQueueSubmit(queue, 1, &sub, fence) == VK_SUCCESS &&
       vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
  vkDestroyFence(device, fence, nullptr);
  vkDestroyCommandPool(device, pool.value(), nullptr);
  return ok;
}

//! Count consecutive red-dominant pixels along a pixel column.
std::size_t count_red_run(const std::vector<std::uint32_t>& pixels,
                          std::uint32_t width, std::uint32_t height,
                          std::uint32_t x) {
  std::size_t best = 0;
  std::size_t current = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t px = pixels[y * width + x];
    const std::uint32_t r = px & 0xFFU;
    const std::uint32_t g = (px >> 8U) & 0xFFU;
    const std::uint32_t b = (px >> 16U) & 0xFFU;
    if (r > 60U && r > g + 20U && r > b + 20U) {
      ++current;
      best = current > best ? current : best;
    } else {
      current = 0;
    }
  }
  return best;
}

}  // namespace

TEST(VulkanHardware, GpuDrivenCullWritesIndirectCommands) {
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppGpuDrivenCull", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device()).is_ok());
  omnicpp::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);

  // ---- Geometry: tall bar (LOD 0) + short bar (LOD 1) --------------------
  std::vector<float> tall_v, short_v;
  std::vector<std::uint32_t> tall_i, short_i;
  build_bar(2.0f, tall_v, tall_i);
  build_bar(1.0f, short_v, short_i);
  omnicpp::render::SceneMesh shell{};
  omnicpp::render::SceneMeshTableBuilder builder;
  const auto slot_tall = builder.add(shell, tall_v, tall_i);
  const auto slot_short = builder.add(shell, short_v, short_i);
  ASSERT_NE(slot_tall, omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);
  ASSERT_NE(slot_short, omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);
  const auto& merged = builder.build();

  auto vb = allocator.create_buffer(
      merged.vertex_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto ib = allocator.create_buffer(
      merged.index_bytes(),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(vb.is_ok() && ib.is_ok());
  std::memcpy(vb.value().mapped, merged.vertex_data.data(),
              merged.vertex_bytes());
  std::memcpy(ib.value().mapped, merged.index_data.data(),
              merged.index_bytes());

  // ---- Payload: near tall bar, far tall bar (-> LOD 1), behind camera ----
  struct Payload {
    SceneMatrix model;
    std::uint32_t material_index;
    std::uint32_t mesh_slot;
    std::array<float, 3> sphere_center;
    float sphere_radius;
    std::uint32_t lod_count;
    std::uint32_t pad;
  };
  static_assert(sizeof(Payload) == 96U, "24 words expected");
  // Far bar offset in x so it clears the near bar's silhouette (depth test
  // would otherwise hide the LOD-1 proof behind the LOD-0 bar).
  const Payload payloads[kObjectCount] = {
      {make_translation(0.0f, 0.0f, -3.0f), 0U, slot_tall,
       {0.0f, 0.0f, -3.0f}, 1.05f, 2U, 0U},
      {make_translation(2.5f, 0.0f, -16.0f), 0U, slot_tall,
       {2.5f, 0.0f, -16.0f}, 1.05f, 2U, 0U},
      {make_translation(0.0f, 0.0f, 8.0f), 0U, slot_tall,
       {0.0f, 0.0f, 8.0f}, 1.05f, 2U, 0U},
  };
  auto payload_buf = allocator.create_buffer(
      kPayloadWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(payload_buf.is_ok());
  {
    auto* words = static_cast<std::uint32_t*>(payload_buf.value().mapped);
    words[0] = kObjectCount;
    words[1] = 0U;
    std::memcpy(words + 2U, payloads, sizeof(payloads));
  }

  // Mesh table SSBO (5 words/slot) + zero-initialized draw-command buffer.
  std::vector<std::uint32_t> table_words;
  table_words.reserve(merged.entries.size() * 5U);
  for (const auto& e : merged.entries) {
    table_words.push_back(e.index_count);
    table_words.push_back(e.index_offset);
    table_words.push_back(e.vertex_base);
    table_words.push_back(e.vertex_count);
    table_words.push_back(e.dedup_of);
  }
  auto table_buf = allocator.create_buffer(
      table_words.size() * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(table_buf.is_ok());
  std::memcpy(table_buf.value().mapped, table_words.data(),
              table_words.size() * 4U);

  auto draw_buf = allocator.create_buffer(
      (kVisibleWord + 1U) * 4U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(draw_buf.is_ok());
  std::memset(draw_buf.value().mapped, 0, (kVisibleWord + 1U) * 4U);

  // ---- One descriptor set shared by compute + graphics -------------------
  const std::vector<omnicpp::render::ReflectedBinding> driven_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT},
      {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT},
      {0U, 2U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_COMPUTE_BIT},
      {0U, 3U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_COMPUTE_BIT}};
  auto driven_layout = descriptors.create_layout(driven_bindings, 8U);
  ASSERT_TRUE(driven_layout.is_ok());
  auto driven_set = descriptors.allocate_set(driven_layout.value());
  ASSERT_TRUE(driven_set.is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(driven_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                vb.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(driven_set.value(), 1U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                payload_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(driven_set.value(), 2U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                table_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(driven_set.value(), 3U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                draw_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());

  // Material (red) + bindless white fallback + set layouts.
  omnicpp::render::PbrMaterialData mat{};
  mat.base_color_factor = {0.9f, 0.1f, 0.1f, 1.0f};
  mat.roughness_factor = 0.7f;
  auto mat_buf = allocator.create_buffer(
      sizeof(omnicpp::render::PbrMaterialData),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(mat_buf.is_ok());
  std::memcpy(mat_buf.value().mapped, &mat, sizeof(mat));
  const std::vector<omnicpp::render::ReflectedBinding> tex_bindings = {
      {1U, 0U, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto tex_layout = descriptors.create_layout(tex_bindings, 1U, true);
  ASSERT_TRUE(tex_layout.is_ok());
  auto tex_set = descriptors.allocate_set(tex_layout.value());
  ASSERT_TRUE(tex_set.is_ok());
  VkImageView white_view = VK_NULL_HANDLE;
  VkSampler white_sampler = VK_NULL_HANDLE;
  omnicpp::render::Allocation white_alloc{};
  ASSERT_TRUE(make_white_texture(context.device(), context.physical_device(),
                                 context.graphics_queue(), qf, allocator,
                                 white_view, white_sampler, white_alloc));
  ASSERT_TRUE(descriptors
                  .write_image(tex_set.value(), 0U,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               white_sampler, white_view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
                  .is_ok());
  const std::vector<omnicpp::render::ReflectedBinding> mat_bindings = {
      {2U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto mat_layout = descriptors.create_layout(mat_bindings, 8U);
  ASSERT_TRUE(mat_layout.is_ok());
  auto mat_set = descriptors.allocate_set(mat_layout.value());
  ASSERT_TRUE(mat_set.is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(mat_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                mat_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());

  // ---- Pipelines ----------------------------------------------------------
  omnicpp::render::VulkanPipeline cull_pipe, gfx_pipe;
  const std::string sd = WARPLOOM_TEST_SHADER_DIR;
  ASSERT_TRUE(cull_pipe
                  .load_shader_stage_file(context.device(),
                                          sd + "/cull_and_draw_lod.comp.spv",
                                          "compute")
                  .is_ok());
  const VkPushConstantRange cull_push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 144U};
  const VkDescriptorSetLayout cull_layouts[1] = {driven_layout.value()};
  ASSERT_TRUE(cull_pipe
                  .create_pipeline_layout(context.device(), cull_layouts, 1U,
                                          &cull_push)
                  .is_ok());
  ASSERT_TRUE(cull_pipe
                  .create_compute_pipeline(context.device(),
                                           cull_pipe.pipeline_layout())
                  .is_ok());

  ASSERT_TRUE(gfx_pipe
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_gpu_driven.vert.spv",
                                          "vertex")
                  .is_ok());
  ASSERT_TRUE(gfx_pipe
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_gpu_driven.frag.spv",
                                          "fragment")
                  .is_ok());
  const VkDescriptorSetLayout gfx_layouts[3] = {driven_layout.value(),
                                                tex_layout.value(),
                                                mat_layout.value()};
  const VkPushConstantRange gfx_push{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};
  ASSERT_TRUE(gfx_pipe
                  .create_pipeline_layout(context.device(), gfx_layouts, 3U,
                                          &gfx_push)
                  .is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target
                  .create(context.device(), context.physical_device(),
                          VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator)
                  .is_ok());
  ASSERT_TRUE(target
                  .create_depth(context.device(), context.physical_device(),
                                VK_FORMAT_D32_SFLOAT)
                  .is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());
  ASSERT_TRUE(gfx_pipe
                  .create_graphics_pipeline(context.device(),
                                            target.render_pass(),
                                            target.format(),
                                            gfx_pipe.pipeline_layout(), true,
                                            true, false)
                  .is_ok());

  // ---- One command buffer: compute cull -> barrier -> indirect draws -----
  const SceneMatrix vp = make_perspective(45.0f, 1.0f, 0.1f, 100.0f);
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

  // Compute: cull + LOD + write commands.
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipe.pipeline());
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          cull_pipe.pipeline_layout(), 0U, 1U,
                          &driven_set.value(), 0U, nullptr);
  struct CullPush {
    std::uint32_t object_count;
    std::uint32_t sphere_word;
    std::uint32_t draw_word;
    std::uint32_t visible_word;
    float planes[6][4];
    float tan_half_fov;
    float viewport_h;
    std::uint32_t lod_threshold_count;
    float lod_thresholds[4];
  } push{};
  push.object_count = kObjectCount;
  push.sphere_word = kSphereWord;
  push.draw_word = kDrawWord;
  push.visible_word = kVisibleWord;
  // Perspective frustum planes (camera at origin, forward -Z, tan_half =
  // tan(22.5 deg)): side planes pass THROUGH the camera (d = 0) with the
  // tan_half slope folded into the normal; near keeps z <= -near, far
  // keeps z >= -far. (A box-with-near-extent plane set would wrongly cull
  // off-axis objects at depth.)
  const float th = 0.41421356f;
  // Inward normals: side planes pass through the camera, so d = 0 and the
  // slope carries the sign — inside means dot(n,p) + d >= 0. With camera
  // forward = -Z, a point at depth d has z = -d, so the slope term must be
  // -tan*z = +d*tan for on-axis points (z-component NEGATIVE tan).
  const float planes[6][4] = {
      {-1, 0, -th, 0.0f}, {1, 0, -th, 0.0f},   // right, left bounds
      {0, -1, -th, 0.0f}, {0, 1, -th, 0.0f},   // top, bottom bounds
      {0, 0, -1, -0.1f}, {0, 0, 1, 100.0f},    // near, far
  };
  std::memcpy(push.planes, planes, sizeof(planes));
  push.tan_half_fov = 0.41421356f;
  push.viewport_h = static_cast<float>(kSize);
  push.lod_threshold_count = 2U;
  const float thresholds[2] = {80.0f, 0.0f};
  std::memcpy(push.lod_thresholds, thresholds, sizeof(thresholds));
  vkCmdPushConstants(cb, cull_pipe.pipeline_layout(),
                     VK_SHADER_STAGE_COMPUTE_BIT, 0U, sizeof(push), &push);
  vkCmdDispatch(cb, (kObjectCount + 63U) / 64U, 1U, 1U);

  // Compute writes -> indirect/vertex reads.
  VkBufferMemoryBarrier bb{};
  bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                     VK_ACCESS_SHADER_READ_BIT;
  bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.buffer = draw_buf.value().buffer;
  bb.offset = 0U;
  bb.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                       0U, 0U, nullptr, 1U, &bb, 0U, nullptr);

  // Graphics: ONE indirect draw for the whole scene.
  VkClearValue clears[2]{};
  // Clear to fully-transparent black: the readback helper counts any nonzero
  // RGB channel as non-clear, so alpha must be 0 in the clear colour.
  clears[0].color = {{0, 0, 0, 0}};
  clears[1].depthStencil = {1.0f, 0U};
  VkRenderPassBeginInfo rb{};
  rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rb.renderPass = target.render_pass();
  rb.framebuffer = target.framebuffer();
  rb.renderArea.extent = {kSize, kSize};
  rb.clearValueCount = 2U;
  rb.pClearValues = clears;
  vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0, 0, float(kSize), float(kSize), 0, 1};
  vkCmdSetViewport(cb, 0, 1, &viewport);
  VkRect2D scissor{{0, 0}, {kSize, kSize}};
  vkCmdSetScissor(cb, 0, 1, &scissor);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline());
  const VkDescriptorSet gfx_sets[3] = {driven_set.value(), tex_set.value(),
                                       mat_set.value()};
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          gfx_pipe.pipeline_layout(), 0U, 3U, gfx_sets, 0U,
                          nullptr);
  struct GfxPush {
    SceneMatrix view_projection;
    SceneMatrix model_unused;
    std::array<float, 4> camera_position;
    std::uint32_t material_index_unused;
    std::array<std::uint32_t, 3> pad{};
  } gfx_push_data{vp, {}, {0.0f, 0.0f, 0.0f, 1.0f}, 0U, {}};
  vkCmdPushConstants(cb, gfx_pipe.pipeline_layout(),
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0U, sizeof(gfx_push_data), &gfx_push_data);
  vkCmdBindIndexBuffer(cb, ib.value().buffer, 0U, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexedIndirect(cb, draw_buf.value().buffer, 0U, kObjectCount,
                           sizeof(VkDrawIndexedIndirectCommand));
  vkCmdEndRenderPass(cb);
  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);

  VkSubmitInfo sub{};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1U;
  sub.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1U, &sub, fence),
            VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(context.device(), 1U, &fence, VK_TRUE, UINT64_MAX),
            VK_SUCCESS);
  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool.value(), nullptr);

  // ---- Proof 1: command readback (host-coherent, GPU-written) ------------
  const auto* words = static_cast<const std::uint32_t*>(draw_buf.value().mapped);
  const auto* cmds = words + kDrawWord;
  const std::uint32_t tall_count = merged.entries[slot_tall].index_count;
  const std::uint32_t short_count = merged.entries[slot_short].index_count;
  // Object 0: near -> LOD 0 (tall bar), visible.
  EXPECT_EQ(cmds[0], tall_count);
  EXPECT_EQ(cmds[1], 1U);
  EXPECT_EQ(cmds[2], merged.entries[slot_tall].index_offset);
  EXPECT_EQ(cmds[4], 0U);  // firstInstance = object index
  // Object 1: far -> LOD 1 (short bar), visible.
  EXPECT_EQ(cmds[5], short_count);
  EXPECT_EQ(cmds[6], 1U);
  EXPECT_EQ(cmds[7], merged.entries[slot_short].index_offset);
  EXPECT_EQ(cmds[9], 1U);
  // Object 2: behind camera -> degenerate command, instanceCount 0.
  EXPECT_EQ(cmds[10], 0U);
  EXPECT_EQ(cmds[11], 0U);
  // Visible counter: exactly 2.
  EXPECT_EQ(words[kVisibleWord], 2U);

  // ---- Proof 2: pixel structure readback ----------------------------------
  const auto pixels_result = omnicpp_test::readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      qf, target.image(), target.format(), kSize, kSize,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      /*store_pixels=*/true);
  ASSERT_TRUE(pixels_result.submitted);
  ASSERT_EQ(pixels_result.pixels.size(),
            static_cast<std::size_t>(kSize) * kSize);

  // Screen-height math (fov 45, viewport 256): world_h/depth * 309.
  //   near bar (h=2, d=3): ~206 px (LOD 0 mesh). LOD 1 there would be ~103.
  //   far bar (h=1, d=16): ~19 px (LOD 1 mesh). LOD 0 there would be ~39.
  const std::size_t near_run = count_red_run(pixels_result.pixels, kSize, kSize, 128U);
  // Far bar: NDC x = 2.5/(16*0.4142) ~= 0.377 -> pixel ~176 (bar spans ~9 px).
  const std::size_t far_run = count_red_run(pixels_result.pixels, kSize, kSize, 176U);
  // Behind-camera bar would be at NDC x = -0.5/(8*0.4142) -> pixel ~92.
  const std::size_t behind_run = count_red_run(pixels_result.pixels, kSize, kSize, 92U);

  std::printf("\n[CULL] near=%zu far=%zu behind=%zu cmds0={%u,%u,%u,%u,%u} cmds1_0=%u visible=%u\n",
              near_run, far_run, behind_run, cmds[0], cmds[1], cmds[2],
              cmds[3], cmds[4], cmds[5], words[kVisibleWord]);
  std::fflush(stdout);

  EXPECT_GT(near_run, 180U)
      << "near bar must draw the full-detail (LOD 0) mesh";
  EXPECT_LT(near_run, 235U);
  EXPECT_GT(far_run, 12U) << "far bar should be visible at LOD 1";
  EXPECT_LT(far_run, 30U)
      << "far bar must draw the low-detail (LOD 1) mesh (LOD 0 would be ~39 px)";
  EXPECT_EQ(behind_run, 0U)
      << "behind-camera bar must be culled by the GPU pass";
  EXPECT_GT(pixels_result.non_clear_pixels, 5000U);

  // Cleanup
  if (white_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(context.device(), white_sampler, nullptr);
  }
  if (white_view != VK_NULL_HANDLE) {
    vkDestroyImageView(context.device(), white_view, nullptr);
  }
  if (white_alloc.is_valid()) {
    allocator.destroy_allocation(white_alloc);
  }
  gfx_pipe.cleanup(context.device());
  cull_pipe.cleanup(context.device());
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
}

#endif  // WARPLOOM_HAS_VULKAN
