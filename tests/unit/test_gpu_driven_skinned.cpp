//! @file test_gpu_driven_skinned.cpp
//! @brief R3 parity proof: the GPU-driven skinned pipeline
//!        (pbr_gpu_driven_skinned.vert — vertex-pull + linear-blend skinning,
//!        palette join keyed off payload word 23) must produce BYTE-IDENTICAL
//!        pixels to the CPU-skinned path (record_pbr_scene + skinned_scene.vert
//!        + pbr_scene.frag) for the same scene.
//!
//! Scene: one static cube (control — exercises the partitioned static draw)
//! plus one two-segment beam whose top half is bound to joint 1 (translated
//! +0.5x). The displacement is the signal: an identity-skin bug, a stalled
//! joint_base, or a neutered palette leaves the top half in place and the
//! parity fails.
//!
//! Path A (reference): record_pbr_scene with default + skinned pipelines.
//! Path B (driven):    TWO vkCmdDrawIndexedIndirect calls over one command
//!                     buffer — static commands through pbr_gpu_driven.vert,
//!                     skinned commands through pbr_gpu_driven_skinned.vert
//!                     (same pbr_gpu_driven.frag). Objects are partitioned
//!                     static-first; firstInstance still addresses the payload.
//!                     The cull pass is NOT exercised (unchanged by R3 — it
//!                     never reads the skin stream); commands are the CPU's,
//!                     in the cull convention (plus one degenerate).
//!
//! Sabotage (both must break parity): (a) neutered palette — joint 1 set to
//! identity, so the top half never displaces; (b) top-half joints rebound to
//! joint 0, same effect through the skin stream. A test that passes either
//! sabotage proves nothing about skinning.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
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

using warploom::render::SceneMatrix;

constexpr std::uint32_t kSize = 256U;

//! Two stacked quads in the XY plane (z = 0), 8 vertices, 12 indices.
//! Bottom quad (verts 0..3, y in {0,1}) binds joint 0; top quad (verts
//! 4..7, y in {1,2}) binds joint 1. Winding matches the +Z faces in
//! test_gpu_driven_ab.cpp (front-facing toward a camera looking down -Z
//! with culling on).
void build_beam_11(std::vector<float>& vertices,
                   std::vector<std::uint32_t>& indices) {
  vertices.clear();
  indices.clear();
  const float xs[4] = {-0.5f, 0.5f, 0.5f, -0.5f};
  for (int half = 0; half < 2; ++half) {
    const float y0 = half == 0 ? 0.0f : 1.0f;
    const float ys[4] = {y0, y0, y0 + 1.0f, y0 + 1.0f};
    const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int c = 0; c < 4; ++c) {
      vertices.push_back(xs[c]);
      vertices.push_back(ys[c]);
      vertices.push_back(0.0f);
      vertices.push_back(1.0f);
      vertices.push_back(1.0f);
      vertices.push_back(1.0f);
      vertices.push_back(0.0f);
      vertices.push_back(0.0f);
      vertices.push_back(1.0f);
      vertices.push_back(uvs[c][0]);
      vertices.push_back(uvs[c][1]);
    }
  }
  const std::uint32_t quads[4] = {0, 1, 2, 0};
  const std::uint32_t quads2[4] = {0, 2, 3, 0};  // placeholder, unused
  (void)quads2;
  for (int half = 0; half < 2; ++half) {
    const std::uint32_t b = static_cast<std::uint32_t>(half) * 4U;
    indices.insert(indices.end(),
                   {b + quads[0], b + quads[1], b + quads[2], b + quads[0],
                    b + quads[2], b + 3U});
  }
}

//! Skin stream for the beam: 8 floats/vertex, bottom half joint 0, top
//! half joint 1, all weights (1,0,0,0) — a rigid step, not a bend. The
//! step is deliberate: any palette/indexing bug moves whole quads, which
//! pixels cannot hide.
void build_beam_skin(std::vector<float>& skin) {
  skin.assign(8U * 8U, 0.0f);
  for (std::uint32_t v = 0; v < 8U; ++v) {
    const float joint = v < 4U ? 0.0f : 1.0f;
    skin[v * 8U + 0U] = joint;
    skin[v * 8U + 4U] = 1.0f;
  }
}

