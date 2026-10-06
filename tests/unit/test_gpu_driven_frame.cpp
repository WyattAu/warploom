//! @file test_gpu_driven_frame.cpp
//! @brief GPU E2E proof for the renderer-owned one-submission GPU-driven
//!        frame. The test hand-builds nothing inside the frame command
//!        buffer: ONE call to VulkanRenderer::record_pbr_frame_gpu_driven
//!        records the [cull compute -> indirect main draw] graph, with the
//!        compute -> draw-indirect barrier computed by compile_graph from
//!        the consumer-declared buffer edge (no hand-authored barriers).
//!        Proofs: GPU-written command readback (LOD 0 near / LOD 1 far /
//!        culled behind-camera / visible == 2) and pixel-structure readback
//!        identical to the harness path in test_gpu_driven_cull.cpp —
//!        proving the graph-computed barrier actually orders the draw's
//!        indirect fetch after the compute writes.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_mesh_table.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/render/vulkan_scene.hpp"
#include "vulkan_test_readback.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

constexpr std::uint32_t kSize = 256U;
constexpr std::uint32_t kObjectCount = 3U;
constexpr std::uint32_t kPayloadWords = 2U + 24U * kObjectCount;
constexpr std::uint32_t kSphereWord = 2U + 18U;
constexpr std::uint32_t kDrawWord = 0U;
constexpr std::uint32_t kVisibleWord = 5U * kObjectCount;

//! 24-word object payload; layout mirrors pbr_gpu_driven.vert.
struct Payload {
  omnicpp::render::SceneMatrix model;
  std::uint32_t material_index;
  std::uint32_t mesh_slot;
  std::array<float, 4> sphere;  // center.xyz + radius
  std::uint32_t lod_count;
  std::uint32_t pad;
};
static_assert(sizeof(Payload) == 96U, "24 words expected");

omnicpp::render::SceneMatrix make_translation(float x, float y, float z) {
  auto m = omnicpp::render::scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

//! Perspective projection, Vulkan clip space (y down, z into [0,1]).
omnicpp::render::SceneMatrix make_perspective(float fov_y, float aspect,
                                              float znear, float zfar) {
  const float f =
      1.0f / std::tan(fov_y * 3.14159265f / 360.0f);
  omnicpp::render::SceneMatrix m =
      omnicpp::render::scene_identity_matrix();
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (zfar + znear) / (znear - zfar);
  m[11] = -1.0f;
  m[14] = (2.0f * zfar * znear) / (znear - zfar);
  m[15] = 0.0f;
  return m;
}

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
                         {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
  const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (std::size_t face = 0; face < static_cast<std::size_t>(6); ++face) {
    const std::uint32_t base =
        static_cast<std::uint32_t>(vertices.size() / 11U);
    for (std::size_t c = 0; c < static_cast<std::size_t>(4); ++c) {
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
    // CW in view space -> CCW in framebuffer space (COUNTER_CLOCKWISE).
    const std::uint32_t tri[6] = {0, 2, 1, 0, 3, 2};
    for (std::size_t k = 0; k < static_cast<std::size_t>(6); ++k) indices.push_back(base + tri[k]);
  }
}

//! Longest vertical run of red-dominant pixels in one column.
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

//! 1x1 opaque-white R8G8B8A8 texture (bindless element 0 fallback).
bool make_white_texture(VkDevice device, VkPhysicalDevice /*physical_device*/,
                        VkQueue queue, std::uint32_t queue_family,
                        omnicpp::render::VulkanMemoryAllocator& allocator,
                        VkImage& out_image, VkImageView& out_view,
                        VkSampler& out_sampler,
                        omnicpp::render::Allocation& out_alloc) {
  VkImage image = VK_NULL_HANDLE;
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {1U, 1U, 1U};
  ii.mipLevels = 1U;
  ii.arrayLayers = 1U;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &ii, nullptr, &image) != VK_SUCCESS) return false;
  out_image = image;
  auto mem = allocator.bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!mem.is_ok()) {
    vkDestroyImage(device, image, nullptr);
    return false;
  }
  out_alloc = mem.value();

  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = ii.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &vi, nullptr, &out_view) != VK_SUCCESS) {
    return false;
  }
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_NEAREST;
  si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  if (vkCreateSampler(device, &si, nullptr, &out_sampler) != VK_SUCCESS) {
    return false;
  }

  // Upload white via staging.
  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      device, queue_family);
  if (!pool.is_ok()) return false;
  auto staging = allocator.create_buffer(
      4U, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!staging.is_ok()) {
    vkDestroyCommandPool(device, pool.value(), nullptr);
    return false;
  }
  std::memcpy(staging.value().mapped, "\xff\xff\xff\xff", 4U);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      device, pool.value());
  if (!cbr.is_ok() || vkBeginCommandBuffer(cbr.value(), &bi) != VK_SUCCESS) {
    vkDestroyCommandPool(device, pool.value(), nullptr);
    return false;
  }
  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = image;
  to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_dst.srcAccessMask = 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cbr.value(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_dst);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {1U, 1U, 1U};
  vkCmdCopyBufferToImage(cbr.value(), staging.value().buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  VkImageMemoryBarrier to_read = to_dst;
  to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cbr.value(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                       0, nullptr, 1, &to_read);
  vkEndCommandBuffer(cbr.value());
  VkSubmitInfo sub{};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1;
  sub.pCommandBuffers = &cbr.value();
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(device, &fi, nullptr, &fence);
  vkQueueSubmit(queue, 1, &sub, fence);
  vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(device, fence, nullptr);
  vkDestroyCommandPool(device, pool.value(), nullptr);
  return true;
}

}  // namespace

