//! @file test_gpu_driven_occlusion_frame.cpp
//! @brief Occlusion culling inside the renderer-owned one-submission GPU-
//!        driven frame. Same record_pbr_frame_gpu_driven path as
//!        test_gpu_driven_frame.cpp, but the cull pipeline's shader is the
//!        H-Z variant (cull_and_draw_lod_occlude.comp) with a packed max-
//!        depth pyramid bound at set 0 / binding 4. Two rounds in ONE
//!        command buffer: round 1 occlusion disabled (bar renders); round
//!        2 enabled with a pyramid that covers the far bar's projection
//!        with NEARER depth (bar GPU-culled; commands read back degenerate
//!        and pixels disappear). Near bar's pyramid tiles stay empty, so it
//!        must survive round 2 — proving coverage, not just sampling.

#include <gtest/gtest.h>

#include <bit>
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
//! Also returns the packed NDC depth constants (C1 - C2/d) so the pyramid
//! producer and the cull shader agree on the exact depth mapping.
omnicpp::render::SceneMatrix make_perspective(float fov_y, float aspect,
                                              float znear, float zfar) {
  const float f =
      1.0f / std::tan(fov_y * 3.14159265f / 360.0f);
  const float zn = 1.0f / (znear - zfar);
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

//! Exact NDC depth of a view-space distance d under the projection above.
float ndc_depth(float view_d, float znear, float zfar) {
  const float inv_range = 1.0f / (zfar - znear);
  return (zfar + znear) * inv_range -
         2.0f * zfar * znear * inv_range / view_d;
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
bool make_white_texture(VkDevice device, VkPhysicalDevice physical_device,
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

TEST(VulkanHardware, RendererGpuDrivenOcclusionFrameOneSubmission) {
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppGpuDrivenOcclFrame", true).is_ok());
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
       VK_SHADER_STAGE_COMPUTE_BIT},
      {0U, 4U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_COMPUTE_BIT}};
  auto driven_layout = descriptors.create_layout(driven_bindings, 8U);
  ASSERT_TRUE(driven_layout.is_ok());

  // ---- H-Z pyramid: 8x8 tiles of packed max depth over the 256x256 view.
  // Tiles are derived with the shader's exact projection math:
  //   ndc = center.xy / (depth * tan_half);  tile = floor((ndc*0.5+0.5)*8)
  // Far bar (2.5, 0, -16), r=1.05: L0 tiles (5,3) and (5,4) get a blocker
  // at view depth 8 (NEARER than the bar's nearest point ~14.95) -> culled.
  // Near bar (0, 0, -3), r=1.05: footprint-adaptive level selects L2 (2x2
  // grid); its own surface depth fills its L0 tiles (4,3),(4,4) so the
  // quarter-tile maxima stay >= its nearest depth -> never occluded.
  constexpr std::uint32_t kPyramidOff = 0U;
  constexpr std::uint32_t kTilesX = 8U;
  constexpr std::uint32_t kTilesY = 8U;
  auto pyramid_buf = allocator.create_buffer(
      kTilesX * kTilesY * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(pyramid_buf.is_ok());
  {
    const float th = 0.41421356f;  // tan(22.5 deg), matches the projection
    auto* tiles = static_cast<std::uint32_t*>(pyramid_buf.value().mapped);
    std::memset(tiles, 0, kTilesX * kTilesY * 4U);
    const auto cover = [&](float cx, float cz, float radius, float depth) {
      // Shader mapping: ndc = xy/(depth*tan_half); uv = ndc*0.5+0.5;
      // tile = floor(uv * tile_count); uv half-extent = ndc half * 0.5.
      const float ndc_x = cx / (cz * th);
      const float ndc_y = 0.0f;
      const float half = radius * th / cz;  // NDC half-extent
      const float uv_min_x = (ndc_x - half) * 0.5f + 0.5f;
      const float uv_max_x = (ndc_x + half) * 0.5f + 0.5f;
      const float uv_min_y = (ndc_y - half) * 0.5f + 0.5f;
      const float uv_max_y = (ndc_y + half) * 0.5f + 0.5f;
      const uint32_t tx_min = static_cast<uint32_t>(
          std::clamp(uv_min_x * float(kTilesX), 0.0f, float(kTilesX - 1)));
      const uint32_t tx_max = static_cast<uint32_t>(
          std::clamp(uv_max_x * float(kTilesX), 0.0f, float(kTilesX - 1)));
      const uint32_t ty_min = static_cast<uint32_t>(
          std::clamp(uv_min_y * float(kTilesY), 0.0f, float(kTilesY - 1)));
      const uint32_t ty_max = static_cast<uint32_t>(
          std::clamp(uv_max_y * float(kTilesY), 0.0f, float(kTilesY - 1)));
      for (uint32_t ty = ty_min; ty <= ty_max; ++ty) {
        for (uint32_t tx = tx_min; tx <= tx_max; ++tx) {
          tiles[ty * kTilesX + tx] = std::bit_cast<std::uint32_t>(depth);
        }
      }
    };
    // Blocker wall segment between the camera and the far bar (depth 8,
    // NDC ~0.977 < the bar's nearest ~0.989): fills the bar's L0 footprint.
    cover(2.5f, 16.0f, 1.05f, ndc_depth(8.0f, 0.1f, 100.0f));
    // The near bar's own rendered surface (depth 3, NDC ~0.935 >= its
    // nearest ~0.900): its L0 footprint keeps every L2 quarter-tile max
    // >= its nearest depth, so the adaptive test can never occlude it.
    cover(0.0f, 3.0f, 1.05f, ndc_depth(3.0f, 0.1f, 100.0f));
  }
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
  ASSERT_TRUE(descriptors
                  .write_buffer(driven_set.value(), 4U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                pyramid_buf.value().buffer, 0U, VK_WHOLE_SIZE)
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
                  .load_shader_stage_file(
                      context.device(),
                      sd + "/cull_and_draw_lod_occlude.comp.spv", "compute")
                  .is_ok());
  // Push layout: base 8 words + planes (96 B) + 4 floats + 6 occl words.
  const VkPushConstantRange cull_push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 164U};
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
    // --- occlusion extension ---
    std::uint32_t occl_enable;
    std::uint32_t tile_count_x;
    std::uint32_t tile_count_y;
    std::uint32_t pyramid_off;
    std::uint32_t near_z;
    std::uint32_t far_z;
  } cull_push_data{};
  static_assert(sizeof(CullPush) == 164U, "push size drift");
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
  auto cbr1 = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool.value());
  auto cbr2 = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool.value());
  ASSERT_TRUE(cbr1.is_ok() && cbr2.is_ok());

  // Occlusion ABI fields shared by both rounds (round 1 keeps the pyramid
  // bound but DISABLED — proves the enable bit, not the buffer presence).
  cull_push_data.tile_count_x = kTilesX;
  cull_push_data.tile_count_y = kTilesY;
  cull_push_data.pyramid_off = kPyramidOff;
  cull_push_data.near_z = std::bit_cast<std::uint32_t>(0.1f);
  cull_push_data.far_z = std::bit_cast<std::uint32_t>(100.0f);

  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(context.device(), &fi, nullptr, &fence);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkSubmitInfo sub{};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.commandBufferCount = 1U;

  const auto submit_frame = [&](VkCommandBuffer cb) {
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    ASSERT_TRUE(renderer.record_pbr_frame_gpu_driven(cb, frame).is_ok());
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    sub.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(context.graphics_queue(), 1U, &sub, fence),
              VK_SUCCESS);
    EXPECT_EQ(
        vkWaitForFences(context.device(), 1U, &fence, VK_TRUE, UINT64_MAX),
        VK_SUCCESS);
    EXPECT_EQ(vkResetFences(context.device(), 1U, &fence), VK_SUCCESS);
  };

  // ---- Round 1: occlusion DISABLED — far bar renders at LOD 1 -----------
  cull_push_data.occl_enable = 0U;
  submit_frame(cbr1.value());

  // Proof 1: GPU-written commands (same structure as the base frame test).
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

  // Proof 2: pixels — near and far bars both present.
  const auto pix1 = omnicpp_test::readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      qf, target.image(), target.format(), kSize, kSize,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*store_pixels=*/true);
  ASSERT_TRUE(pix1.submitted);
  ASSERT_EQ(pix1.pixels.size(), static_cast<std::size_t>(kSize) * kSize);
  const std::size_t near_run1 =
      count_red_run(pix1.pixels, kSize, kSize, 128U);
  const std::size_t far_run1 =
      count_red_run(pix1.pixels, kSize, kSize, 176U);
  EXPECT_GT(near_run1, 180U);
  EXPECT_GT(far_run1, 12U) << "far bar must render with occlusion disabled";

  // ---- Round 2: occlusion ENABLED — far bar GPU-culled ------------------
  // Reset the counter (atomicAdd accumulates across frames); commands are
  // rewritten absolutely by the compute pass every round.
  std::memset(draw_buf.value().mapped, 0, (kVisibleWord + 1U) * 4U);
  cull_push_data.occl_enable = 1U;
  submit_frame(cbr2.value());

  // Proof 3: commands — far bar degenerate, near bar intact, visible == 1.
  EXPECT_EQ(cmds[0], tall_count) << "near bar survives (its tiles are empty)";
  EXPECT_EQ(cmds[5], 0U) << "far bar must be occlusion-culled on the GPU";
  EXPECT_EQ(cmds[6], 0U);
  EXPECT_EQ(words[kVisibleWord], 1U);

  // Proof 4: pixels — far bar gone, near bar unchanged.
  const auto pix2 = omnicpp_test::readback_swapchain_image(
      context.physical_device(), context.device(), context.graphics_queue(),
      qf, target.image(), target.format(), kSize, kSize,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*store_pixels=*/true);
  ASSERT_TRUE(pix2.submitted);
  const std::size_t near_run2 =
      count_red_run(pix2.pixels, kSize, kSize, 128U);
  const std::size_t far_run2 =
      count_red_run(pix2.pixels, kSize, kSize, 176U);
  const std::size_t behind_run2 =
      count_red_run(pix2.pixels, kSize, kSize, 92U);

  std::printf("\n[OCCL] round1 near=%zu far=%zu | round2 near=%zu far=%zu "
              "behind=%zu\n",
              near_run1, far_run1, near_run2, far_run2, behind_run2);
  std::fflush(stdout);

  EXPECT_GT(near_run2, 180U) << "near bar must survive occlusion";
  EXPECT_EQ(far_run2, 0U) << "far bar pixels must disappear";
  EXPECT_EQ(behind_run2, 0U);
  EXPECT_GT(pix2.non_clear_pixels, 4000U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool.value(), nullptr);

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
  omnicpp::render::Allocation pyramid_alloc = pyramid_buf.value();
  allocator.destroy_allocation(pyramid_alloc);
  gfx_pipe.cleanup(context.device());
  cull_pipe.cleanup(context.device());
  target.cleanup(context.device());
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
}

#else  // !WARPLOOM_HAS_VULKAN

TEST(VulkanHardware, RendererGpuDrivenOcclusionFrameOneSubmission) {
  GTEST_SKIP() << "Vulkan unavailable";
}

#endif  // WARPLOOM_HAS_VULKAN
