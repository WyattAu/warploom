//! @file test_gpu_driven_ab.cpp
//! @brief GPU A/B parity proof for GPU-driven migration step 2: the
//!        vertex-pull pipeline (pbr_gpu_driven.{vert,frag}, payload via
//!        gl_InstanceIndex, geometry via global indices) must produce
//!        BYTE-IDENTICAL pixels to record_pbr_scene's per-draw path for
//!        the same scene. Two cubes, distinct materials, one rotated;
//!        the driven path also carries a third DEGENERATE draw command
//!        (indexCount = 0, the cull convention) that must contribute
//!        nothing.
//!
//! Path A (reference): pbr_scene.{vert,frag} + record_pbr_scene.
//! Path B (driven):    pbr_gpu_driven.{vert,frag} + ONE instanced
//!                     vkCmdDrawIndexedIndirect; object payloads live in
//!                     an SSBO; mesh-table entries supply firstIndex and
//!                     indexCount.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_mesh_table.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_scene.hpp"
#include "vulkan_test_readback.hpp"

#ifdef OMNICPP_HAS_VULKAN

namespace {

using omnicpp::render::SceneMatrix;

constexpr std::uint32_t kSize = 256U;
constexpr std::uint32_t kObjectCount = 3U;  // 2 drawn + 1 degenerate
constexpr std::uint32_t kPayloadWords = 2U + 18U * kObjectCount;

//! Unit cube spanning [-1,1]^3, per-face flat normals, white vertex color.
//! 11 floats/vertex (position.xyz, color.rgb, normal.xyz, uv.xy), 24
//! vertices, 36 indices — the engine's standard layout.
void build_unit_cube(std::vector<float>& vertices,
                     std::vector<std::uint32_t>& indices) {
  vertices.clear();
  indices.clear();
  const float f[6][4][3] = {
      {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},      // +Z
      {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},  // -Z
      {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},      // +X
      {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},  // -X
      {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},      // +Y
      {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},  // -Y
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
    const std::uint32_t tri[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) indices.push_back(base + tri[k]);
  }
}

// Column-major SceneMatrix helpers (flat float[16], matching the tests'
// scene-path convention: m[0..3] = column 0, m[12..14] = translation).

omnicpp::render::SceneMatrix make_perspective(float fov_y_degrees, float aspect, float znear,
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

omnicpp::render::SceneMatrix make_translation(float x, float y, float z) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

omnicpp::render::SceneMatrix make_rotation_y(float radians) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  m[0] = c;  m[2] = -s;
  m[8] = s;  m[10] = c;
  return m;
}

//! Column-major matrix multiply a * b.
omnicpp::render::SceneMatrix mat_mul(const SceneMatrix& a, const SceneMatrix& b) {
  SceneMatrix out{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      out[col * 4 + row] = sum;
    }
  }
  return out;
}

struct SolidTexture {
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  omnicpp::render::Allocation allocation{};
};

bool make_solid_texture(VkDevice device, VkPhysicalDevice physical_device,
                        VkQueue queue, std::uint32_t queue_family,
                        omnicpp::render::VulkanMemoryAllocator& allocator,
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
  // Keep a mutable local: destroy_allocation takes Allocation&.
  omnicpp::render::Allocation image_mem = mem.value();
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
  out.allocation = image_mem;

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
    barrier.srcAccessMask = 0;
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
                           omnicpp::render::VulkanMemoryAllocator& allocator,
                           SolidTexture& t) {
  if (t.sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device, t.sampler, nullptr);
    t.sampler = VK_NULL_HANDLE;
  }
  if (t.view != VK_NULL_HANDLE) {
    vkDestroyImageView(device, t.view, nullptr);
    t.view = VK_NULL_HANDLE;
  }
  if (t.allocation.is_valid()) {
    allocator.destroy_allocation(t.allocation);
    t.allocation = {};
  }
}

//! Object payload word layout (18 words), matching pbr_gpu_driven.vert:
//! [0..15] column-major model matrix (bitcast floats),
//! [16] material_index, [17] mesh_slot (informational in the vertex stage).
struct Payload {
  omnicpp::render::SceneMatrix model;
  std::uint32_t material_index;
  std::uint32_t mesh_slot;
};
static_assert(sizeof(Payload) == 72U, "18 words expected");

}  // namespace