//! Unit cube spanning [-1,1]^3 (copied from test_gpu_driven_ab.cpp — the
//! control object must be the same mesh both paths already agree on).
void build_unit_cube(std::vector<float>& vertices,
                     std::vector<std::uint32_t>& indices) {
  vertices.clear();
  indices.clear();
  const float f[6][4][3] = {
      {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
      {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
      {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},
      {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
      {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},
      {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
  };
  const float n[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0},
                         {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
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
    const std::uint32_t tri[6] = {0, 1, 2, 0, 2, 3};
    for (std::size_t k = 0; k < static_cast<std::size_t>(6); ++k)
      indices.push_back(base + tri[k]);
  }
}

warploom::render::SceneMatrix make_perspective(float fov_y_degrees, float aspect,
                                              float znear, float zfar) {
  SceneMatrix m = warploom::render::scene_identity_matrix();
  const float f = 1.0f / std::tan(fov_y_degrees * 3.14159265f / 360.0f);
  m[0] = f / aspect;
  m[5] = -f;
  m[10] = (zfar + znear) / (znear - zfar);
  m[11] = -1.0f;
  m[14] = (2.0f * zfar * znear) / (znear - zfar);
  m[15] = 0.0f;
  return m;
}

warploom::render::SceneMatrix make_translation(float x, float y, float z) {
  SceneMatrix m = warploom::render::scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

struct SolidTexture {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  warploom::render::Allocation allocation{};
};

bool make_solid_texture(VkDevice device, VkQueue queue,
                        std::uint32_t queue_family,
                        warploom::render::VulkanMemoryAllocator& allocator,
                        const std::uint8_t (&rgba)[4], SolidTexture& out) {
  out = {};
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
  warploom::render::Allocation image_mem = mem.value();
  out.allocation = image_mem;
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &vi, nullptr, &out.view) != VK_SUCCESS) {
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
  if (vkCreateSampler(device, &si, nullptr, &out.sampler) != VK_SUCCESS) {
    vkDestroyImageView(device, out.view, nullptr);
    allocator.destroy_allocation(image_mem);
    vkDestroyImage(device, image, nullptr);
    return false;
  }
  out.image = image;
  auto pool = warploom::render::VulkanRenderer::create_command_pool(device,
                                                                   queue_family);
  if (!pool.is_ok()) return false;
  auto cbr = warploom::render::VulkanRenderer::allocate_command_buffer(
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
    color.float32[0] = rgba[0] / 255.0f;
    color.float32[1] = rgba[1] / 255.0f;
    color.float32[2] = rgba[2] / 255.0f;
    color.float32[3] = rgba[3] / 255.0f;
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

void destroy_solid_texture(VkDevice device,
                           warploom::render::VulkanMemoryAllocator& allocator,
                           SolidTexture& t) {
  if (t.sampler != VK_NULL_HANDLE) vkDestroySampler(device, t.sampler, nullptr);
  if (t.view != VK_NULL_HANDLE) vkDestroyImageView(device, t.view, nullptr);
  if (t.allocation.is_valid()) allocator.destroy_allocation(t.allocation);
  if (t.image != VK_NULL_HANDLE) vkDestroyImage(device, t.image, nullptr);
  t = {};
}

//! 24-word payload, mirroring pbr_gpu_driven.vert. Word 23 is the R3
//! joint_base (0 for both objects here: one actor at palette slot 0).
struct Payload {
  warploom::render::SceneMatrix model;
  std::uint32_t material_index;
  std::uint32_t mesh_slot;
  std::array<float, 4> sphere;
  std::uint32_t lod_count;
  std::uint32_t joint_base;
};
static_assert(sizeof(Payload) == 96U, "24 words expected");

}  // namespace

TEST(VulkanHardware, GpuDrivenSkinnedMatchesCpuSkinnedPixels) {
  if (!warploom::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  warploom::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("WarploomGpuDrivenSkinned", true).is_ok());
  warploom::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device()).is_ok());
  warploom::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);

  // ---- Geometry -----------------------------------------------------------
  std::vector<float> cube_verts, beam_verts, beam_skin;
  std::vector<std::uint32_t> cube_idx, beam_idx;
  build_unit_cube(cube_verts, cube_idx);
  build_beam_11(beam_verts, beam_idx);
  build_beam_skin(beam_skin);

  // Path B shared streams through the table builder (R3 skin stream).
  warploom::render::SceneMeshTableBuilder table_builder;
  warploom::render::SceneMesh shell{};
  const auto slot_cube =
      table_builder.add(shell, cube_verts, cube_idx, std::vector<float>{});
  const auto slot_beam = table_builder.add(shell, beam_verts, beam_idx,
                                           beam_skin);
  ASSERT_NE(slot_cube, warploom::render::SceneMeshTableBuilder::kInvalidSlot);
  ASSERT_NE(slot_beam, warploom::render::SceneMeshTableBuilder::kInvalidSlot);
  // Different geometry: no dedup between the slots.
  EXPECT_EQ(table_builder.build().dedup_count, 0U);
  const auto& merged = table_builder.build();
  ASSERT_EQ(merged.skin_data.size(),
            merged.vertex_data.size() / 11U * 8U);

  auto vb = allocator.create_buffer(
      merged.vertex_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto skin_buf = allocator.create_buffer(
      merged.skin_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto ib = allocator.create_buffer(
      merged.index_bytes(),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(vb.is_ok() && skin_buf.is_ok() && ib.is_ok());
  std::memcpy(vb.value().mapped, merged.vertex_data.data(),
              merged.vertex_bytes());
  std::memcpy(skin_buf.value().mapped, merged.skin_data.data(),
              merged.skin_bytes());
  std::memcpy(ib.value().mapped, merged.index_data.data(),
              merged.index_bytes());

  // Path A beam mesh: CPU skinned layout = [all static 11N][skin 8N],
  // exactly as skinned_scene.vert reads it (static at v*11, skin block at
  // 11*N + v*8). NOT per-vertex interleaved: the static stream must stay
  // 11-packed or every vertex past v0 reads joints as positions.
  std::vector<float> beam_19 = beam_verts;
  beam_19.insert(beam_19.end(), beam_skin.begin(), beam_skin.end());
  auto beam_a_vb = allocator.create_buffer(
      beam_19.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto beam_a_ib = allocator.create_buffer(
      beam_idx.size() * sizeof(std::uint32_t),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto cube_a_vb = allocator.create_buffer(
      cube_verts.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto cube_a_ib = allocator.create_buffer(
      cube_idx.size() * sizeof(std::uint32_t),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(beam_a_vb.is_ok() && beam_a_ib.is_ok() && cube_a_vb.is_ok() &&
              cube_a_ib.is_ok());
  std::memcpy(beam_a_vb.value().mapped, beam_19.data(),
              beam_19.size() * sizeof(float));
  std::memcpy(beam_a_ib.value().mapped, beam_idx.data(),
              beam_idx.size() * sizeof(std::uint32_t));
  std::memcpy(cube_a_vb.value().mapped, cube_verts.data(),
              cube_verts.size() * sizeof(float));
  std::memcpy(cube_a_ib.value().mapped, cube_idx.data(),
              cube_idx.size() * sizeof(std::uint32_t));

  // ---- Bones: joint 0 identity, joint 1 translate +0.5x ------------------
  std::array<float, 32> bones{};
  bones[0] = 1.0f;
  bones[5] = 1.0f;
  bones[10] = 1.0f;
  bones[15] = 1.0f;
  bones[16] = 1.0f;
  bones[21] = 1.0f;
  bones[26] = 1.0f;
  bones[28] = 0.5f;  // translation x
  bones[31] = 1.0f;
  auto bone_buf = allocator.create_buffer(
      sizeof(bones), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(bone_buf.is_ok());
  std::memcpy(bone_buf.value().mapped, bones.data(), sizeof(bones));

  // ---- Payload + indirect commands (path B) --------------------------------
  // Objects partitioned static-first: [0] cube, [1] beam. The static draw
  // covers command 0, the skinned draw command 1 — the R3 partition.
  constexpr std::uint32_t kObjectCount = 3U;  // 2 drawn + 1 degenerate
  constexpr std::uint32_t kPayloadWords = 2U + 24U * kObjectCount;
  auto payload_buf = allocator.create_buffer(
      kPayloadWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto draw_buf = allocator.create_buffer(
      kObjectCount * 20U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(payload_buf.is_ok() && draw_buf.is_ok());

  const SceneMatrix model_cube = make_translation(-1.6f, 0.0f, -4.5f);
  const SceneMatrix model_beam = make_translation(1.6f, -0.5f, -4.5f);
  const Payload payloads[kObjectCount] = {
      {model_cube, 0U, slot_cube, {-1.6f, 0.0f, -4.5f, 0.8660254f}, 1U, 0U},
      // Beam sphere covers the displaced top half (x +0..1 local): center
      // (1.85, 0.5), radius 1.25 — the conservative-sphere contract.
      {model_beam, 1U, slot_beam, {1.85f, 0.5f, -4.5f, 1.25f}, 1U, 0U},
      {warploom::render::scene_identity_matrix(), 0U, slot_cube,
       {0.0f, 0.0f, 0.0f, 0.8660254f}, 1U, 0U},
  };
  {
    auto* words = static_cast<std::uint32_t*>(payload_buf.value().mapped);
    words[0] = kObjectCount;
    words[1] = 0U;
    std::memcpy(words + 2U, payloads, sizeof(payloads));
    auto* cmds =
        static_cast<VkDrawIndexedIndirectCommand*>(draw_buf.value().mapped);
    cmds[0] = {merged.entries[slot_cube].index_count, 1U,
               merged.entries[slot_cube].index_offset, 0U, 0U};
    cmds[1] = {merged.entries[slot_beam].index_count, 1U,
               merged.entries[slot_beam].index_offset, 0U, 1U};
    cmds[2] = {0U, 0U, 0U, 0U, 2U};  // degenerate, cull convention
  }

  // ---- Materials: gray cube, RED beam (the displaced signal) ---------------
  warploom::render::PbrMaterialData mats[2]{};
  mats[0].base_color_factor = {0.75f, 0.75f, 0.75f, 1.0f};
  mats[0].roughness_factor = 0.5f;
  auto mat_buf = allocator.create_buffer(
      2U * sizeof(warploom::render::PbrMaterialData),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(mat_buf.is_ok());
  mats[1].base_color_factor = {0.9f, 0.1f, 0.1f, 1.0f};
  mats[1].roughness_factor = 0.5f;
  std::memcpy(mat_buf.value().mapped, mats, sizeof(mats));

  // ---- Descriptors ----------------------------------------------------------
  const std::vector<warploom::render::ReflectedBinding> mesh_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT}};
  auto mesh_layout = descriptors.create_layout(mesh_bindings, 8U);
  ASSERT_TRUE(mesh_layout.is_ok());
  // Driven set 0: verts (0), payload (1), skin stream (4). Binding 4 is
  // unused by the static vertex shader — legal, and one set serves both
  // driven pipelines.
  const std::vector<warploom::render::ReflectedBinding> driven_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT},
      {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT},
      {0U, 4U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT}};
  auto driven_layout = descriptors.create_layout(driven_bindings, 8U);
  ASSERT_TRUE(driven_layout.is_ok());
  const std::vector<warploom::render::ReflectedBinding> tex_bindings = {
      {1U, 0U, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto tex_layout = descriptors.create_layout(tex_bindings, 1U, true);
  ASSERT_TRUE(tex_layout.is_ok());
  const std::vector<warploom::render::ReflectedBinding> mat_bindings = {
      {2U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto mat_layout = descriptors.create_layout(mat_bindings, 8U);
  ASSERT_TRUE(mat_layout.is_ok());
  const std::vector<warploom::render::ReflectedBinding> bone_bindings = {
      {3U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT}};
  auto bone_layout = descriptors.create_layout(bone_bindings, 8U);
  ASSERT_TRUE(bone_layout.is_ok());

  auto cube_mesh_set = descriptors.allocate_set(mesh_layout.value());
  auto beam_mesh_set = descriptors.allocate_set(mesh_layout.value());
  auto driven_set = descriptors.allocate_set(driven_layout.value());
  auto tex_set = descriptors.allocate_set(tex_layout.value());
  auto mat_set = descriptors.allocate_set(mat_layout.value());
  auto bone_set = descriptors.allocate_set(bone_layout.value());
  ASSERT_TRUE(cube_mesh_set.is_ok() && beam_mesh_set.is_ok() &&
              driven_set.is_ok() && tex_set.is_ok() && mat_set.is_ok() &&
              bone_set.is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(cube_mesh_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                cube_a_vb.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(beam_mesh_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                beam_a_vb.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
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
                  .write_buffer(driven_set.value(), 4U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                skin_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(mat_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                mat_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(bone_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                bone_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());

  SolidTexture white{};
  const std::uint8_t white_rgba[4] = {255U, 255U, 255U, 255U};
  ASSERT_TRUE(make_solid_texture(context.device(), context.graphics_queue(),
                                 qf, allocator, white_rgba, white));
  ASSERT_TRUE(descriptors
                  .write_image(tex_set.value(), 0U,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               white.sampler, white.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
                  .is_ok());

  // ---- Targets ---------------------------------------------------------------
  warploom::render::VulkanOffscreenTarget target_a, target_b;
  for (warploom::render::VulkanOffscreenTarget* t : {&target_a, &target_b}) {
    ASSERT_TRUE(t
                    ->create(context.device(), context.physical_device(),
                             VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize,
                             &allocator)
                    .is_ok());
    ASSERT_TRUE(t
                    ->create_depth(context.device(), context.physical_device(),
                                   VK_FORMAT_D32_SFLOAT)
                    .is_ok());
    ASSERT_TRUE(t->create_render_pass(context.device()).is_ok());
    ASSERT_TRUE(t->create_framebuffer(context.device()).is_ok());
  }

  // ---- Pipelines ---------------------------------------------------------------
  warploom::render::VulkanPipeline pipe_a, pipe_a_skinned, pipe_b_static,
      pipe_b_skinned;
  const std::string sd = WARPLOOM_TEST_SHADER_DIR;
  const VkPushConstantRange push_all{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};
  // Path A static.
  ASSERT_TRUE(pipe_a
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_scene.vert.spv", "vertex")
                  .is_ok());
  ASSERT_TRUE(pipe_a
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_scene.frag.spv",
                                          "fragment")
                  .is_ok());
  const VkDescriptorSetLayout layouts_a[3] = {mesh_layout.value(),
                                              tex_layout.value(),
                                              mat_layout.value()};
  ASSERT_TRUE(
      pipe_a.create_pipeline_layout(context.device(), layouts_a, 3U, &push_all)
          .is_ok());
  ASSERT_TRUE(pipe_a
                  .create_graphics_pipeline(context.device(),
                                            target_a.render_pass(),
                                            target_a.format(),
                                            pipe_a.pipeline_layout(), true,
                                            true, false)
                  .is_ok());
  // Path A skinned (sets mesh/tex/mat/bones).
  ASSERT_TRUE(pipe_a_skinned
                  .load_shader_stage_file(context.device(),
                                          sd + "/skinned_scene.vert.spv",
                                          "vertex")
                  .is_ok());
  ASSERT_TRUE(pipe_a_skinned
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_scene.frag.spv",
                                          "fragment")
                  .is_ok());
  const VkDescriptorSetLayout layouts_as[4] = {
      mesh_layout.value(), tex_layout.value(), mat_layout.value(),
      bone_layout.value()};
  ASSERT_TRUE(pipe_a_skinned
                  .create_pipeline_layout(context.device(), layouts_as, 4U,
                                          &push_all)
                  .is_ok());
  ASSERT_TRUE(pipe_a_skinned
                  .create_graphics_pipeline(context.device(),
                                            target_a.render_pass(),
                                            target_a.format(),
                                            pipe_a_skinned.pipeline_layout(),
                                            true, true, false)
                  .is_ok());
  // Path B static driven.
  ASSERT_TRUE(pipe_b_static
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_gpu_driven.vert.spv",
                                          "vertex")
                  .is_ok());
  ASSERT_TRUE(pipe_b_static
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_gpu_driven.frag.spv",
                                          "fragment")
                  .is_ok());
  const VkDescriptorSetLayout layouts_b[3] = {driven_layout.value(),
                                              tex_layout.value(),
                                              mat_layout.value()};
  ASSERT_TRUE(pipe_b_static
                  .create_pipeline_layout(context.device(), layouts_b, 3U,
                                          &push_all)
                  .is_ok());
  ASSERT_TRUE(pipe_b_static
                  .create_graphics_pipeline(context.device(),
                                            target_b.render_pass(),
                                            target_b.format(),
                                            pipe_b_static.pipeline_layout(),
                                            true, true, false)
                  .is_ok());
  // Path B skinned driven (adds set 3 bones).
  ASSERT_TRUE(pipe_b_skinned
                  .load_shader_stage_file(
                      context.device(),
                      sd + "/pbr_gpu_driven_skinned.vert.spv", "vertex")
                  .is_ok());
  ASSERT_TRUE(pipe_b_skinned
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_gpu_driven.frag.spv",
                                          "fragment")
                  .is_ok());
  const VkDescriptorSetLayout layouts_bs[4] = {
      driven_layout.value(), tex_layout.value(), mat_layout.value(),
      bone_layout.value()};
  ASSERT_TRUE(pipe_b_skinned
                  .create_pipeline_layout(context.device(), layouts_bs, 4U,
                                          &push_all)
                  .is_ok());
  ASSERT_TRUE(pipe_b_skinned
                  .create_graphics_pipeline(context.device(),
                                            target_b.render_pass(),
                                            target_b.format(),
                                            pipe_b_skinned.pipeline_layout(),
                                            true, true, false)
                  .is_ok());

  // ---- Path A scene ---------------------------------------------------------------
  const SceneMatrix vp = make_perspective(45.0f, 1.0f, 0.1f, 100.0f);
  warploom::render::SceneMesh cube_mesh_a, beam_mesh_a;
  cube_mesh_a.vertex_buffer = cube_a_vb.value().buffer;
  cube_mesh_a.index_buffer = cube_a_ib.value().buffer;
  cube_mesh_a.index_count = static_cast<std::uint32_t>(cube_idx.size());
  cube_mesh_a.descriptor_set = cube_mesh_set.value();
  beam_mesh_a.vertex_buffer = beam_a_vb.value().buffer;
  beam_mesh_a.index_buffer = beam_a_ib.value().buffer;
  beam_mesh_a.index_count = static_cast<std::uint32_t>(beam_idx.size());
  beam_mesh_a.descriptor_set = beam_mesh_set.value();
  warploom::render::VulkanPbrScene scene{};
  scene.pipeline = pipe_a.pipeline();
  scene.pipeline_layout = pipe_a.pipeline_layout();
  scene.skinned_pipeline = pipe_a_skinned.pipeline();
  scene.skinned_pipeline_layout = pipe_a_skinned.pipeline_layout();
  scene.camera.view_projection = vp;
  scene.camera_position = {0.0f, 0.0f, 0.0f, 1.0f};
  scene.texture_set = tex_set.value();
  scene.material_set = mat_set.value();
  scene.bone_set = bone_set.value();
  {
    warploom::render::ScenePbrObject cube_obj;
    cube_obj.mesh = &cube_mesh_a;
    cube_obj.model = model_cube;
    cube_obj.material_index = 0U;
    scene.objects.push_back(cube_obj);
    warploom::render::ScenePbrObject beam_obj;
    beam_obj.mesh = &beam_mesh_a;
    beam_obj.model = model_beam;
    beam_obj.material_index = 1U;
    beam_obj.joint_base = 0U;
    beam_obj.skinned = true;
    scene.objects.push_back(beam_obj);
  }

  struct PushB {
    warploom::render::SceneMatrix view_projection;
    warploom::render::SceneMatrix model_unused;
    std::array<float, 4> camera_position;
    std::uint32_t material_index_unused;
    std::array<std::uint32_t, 3> pad{};
  };

  // ---- Record + submit + readback ----------------------------------------------
  const auto run = [&](warploom::render::VulkanOffscreenTarget& target,
                       bool driven) -> warploom_test::ReadbackResult {
    auto pool = warploom::render::VulkanRenderer::create_command_pool(
        context.device(), qf);
    if (!pool.is_ok()) return {};
    auto cbr = warploom::render::VulkanRenderer::allocate_command_buffer(
        context.device(), pool.value());
    if (!cbr.is_ok()) {
      vkDestroyCommandPool(context.device(), pool.value(), nullptr);
      return {};
    }
    VkCommandBuffer cb = cbr.value();
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(context.device(), &fi, nullptr, &fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue clears[2]{};
    clears[0].color = {{0, 0, 0, 1}};
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

    if (!driven) {
      warploom::render::VulkanRenderer renderer;
      const auto recorded = renderer.record_pbr_scene(cb, scene, kSize, kSize);
      if (!recorded.is_ok()) {
        EXPECT_TRUE(false) << "record_pbr_scene failed";
        return {};
      }
    } else {
      // Partitioned draws through the engine recorder (R3 fields): command
      // 0 (cube) through the static pipeline, command 1 (beam) through the
      // skinned pipeline. firstInstance is the global object index, so the
      // payload addressing is unchanged; the degenerate tail command is
      // never drawn.
      const PushB push{vp, {}, {0.0f, 0.0f, 0.0f, 1.0f}, 0U, {}};
      warploom::render::VulkanRenderer renderer;
      warploom::render::VulkanRenderer::GpuDrivenFrame frame{};
      frame.object_count = kObjectCount;
      frame.indirect_buffer = draw_buf.value().buffer;
      frame.index_buffer = ib.value().buffer;
      frame.draw_push = {&push, static_cast<std::uint32_t>(sizeof(push))};
      // Static partition.
      frame.draw_pipeline = pipe_b_static.pipeline();
      frame.draw_pipeline_layout = pipe_b_static.pipeline_layout();
      frame.draw_sets[0] = driven_set.value();
      frame.draw_sets[1] = tex_set.value();
      frame.draw_sets[2] = mat_set.value();
      frame.draw_set_slots[0] = 0U;
      frame.draw_set_slots[1] = 1U;
      frame.draw_set_slots[2] = 2U;
      frame.draw_set_count = 3U;
      frame.draw_first_command = 0U;
      frame.draw_command_count = 1U;
      auto recorded = renderer.record_gpu_driven_draw(cb, kSize, kSize, frame);
      if (!recorded.is_ok()) {
        EXPECT_TRUE(false) << "record_gpu_driven_draw (static) failed";
        return {};
      }
      // Skinned partition: same sets plus the bone palette at slot 3.
      frame.draw_pipeline = pipe_b_skinned.pipeline();
      frame.draw_pipeline_layout = pipe_b_skinned.pipeline_layout();
      frame.draw_sets[3] = bone_set.value();
      frame.draw_set_slots[3] = 3U;
      frame.draw_set_count = 4U;
      frame.draw_first_command = 1U;
      frame.draw_command_count = 1U;
      recorded = renderer.record_gpu_driven_draw(cb, kSize, kSize, frame);
      if (!recorded.is_ok()) {
        EXPECT_TRUE(false) << "record_gpu_driven_draw (skinned) failed";
        return {};
      }
    }

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    vkQueueSubmit(context.graphics_queue(), 1U, &sub, fence);
    vkWaitForFences(context.device(), 1U, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(context.device(), fence, nullptr);
    vkDestroyCommandPool(context.device(), pool.value(), nullptr);
    return warploom_test::readback_swapchain_image(
        context.physical_device(), context.device(), context.graphics_queue(),
        qf, target.image(), target.format(), kSize, kSize,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  };

  const auto result_a = run(target_a, false);
  const auto result_b = run(target_b, true);
  ASSERT_TRUE(result_a.submitted);
  ASSERT_TRUE(result_b.submitted);
  std::printf(
      "\n[SKINNED-AB] A: non_clear=%zu red=%zu | B: non_clear=%zu red=%zu\n",
      result_a.non_clear_pixels, result_a.red_dominant_pixels,
      result_b.non_clear_pixels, result_b.red_dominant_pixels);
  std::fflush(stdout);
  EXPECT_GT(result_a.non_clear_pixels, 1000U)
      << "reference path should render cube + beam";
  EXPECT_GT(result_b.non_clear_pixels, 1000U)
      << "driven path should render cube + beam";
  // The beam is red and displaced +0.5x by joint 1: red pixels prove the
  // skinned draw emitted, on both paths.
  EXPECT_GT(result_a.red_dominant_pixels, 200U)
      << "reference: red beam missing";
  EXPECT_GT(result_b.red_dominant_pixels, 200U)
      << "driven: red beam missing (skin stream / palette addressing?)";

  // ---- Parity: byte-identical pixels --------------------------------------
  ASSERT_EQ(result_a.hash, result_b.hash)
      << "driven-skinned path must be pixel-identical to CPU-skinned path";

  // ---- Sabotage 1: neutered palette (joint 1 -> identity) ------------------
  // The top half no longer displaces. If parity still held, the test would
  // prove nothing about the palette.
  {
    // Float view: writing integer 1U into a float word is a denormal
    // (~zero), not 1.0f — a real trap this test once fell into, collapsing
    // the top half to a point instead of leaving it undisplaced.
    auto* bone_floats = static_cast<float*>(bone_buf.value().mapped);
    float saved[16];
    std::memcpy(saved, bone_floats + 16U, sizeof(saved));
    std::memset(bone_floats + 16U, 0, sizeof(saved));
    bone_floats[16U] = 1.0f;
    bone_floats[21U] = 1.0f;
    bone_floats[26U] = 1.0f;
    bone_floats[31U] = 1.0f;  // identity, no +0.5x translation
    const auto neutered = run(target_b, true);
    ASSERT_TRUE(neutered.submitted);
    EXPECT_NE(neutered.hash, result_a.hash)
        << "neutered palette must break parity (top half undisplaced)";
    std::memcpy(bone_floats + 16U, saved, sizeof(saved));
  }

  // ---- Sabotage 2: top-half joints rebound to joint 0 -----------------------
  // Same visible effect through the skin stream instead of the palette: if
  // the shader ignored the stream, parity would survive.
  {
    // Top-half joint words live at stream offsets [4..7]*8+0..3 (beam is
    // the second mesh in the builder: cube occupies verts 0..23).
    auto* skin_words = static_cast<float*>(skin_buf.value().mapped);
    const std::size_t beam_vert_base =
        merged.entries[slot_beam].vertex_base / 11U;
    float saved[4][4];
    for (std::uint32_t v = 4U; v < 8U; ++v) {
      for (std::uint32_t k = 0; k < 4U; ++k) {
        saved[v - 4U][k] = skin_words[(beam_vert_base + v) * 8U + k];
        skin_words[(beam_vert_base + v) * 8U + k] = 0.0f;
      }
    }
    const auto rebound = run(target_b, true);
    ASSERT_TRUE(rebound.submitted);
    EXPECT_NE(rebound.hash, result_a.hash)
        << "rebound joints must break parity (top half undisplaced)";
    for (std::uint32_t v = 4U; v < 8U; ++v) {
      for (std::uint32_t k = 0; k < 4U; ++k) {
        skin_words[(beam_vert_base + v) * 8U + k] = saved[v - 4U][k];
      }
    }
  }
  // Restore check: the image is the parity image again (proves the
  // sabotages acted through the buffers they claimed, not leftovers).
  {
    const auto restored = run(target_b, true);
    ASSERT_TRUE(restored.submitted);
    EXPECT_EQ(restored.hash, result_a.hash)
        << "restored buffers must reproduce the parity image";
  }

  destroy_solid_texture(context.device(), allocator, white);
  target_a.cleanup();
  target_b.cleanup();
  pipe_b_skinned.cleanup(context.device());
  pipe_b_static.cleanup(context.device());
  pipe_a_skinned.cleanup(context.device());
  pipe_a.cleanup(context.device());
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
}

#endif  // WARPLOOM_HAS_VULKAN