TEST(VulkanHardware, RendererGpuDrivenFrameOneSubmission) {
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppGpuDrivenFrame", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device())
          .is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);

  // ---- Geometry: tall bar (LOD 0) + short bar (LOD 1), shared buffers ----
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

  // ---- Payload: near tall, far tall (-> LOD 1), behind camera ----
  const Payload payloads[kObjectCount] = {
      {make_translation(0.0f, 0.0f, -3.0f), 0U, slot_tall,
       {0.0f, 0.0f, -3.0f, 1.05f}, 2U, 0U},
      {make_translation(2.5f, 0.0f, -16.0f), 0U, slot_tall,
       {2.5f, 0.0f, -16.0f, 1.05f}, 2U, 0U},
      {make_translation(0.0f, 0.0f, 8.0f), 0U, slot_tall,
       {0.0f, 0.0f, 8.0f, 1.05f}, 2U, 0U},
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

  // ---- Shared descriptor set + material/texture sets ----
  omnicpp::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());

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
  VkImage white_image = VK_NULL_HANDLE;
  VkImageView white_view = VK_NULL_HANDLE;
  VkSampler white_sampler = VK_NULL_HANDLE;
  omnicpp::render::Allocation white_alloc{};
  ASSERT_TRUE(make_white_texture(context.device(), context.physical_device(),
                                 context.graphics_queue(), qf, allocator,
                                 white_image, white_view, white_sampler,
                                 white_alloc));
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

  // ---- Pipelines (identical to the proven harness path) ----
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

  // ---- ONE renderer call records the whole frame ----
  const omnicpp::render::SceneMatrix vp =
      make_perspective(45.0f, 1.0f, 0.1f, 100.0f);
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
  } cull_push_data{};
  cull_push_data.object_count = kObjectCount;
  cull_push_data.sphere_word = kSphereWord;
  cull_push_data.draw_word = kDrawWord;
  cull_push_data.visible_word = kVisibleWord;
  // Inward side planes pass through the camera: z-component = -tan_half.
  const float th = 0.41421356f;
  const float planes[6][4] = {
      {-1, 0, -th, 0.0f}, {1, 0, -th, 0.0f},  {0, -1, -th, 0.0f},
      {0, 1, -th, 0.0f},  {0, 0, -1, -0.1f}, {0, 0, 1, 100.0f},
  };
  std::memcpy(cull_push_data.planes, planes, sizeof(planes));
  cull_push_data.tan_half_fov = th;
  cull_push_data.viewport_h = static_cast<float>(kSize);
  cull_push_data.lod_threshold_count = 2U;
  const float thresholds[2] = {80.0f, 0.0f};
  std::memcpy(cull_push_data.lod_thresholds, thresholds, sizeof(thresholds));

  struct GfxPush {
    omnicpp::render::SceneMatrix view_projection;
    omnicpp::render::SceneMatrix model_unused;
    std::array<float, 4> camera_position;
    std::uint32_t material_index_unused;
    std::array<std::uint32_t, 3> pad{};
  } gfx_push_data{vp, {}, {0.0f, 0.0f, 0.0f, 1.0f}, 0U, {}};

  omnicpp::render::VulkanRenderer::GpuDrivenFrame frame{};
  frame.cull_pipeline = cull_pipe.pipeline();
  frame.cull_pipeline_layout = cull_pipe.pipeline_layout();
  frame.cull_set = driven_set.value();
  frame.object_count = kObjectCount;
  frame.cull_push = {&cull_push_data, sizeof(cull_push_data)};
  frame.indirect_buffer = draw_buf.value().buffer;
  frame.draw_pipeline = gfx_pipe.pipeline();
  frame.draw_pipeline_layout = gfx_pipe.pipeline_layout();
  frame.draw_sets[0] = driven_set.value();
  frame.draw_sets[1] = tex_set.value();
  frame.draw_sets[2] = mat_set.value();
  frame.draw_set_count = 3U;
  frame.draw_push = {&gfx_push_data, sizeof(gfx_push_data)};
  frame.index_buffer = ib.value().buffer;
  frame.render_pass = target.render_pass();
  frame.framebuffer = target.framebuffer();
  frame.width = kSize;
  frame.height = kSize;
  VkClearValue clears[2]{};
  clears[0].color = {{0, 0, 0, 0}};  // transparent black: readback convention
  clears[1].depthStencil = {1.0f, 0U};
  frame.clear_values = clears;
  frame.clear_value_count = 2U;

  omnicpp::render::VulkanRenderer renderer;
  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool.value());
  ASSERT_TRUE(cbr.is_ok());
  VkCommandBuffer cb = cbr.value();
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
  ASSERT_TRUE(
      renderer.record_pbr_frame_gpu_driven(cb, frame).is_ok());
  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);

  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(context.device(), &fi, nullptr, &fence);
  VkSubmitInfo sub{};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1U;
  sub.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(context.graphics_queue(), 1U, &sub, fence),
            VK_SUCCESS);
  ASSERT_EQ(
      vkWaitForFences(context.device(), 1U, &fence, VK_TRUE, UINT64_MAX),
      VK_SUCCESS);
  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool.value(), nullptr);

  // ---- Proof 1: GPU-written commands ----
  const auto* words = static_cast<const std::uint32_t*>(draw_buf.value().mapped);
  const auto* cmds = words + kDrawWord;
  const std::uint32_t tall_count = merged.entries[slot_tall].index_count;
  const std::uint32_t short_count = merged.entries[slot_short].index_count;
  EXPECT_EQ(cmds[0], tall_count);
  EXPECT_EQ(cmds[1], 1U);
  EXPECT_EQ(cmds[2], merged.entries[slot_tall].index_offset);
  EXPECT_EQ(cmds[4], 0U);
  EXPECT_EQ(cmds[5], short_count);
  EXPECT_EQ(cmds[6], 1U);
  EXPECT_EQ(cmds[7], merged.entries[slot_short].index_offset);
  EXPECT_EQ(cmds[9], 1U);
  EXPECT_EQ(cmds[10], 0U);
  EXPECT_EQ(cmds[11], 0U);
  EXPECT_EQ(words[kVisibleWord], 2U);

  // ---- Proof 2: pixel structure matches the harness path ----
  const auto pixels_result = omnicpp_test::readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      qf, target.image(), target.format(), kSize, kSize,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      /*store_pixels=*/true);
  ASSERT_TRUE(pixels_result.submitted);
  ASSERT_EQ(pixels_result.pixels.size(),
            static_cast<std::size_t>(kSize) * kSize);

  const std::size_t near_run =
      count_red_run(pixels_result.pixels, kSize, kSize, 128U);
  const std::size_t far_run =
      count_red_run(pixels_result.pixels, kSize, kSize, 176U);
  const std::size_t behind_run =
      count_red_run(pixels_result.pixels, kSize, kSize, 92U);

  std::printf("\n[FRAME] near=%zu far=%zu behind=%zu\n", near_run, far_run,
              behind_run);
  std::fflush(stdout);

  EXPECT_GT(near_run, 180U) << "near bar must draw the LOD 0 mesh";
  EXPECT_LT(near_run, 235U);
  EXPECT_GT(far_run, 12U) << "far bar should be visible at LOD 1";
  EXPECT_LT(far_run, 30U)
      << "far bar must draw the LOD 1 mesh (LOD 0 would be ~39 px)";
  EXPECT_EQ(behind_run, 0U) << "behind-camera bar must be culled on the GPU";
  EXPECT_GT(pixels_result.non_clear_pixels, 5000U);

  // Cleanup
  if (white_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(context.device(), white_sampler, nullptr);
  }
  if (white_view != VK_NULL_HANDLE) {
    vkDestroyImageView(context.device(), white_view, nullptr);
  }
  if (white_image != VK_NULL_HANDLE) {
    vkDestroyImage(context.device(), white_image, nullptr);
  }
  allocator.destroy_allocation(white_alloc);
  gfx_pipe.cleanup(context.device());
  cull_pipe.cleanup(context.device());
  target.cleanup(context.device());
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
}

#else  // !WARPLOOM_HAS_VULKAN

TEST(VulkanHardware, RendererGpuDrivenFrameOneSubmission) {
  GTEST_SKIP() << "Vulkan unavailable";
}

#endif  // WARPLOOM_HAS_VULKAN