TEST(VulkanHardware, GpuDrivenVertexPullMatchesPerDrawPixels) {
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppGpuDrivenAB", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(
      allocator.initialize(context.device(), context.physical_device()).is_ok());
  omnicpp::render::VulkanDescriptorManager descriptors;
  ASSERT_TRUE(descriptors.initialize(context.device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(context.queue_families().graphics_family);

  // ---- Shared geometry through the mesh table builder --------------------
  std::vector<float> cube_verts;
  std::vector<std::uint32_t> cube_idx;
  build_unit_cube(cube_verts, cube_idx);
  omnicpp::render::SceneMesh mesh_shell{};
  omnicpp::render::SceneMeshTableBuilder table_builder;
  const auto slot_cube = table_builder.add(mesh_shell, cube_verts, cube_idx);
  ASSERT_NE(slot_cube, omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);
  const auto& merged = table_builder.build();

  // Vertex buffer: STORAGE (both paths pull) — one shared copy serves both.
  auto vb = allocator.create_buffer(
      merged.vertex_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  // Index buffer: INDEX | STORAGE, single shared copy. Path A binds it as
  // an index buffer; path B binds the same one. Global indices match the
  // table builder's rewrite, and path A's per-mesh set exposes the same
  // vertex bytes, so both paths fetch identical vertices.
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

  // Payload + indirect draw-command buffers (driven path only).
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

  const SceneMatrix model_a = make_translation(-1.6f, 0.0f, -4.5f);
  const SceneMatrix model_b =
      mat_mul(make_translation(1.6f, 0.0f, -4.5f), make_rotation_y(0.6f));
  const Payload payloads[kObjectCount] = {
      {model_a, 0U, slot_cube},
      {model_b, 1U, slot_cube},
      {omnicpp::render::scene_identity_matrix(), 0U, slot_cube},
  };
  {
    auto* words = static_cast<std::uint32_t*>(payload_buf.value().mapped);
    words[0] = kObjectCount;
    words[1] = 0U;  // reserved: mesh-table word offset (step 3)
    std::memcpy(words + 2U, payloads, sizeof(payloads));
    auto* cmds =
        static_cast<VkDrawIndexedIndirectCommand*>(draw_buf.value().mapped);
    for (std::uint32_t i = 0; i < kObjectCount; ++i) {
      // {indexCount, instanceCount, firstIndex, vertexOffset, firstInstance}
      // vertexOffset stays 0 (global indices already land in the right
      // vertex block); firstInstance = i so gl_InstanceIndex addresses
      // payload[i].
      cmds[i] = {merged.entries[slot_cube].index_count, 1U,
                 merged.entries[slot_cube].index_offset, 0U, i};
    }
    cmds[2].indexCount = 0U;  // the cull convention: degenerate command
  }

  // ---- Materials (two slots, distinct colours) ----------------------------
  omnicpp::render::PbrMaterialData mats[2]{};
  mats[0].base_color_factor = {0.9f, 0.1f, 0.1f, 1.0f};
  mats[0].roughness_factor = 0.5f;
  mats[1].base_color_factor = {0.1f, 0.2f, 0.9f, 1.0f};
  mats[1].roughness_factor = 0.8f;
  auto mat_buf = allocator.create_buffer(
      2U * sizeof(omnicpp::render::PbrMaterialData),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(mat_buf.is_ok());
  std::memcpy(mat_buf.value().mapped, mats, sizeof(mats));

  // ---- Descriptor layouts -------------------------------------------------
  // Path A set 0: per-mesh vertex SSBO. Driven set 0: vertex SSBO (binding
  // 0) + payload SSBO (binding 1), VERTEX stage. Shared: set 1 bindless
  // samplers, set 2 material SSBO.
  const std::vector<omnicpp::render::ReflectedBinding> mesh_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT}};
  auto mesh_layout = descriptors.create_layout(mesh_bindings, 8U);
  ASSERT_TRUE(mesh_layout.is_ok());
  const std::vector<omnicpp::render::ReflectedBinding> driven_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT},
      {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT}};
  auto driven_layout = descriptors.create_layout(driven_bindings, 8U);
  ASSERT_TRUE(driven_layout.is_ok());
  const std::vector<omnicpp::render::ReflectedBinding> tex_bindings = {
      {1U, 0U, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto tex_layout = descriptors.create_layout(tex_bindings, 1U, true);
  ASSERT_TRUE(tex_layout.is_ok());
  const std::vector<omnicpp::render::ReflectedBinding> mat_bindings = {
      {2U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto mat_layout = descriptors.create_layout(mat_bindings, 8U);
  ASSERT_TRUE(mat_layout.is_ok());

  auto mesh_set = descriptors.allocate_set(mesh_layout.value());
  auto driven_set = descriptors.allocate_set(driven_layout.value());
  auto tex_set = descriptors.allocate_set(tex_layout.value());
  auto mat_set = descriptors.allocate_set(mat_layout.value());
  ASSERT_TRUE(mesh_set.is_ok() && driven_set.is_ok() && tex_set.is_ok() &&
              mat_set.is_ok());
  ASSERT_TRUE(descriptors
                  .write_buffer(mesh_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                vb.value().buffer, 0U, VK_WHOLE_SIZE)
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
                  .write_buffer(mat_set.value(), 0U,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                mat_buf.value().buffer, 0U, VK_WHOLE_SIZE)
                  .is_ok());

  SolidTexture white{};
  const std::uint8_t white_rgba[4] = {255U, 255U, 255U, 255U};
  ASSERT_TRUE(make_solid_texture(context.device(), context.physical_device(),
                                 context.graphics_queue(), qf, allocator,
                                 white_rgba, white));
  ASSERT_TRUE(descriptors
                  .write_image(tex_set.value(), 0U,
                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               white.sampler, white.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
                  .is_ok());

  // ---- Two identical offscreen targets ------------------------------------
  omnicpp::render::VulkanOffscreenTarget target_a, target_b;
  for (omnicpp::render::VulkanOffscreenTarget* t :
       {&target_a, &target_b}) {
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

  // ---- Pipelines ----------------------------------------------------------
  omnicpp::render::VulkanPipeline pipe_a, pipe_b;
  const std::string sd = OMNICPP_TEST_SHADER_DIR;
  ASSERT_TRUE(pipe_a
                  .load_shader_stage_file(
                      context.device(), sd + "/pbr_scene.vert.spv", "vertex")
                  .is_ok());
  ASSERT_TRUE(pipe_a
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_scene.frag.spv",
                                          "fragment")
                  .is_ok());
  const VkDescriptorSetLayout layouts_a[3] = {mesh_layout.value(),
                                              tex_layout.value(),
                                              mat_layout.value()};
  const VkPushConstantRange push_a{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};
  ASSERT_TRUE(pipe_a
                  .create_pipeline_layout(context.device(), layouts_a, 3U,
                                          &push_a)
                  .is_ok());
  ASSERT_TRUE(pipe_a
                  .create_graphics_pipeline(context.device(),
                                            target_a.render_pass(),
                                            target_a.format(),
                                            pipe_a.pipeline_layout(), true,
                                            true, false)
                  .is_ok());

  ASSERT_TRUE(pipe_b
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_gpu_driven.vert.spv",
                                          "vertex")
                  .is_ok());
  ASSERT_TRUE(pipe_b
                  .load_shader_stage_file(context.device(),
                                          sd + "/pbr_gpu_driven.frag.spv",
                                          "fragment")
                  .is_ok());
  const VkDescriptorSetLayout layouts_b[3] = {driven_layout.value(),
                                              tex_layout.value(),
                                              mat_layout.value()};
  const VkPushConstantRange push_b{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};  // same ABI shape as path A (model/material words unused)
  ASSERT_TRUE(pipe_b
                  .create_pipeline_layout(context.device(), layouts_b, 3U,
                                          &push_b)
                  .is_ok());
  ASSERT_TRUE(pipe_b
                  .create_graphics_pipeline(context.device(),
                                            target_b.render_pass(),
                                            target_b.format(),
                                            pipe_b.pipeline_layout(), true,
                                            true, false)
                  .is_ok());

  // ---- Path A scene snapshot (per-draw record) ----------------------------
  const SceneMatrix vp = make_perspective(45.0f, 1.0f, 0.1f, 100.0f);
  std::vector<omnicpp::render::SceneMesh> path_a_meshes(2U);
  omnicpp::render::VulkanPbrScene scene{};
  scene.pipeline = pipe_a.pipeline();
  scene.pipeline_layout = pipe_a.pipeline_layout();
  scene.camera.view_projection = vp;
  scene.camera_position = {0.0f, 0.0f, 0.0f, 1.0f};
  scene.texture_set = tex_set.value();
  scene.material_set = mat_set.value();
  for (std::uint32_t i = 0; i < 2U; ++i) {
    auto& m = path_a_meshes[i];
    m.vertex_buffer = vb.value().buffer;
    m.index_buffer = ib.value().buffer;
    m.index_offset = merged.entries[slot_cube].index_offset *
                     sizeof(std::uint32_t);
    m.index_count = merged.entries[slot_cube].index_count;
    auto s = descriptors.allocate_set(mesh_layout.value());
    ASSERT_TRUE(s.is_ok());
    ASSERT_TRUE(descriptors
                    .write_buffer(s.value(), 0U,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  vb.value().buffer, 0U, VK_WHOLE_SIZE)
                    .is_ok());
    m.descriptor_set = s.value();
    omnicpp::render::ScenePbrObject obj;
    obj.mesh = &m;
    obj.model = payloads[i].model;
    obj.material_index = payloads[i].material_index;
    scene.objects.push_back(obj);
  }

  // ---- Record + submit + readback for one target --------------------------
  const auto run = [&](omnicpp::render::VulkanOffscreenTarget& target,
                       bool driven) -> omnicpp_test::ReadbackResult {
    auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
        context.device(), qf);
    if (!pool.is_ok()) return {};
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
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
      omnicpp::render::VulkanRenderer renderer;
      const auto recorded =
          renderer.record_pbr_scene(cb, scene, kSize, kSize);
      if (!recorded.is_ok()) {
        EXPECT_TRUE(false) << "record_pbr_scene failed";
        return {};
      }
    } else {
      // ONE draw for the whole scene: instanced indirect, payload-driven.
      // Push keeps the 160-byte shape (model/material words ignored).
      struct PushB {
        omnicpp::render::SceneMatrix view_projection;
        omnicpp::render::SceneMatrix model_unused;
        std::array<float, 4> camera_position;
        std::uint32_t material_index_unused;
        std::array<std::uint32_t, 3> pad{};
      } push{vp, {}, {0.0f, 0.0f, 0.0f, 1.0f}, 0U, {}};
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipe_b.pipeline());
      const VkDescriptorSet sets[3] = {driven_set.value(), tex_set.value(),
                                       mat_set.value()};
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipe_b.pipeline_layout(), 0U, 3U, sets, 0U,
                              nullptr);
      vkCmdPushConstants(cb, pipe_b.pipeline_layout(),
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0U, sizeof(push), &push);
      vkCmdBindIndexBuffer(cb, ib.value().buffer, 0U, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexedIndirect(cb, draw_buf.value().buffer, 0U, kObjectCount,
                               sizeof(VkDrawIndexedIndirectCommand));
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
    return omnicpp_test::readback_swapchain_image(
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
      "\n[AB] A: non_clear=%zu red=%zu blue=%zu | B: non_clear=%zu red=%zu "
      "blue=%zu\n",
      result_a.non_clear_pixels, result_a.red_dominant_pixels,
      result_a.blue_dominant_pixels, result_b.non_clear_pixels,
      result_b.red_dominant_pixels, result_b.blue_dominant_pixels);
  std::fflush(stdout);
  EXPECT_GT(result_a.non_clear_pixels, 2000U)
      << "reference path should render substantial coverage";
  EXPECT_GT(result_b.non_clear_pixels, 2000U)
      << "driven path should render substantial coverage";
  // Colour structure: cube A red, cube B blue — on both paths.
  EXPECT_GT(result_a.red_dominant_pixels, 200U);
  EXPECT_GT(result_a.blue_dominant_pixels, 200U);
  EXPECT_GT(result_b.red_dominant_pixels, 200U)
      << "driven path: red cube missing (payload/material addressing?)";
  EXPECT_GT(result_b.blue_dominant_pixels, 200U)
      << "driven path: blue cube missing (payload/material addressing?)";

  // ---- Parity: byte-identical pixels --------------------------------------
  ASSERT_EQ(result_a.hash, result_b.hash)
      << "vertex-pull driven path must be pixel-identical to per-draw path";

  destroy_solid_texture(context.device(), allocator, white);
  pipe_b.cleanup(context.device());
  pipe_a.cleanup(context.device());
  descriptors.cleanup();
  allocator.cleanup();
  context.cleanup();
}

#endif  // OMNICPP_HAS_VULKAN
