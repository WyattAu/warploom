//! @file test_pbr_frame_variants.cpp
//! @brief ScenePbrObject::skinned -- one pass draws both static and rigged
//!        geometry through record_pbr_frame.
//!
//! The viewport has always needed this: a scene mixes a skinned mannequin
//! with static ground, so the draw list has to switch between a skinned and a
//! static vertex stage mid-pass. The renderer could only bind one pipeline,
//! which is why the application hand-rolled its own shadow pre-pass and lit
//! pass instead of using record_pbr_frame / record_pbr_scene.
//!
//! What this proves:
//!   (1) A scene holding one static and one rigged object renders both.
//!   (2) The rigged object is deformed by its bone matrices when marked
//!       skinned, and is NOT deformed when the same object is marked static.
//!       So the per-object pipeline switch genuinely takes effect.
//!   (3) The static object is unaffected by the flag.
//!   (4) The whole thing runs through record_pbr_frame, i.e. the render
//!       graph, not only through a hand-written command buffer.
//!
//! Geometry: the bar from test_gpu_skinning (two stacked quads; lower bound
//! to bone 0, upper to bone 1) plus a small static cube.

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/render/vulkan_scene.hpp"
// gtest first: vulkan_test_readback.hpp uses SUCCEED()/ADD_FAILURE() in its
// own body, so it must not be reached before the framework is declared.
#include <gtest/gtest.h>
#include "vulkan_test_readback.hpp"

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using omnicpp::render::PbrMaterialData;
using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
using omnicpp::render::ScenePbrObject;
using omnicpp::render::VulkanPbrScene;
using omnicpp::render::scene_identity_matrix;

constexpr std::uint32_t kRes = 256;

SceneMatrix make_perspective(float fov, float aspect, float zn, float zf) {
  SceneMatrix m = scene_identity_matrix();
  const float f = 1.0f / std::tan(fov * 3.14159265f / 360.0f);
  m[0] = f / aspect; m[5] = f;
  m[10] = (zf + zn) / (zn - zf); m[11] = -1.0f;
  m[14] = (2.0f * zf * zn) / (zn - zf); m[15] = 0.0f;
  return m;
}

SceneMatrix make_translation(float x, float y, float z) {
  SceneMatrix m = scene_identity_matrix();
  m[12] = x; m[13] = y; m[14] = z;
  return m;
}

SceneMatrix make_rotation_x(float radians) {
  SceneMatrix m = scene_identity_matrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  m[5] = c;  m[6] = s;
  m[9] = -s; m[10] = c;
  return m;
}

SceneMatrix make_multiply(const SceneMatrix& a, const SceneMatrix& b) {
  SceneMatrix out = scene_identity_matrix();
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[col * 4 + k];
      out[col * 4 + row] = sum;
    }
  }
  return out;
}

SceneMatrix make_scale(float s) {
  SceneMatrix m = scene_identity_matrix();
  m[0] = s; m[5] = s; m[10] = s;
  return m;
}

//! Two stacked quads in the XY plane. Lower vertices bind bone 0, upper
//! vertices bind bone 1, so rotating bone 1 bends the top half away.
void build_bar(std::vector<float>& verts, std::vector<std::uint32_t>& idx,
               std::vector<float>& skin) {
  const float lower[4][3] = {{-0.2f, -1.0f, 0.0f}, {0.2f, -1.0f, 0.0f},
                             {0.2f, 0.0f, 0.0f}, {-0.2f, 0.0f, 0.0f}};
  const float upper[4][3] = {{-0.2f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f},
                             {0.2f, 1.0f, 0.0f}, {-0.2f, 1.0f, 0.0f}};
  verts.clear();
  for (int i = 0; i < 4; ++i)
    verts.insert(verts.end(), {lower[i][0], lower[i][1], lower[i][2],
                               1, 1, 1, 0, 0, 1, 0, 0});
  for (int i = 0; i < 4; ++i)
    verts.insert(verts.end(), {upper[i][0], upper[i][1], upper[i][2],
                               1, 1, 1, 0, 0, 1, 0, 0});
  idx = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

  skin.clear();
  for (int i = 0; i < 4; ++i)
    skin.insert(skin.end(), {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f});
  for (int i = 0; i < 4; ++i)
    skin.insert(skin.end(), {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f});
}

void build_cube(std::vector<float>& verts, std::vector<std::uint32_t>& idx) {
  const float p[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                         {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
  verts.clear();
  for (int i = 0; i < 8; ++i)
    verts.insert(verts.end(), {p[i][0], p[i][1], p[i][2],
                               1, 1, 1, 0, 0, 1, 0, 0});
  const std::uint32_t t[36] = {0, 3, 1, 1, 3, 2, 4, 5, 7, 5, 6, 7,
                               0, 1, 4, 1, 5, 4, 3, 7, 2, 2, 7, 6,
                               0, 4, 3, 3, 4, 7, 1, 2, 5, 2, 6, 5};
  idx.assign(t, t + 36);
}

//! Vertex pull layout: positions then an optional skinning payload (8 floats
//! per vertex: joints x4 + weights x4) appended to the same SSBO.
bool build_mesh(omnicpp::render::VulkanMemoryAllocator& alloc,
                omnicpp::render::VulkanDescriptorManager& desc,
                VkDescriptorSetLayout mesh_layout,
                const std::vector<float>& verts,
                const std::vector<std::uint32_t>& idx,
                const std::vector<float>& skin, SceneMesh& out_mesh,
                omnicpp::render::Allocation& out_va,
                omnicpp::render::Allocation& out_ia) {
  std::vector<float> combined = verts;
  if (!skin.empty())
    combined.insert(combined.end(), skin.begin(), skin.end());

  auto vb = alloc.create_buffer(
      combined.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto ib = alloc.create_buffer(
      idx.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!vb.is_ok() || !ib.is_ok()) return false;
  out_va = vb.value();
  out_ia = ib.value();
  std::memcpy(out_va.mapped, combined.data(), combined.size() * sizeof(float));
  std::memcpy(out_ia.mapped, idx.data(), idx.size() * sizeof(std::uint32_t));

  auto ds = desc.allocate_set(mesh_layout);
  if (!ds.is_ok()) return false;
  desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    out_va.buffer, 0, VK_WHOLE_SIZE);

  out_mesh.vertex_buffer = out_va.buffer;
  out_mesh.index_buffer = out_ia.buffer;
  out_mesh.index_count = static_cast<std::uint32_t>(idx.size());
  out_mesh.index_offset = 0;
  out_mesh.descriptor_set = ds.value();
  return true;
}

struct SolidTexture {
  VkImage img{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  omnicpp::render::Allocation alloc{};
};

bool make_white(VkDevice d, VkPhysicalDevice pd, VkQueue q, std::uint32_t fam,
                omnicpp::render::VulkanMemoryAllocator& a, SolidTexture& out) {
  out = {};
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {1, 1, 1};
  ii.mipLevels = ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(d, &ii, nullptr, &out.img) != VK_SUCCESS) return false;
  auto mem = a.bind_image(out.img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!mem.is_ok()) { vkDestroyImage(d, out.img, nullptr); return false; }
  out.alloc = mem.value();

  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(d, &vi, nullptr, &out.view) != VK_SUCCESS) {
    a.destroy_allocation(out.alloc); vkDestroyImage(d, out.img, nullptr);
    return false;
  }
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(d, &si, nullptr, &out.sampler) != VK_SUCCESS) {
    vkDestroyImageView(d, out.view, nullptr);
    a.destroy_allocation(out.alloc); vkDestroyImage(d, out.img, nullptr);
    return false;
  }

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(d, fam);
  if (!pool.is_ok()) return false;
  auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(d, pool.value());
  if (!cbr.is_ok()) { vkDestroyCommandPool(d, pool.value(), nullptr); return false; }
  VkCommandBuffer cb = cbr.value();
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(d, &fi, nullptr, &fence);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  bool ok = vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.image = out.img;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.subresourceRange = range;
  if (ok) {
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkClearColorValue cc{{1.0f, 1.0f, 1.0f, 1.0f}};
    VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, out.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &cc, 1, &cr);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
  }
  if (ok) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ok = vkQueueSubmit(q, 1, &si, fence) == VK_SUCCESS;
  }
  if (ok)
    ok = vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
  vkDestroyFence(d, fence, nullptr);
  vkDestroyCommandPool(d, pool.value(), nullptr);
  if (!ok) return false;
  return true;
}

void destroy_white(VkDevice d, omnicpp::render::VulkanMemoryAllocator& a,
                   SolidTexture& t) {
  if (t.sampler) vkDestroySampler(d, t.sampler, nullptr);
  if (t.view) vkDestroyImageView(d, t.view, nullptr);
  if (t.alloc.is_valid()) a.destroy_allocation(t.alloc);
  if (t.img) vkDestroyImage(d, t.img, nullptr);
  t = {};
}

//! Two pipelines sharing one 4-set layout (mesh, textures, material, bones):
//! a static one built from pbr_scene.vert and a skinned one from
//! skinned_scene.vert. Sharing the layout is what lets the draw loop swap
//! pipeline without re-binding descriptor sets -- and it is the shape the
//! viewport should adopt.
struct VariantHarness {
  omnicpp::render::VulkanContext ctx;
  omnicpp::render::VulkanMemoryAllocator alloc;
  omnicpp::render::VulkanDescriptorManager desc;
  VkDescriptorSetLayout mesh_layout{};
  VkDescriptorSetLayout tex_layout{};
  VkDescriptorSetLayout mat_layout{};
  VkDescriptorSetLayout bone_layout{};
  VkDescriptorSet tex_set{};
  VkDescriptorSet mat_set{};
  VkDescriptorSet bone_set{};
  SolidTexture white;
  SceneMesh bar_mesh{};
  SceneMesh cube_mesh{};
  omnicpp::render::Allocation bar_va{}, bar_ia{}, cube_va{}, cube_ia{};
  omnicpp::render::Allocation mat_buf{}, bone_buf{};
  omnicpp::render::VulkanOffscreenTarget target;
  omnicpp::render::VulkanPipeline static_pipe;
  omnicpp::render::VulkanPipeline skinned_pipe;
  std::uint32_t qf{0};

  bool init(const char* name) {
    if (!ctx.initialize(name, true).is_ok()) return false;
    if (!ctx.has_descriptor_indexing()) { ctx.cleanup(); return false; }
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) { ctx.cleanup(); return false; }
    if (!desc.initialize(ctx.device()).is_ok()) return false;
    qf = static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
    const VkDevice dev = ctx.device();

    auto r0 = desc.create_layout({{0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_VERTEX_BIT}}, 8);
    if (!r0.is_ok()) return false;
    mesh_layout = r0.value();
    auto r1 = desc.create_layout({{1,0,0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 1, true);
    if (!r1.is_ok()) return false;
    tex_layout = r1.value();
    auto ts = desc.allocate_set(tex_layout);
    if (!ts.is_ok()) return false;
    tex_set = ts.value();
    auto r2 = desc.create_layout({{2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 8);
    if (!r2.is_ok()) return false;
    mat_layout = r2.value();
    auto ms = desc.allocate_set(mat_layout);
    if (!ms.is_ok()) return false;
    mat_set = ms.value();
    auto mb = alloc.create_buffer(128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mb.is_ok()) return false;
    mat_buf = mb.value();
    desc.write_buffer(mat_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mat_buf.buffer, 0, VK_WHOLE_SIZE);

    auto r3 = desc.create_layout({{3,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_VERTEX_BIT}}, 8);
    if (!r3.is_ok()) return false;
    bone_layout = r3.value();
    auto bs = desc.allocate_set(bone_layout);
    if (!bs.is_ok()) return false;
    bone_set = bs.value();
    auto bb = alloc.create_buffer(2*64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!bb.is_ok()) return false;
    bone_buf = bb.value();
    desc.write_buffer(bone_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bone_buf.buffer, 0, VK_WHOLE_SIZE);

    if (!make_white(dev, ctx.physical_device(), ctx.graphics_queue(), qf, alloc, white)) return false;
    desc.write_image(tex_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     white.sampler, white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0);

    std::vector<float> v, skin; std::vector<std::uint32_t> i;
    build_bar(v, i, skin);
    if (!build_mesh(alloc, desc, mesh_layout, v, i, skin, bar_mesh, bar_va, bar_ia)) return false;
    build_cube(v, i);
    if (!build_mesh(alloc, desc, mesh_layout, v, i, {}, cube_mesh, cube_va, cube_ia)) return false;

    if (!target.create(dev, ctx.physical_device(), VK_FORMAT_B8G8R8A8_UNORM, kRes, kRes, &alloc).is_ok() ||
        !target.create_depth(dev, ctx.physical_device(), VK_FORMAT_D32_SFLOAT).is_ok() ||
        !target.create_render_pass(dev).is_ok() ||
        !target.create_framebuffer(dev).is_ok()) return false;

    const std::string sd = WARPLOOM_TEST_SHADER_DIR;
    VkDescriptorSetLayout layouts[4] = {mesh_layout, tex_layout, mat_layout, bone_layout};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};

    if (!static_pipe.load_shader_stage_file(dev, sd+"/pbr_scene.vert.spv", "vertex").is_ok() ||
        !static_pipe.load_shader_stage_file(dev, sd+"/pbr_scene.frag.spv", "fragment").is_ok()) return false;
    if (!static_pipe.create_pipeline_layout(dev, layouts, 4, &pr).is_ok() ||
        !static_pipe.create_graphics_pipeline(dev, target.render_pass(), target.format(),
            static_pipe.pipeline_layout(), true, true, false).is_ok()) return false;

    if (!skinned_pipe.load_shader_stage_file(dev, sd+"/skinned_scene.vert.spv", "vertex").is_ok() ||
        !skinned_pipe.load_shader_stage_file(dev, sd+"/pbr_scene.frag.spv", "fragment").is_ok()) return false;
    if (!skinned_pipe.create_pipeline_layout(dev, layouts, 4, &pr).is_ok() ||
        !skinned_pipe.create_graphics_pipeline(dev, target.render_pass(), target.format(),
            skinned_pipe.pipeline_layout(), true, true, false).is_ok()) return false;
    return true;
  }

  void write_bones(const SceneMatrix* bones, std::uint32_t count) {
    std::memcpy(bone_buf.mapped, bones, count * 64);
  }
  void write_material(const PbrMaterialData& m, std::uint32_t slot) {
    std::memcpy(static_cast<std::uint8_t*>(mat_buf.mapped) + slot * sizeof(PbrMaterialData),
                &m, sizeof(m));
  }

  VulkanPbrScene make_scene() {
    VulkanPbrScene s{};
    s.pipeline = static_pipe.pipeline();
    s.pipeline_layout = static_pipe.pipeline_layout();
    s.skinned_pipeline = skinned_pipe.pipeline();
    s.skinned_pipeline_layout = skinned_pipe.pipeline_layout();
    s.camera.view_projection = make_perspective(45.0f, 1.0f, 0.1f, 100.0f);
    s.camera_position = {0.0f, 0.0f, 4.0f, 1.0f};
    s.texture_set = tex_set;
    s.material_set = mat_set;
    s.bone_set = bone_set;
    return s;
  }

  //! Render through record_pbr_frame -- the graph path the application will
  //! use, not a hand-written command buffer.
  omnicpp_test::ReadbackResult render_frame(const VulkanPbrScene& scene) {
    const VkDevice dev = ctx.device();
    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(dev, qf);
    if (!pr.is_ok()) return {};
    auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(dev, pr.value());
    if (!cr.is_ok()) { vkDestroyCommandPool(dev, pr.value(), nullptr); return {}; }
    VkCommandBuffer cb = cr.value();
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev, &fi, nullptr, &fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
      vkDestroyFence(dev, fence, nullptr);
      vkDestroyCommandPool(dev, pr.value(), nullptr);
      return {};
    }

    omnicpp::render::VulkanRenderer renderer;
    omnicpp::render::VulkanRenderer::PbrFrameTargets targets{};
    targets.render_pass = target.render_pass();
    targets.framebuffer = target.framebuffer();
    targets.width = kRes;
    targets.height = kRes;
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0U};
    targets.clear_values = clears;
    targets.clear_value_count = 2;

    omnicpp_test::ReadbackResult out{};
    if (renderer.record_pbr_frame(cb, scene, targets).is_ok() &&
        vkEndCommandBuffer(cb) == VK_SUCCESS) {
      vkResetFences(dev, 1, &fence);
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
      vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
      out = omnicpp_test::readback_swapchain_image(
          ctx.physical_device(), dev, ctx.graphics_queue(), qf, target.image(),
          target.format(), kRes, kRes, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*store_pixels=*/true);
    } else {
      vkEndCommandBuffer(cb);
    }
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pr.value(), nullptr);
    return out;
  }

  void cleanup() {
    const VkDevice dev = ctx.device();
    skinned_pipe.cleanup(dev);
    static_pipe.cleanup(dev);
    target.cleanup(dev);
    destroy_white(dev, alloc, white);
    if (bar_va.is_valid()) alloc.destroy_allocation(bar_va);
    if (bar_ia.is_valid()) alloc.destroy_allocation(bar_ia);
    if (cube_va.is_valid()) alloc.destroy_allocation(cube_va);
    if (cube_ia.is_valid()) alloc.destroy_allocation(cube_ia);
    if (mat_buf.is_valid()) alloc.destroy_allocation(mat_buf);
    if (bone_buf.is_valid()) alloc.destroy_allocation(bone_buf);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

}  // namespace

//! One record_pbr_frame call must draw a rigged object through the skinned
//! vertex stage and a static object through the static one, in the same pass.
TEST(VulkanHardware, PbrFrameDrawsMixedStaticAndSkinned) {
  VariantHarness h;
  if (!h.init("mixed_variants_test"))
    GTEST_SKIP() << "Vulkan unavailable or descriptor indexing missing";

  PbrMaterialData red{};
  red.base_color_factor = {0.9f, 0.1f, 0.1f, 1.0f};
  red.metallic_factor = 0.0f;
  red.roughness_factor = 0.8f;
  h.write_material(red, 0);
  PbrMaterialData blue{};
  blue.base_color_factor = {0.1f, 0.2f, 0.9f, 1.0f};
  blue.metallic_factor = 0.0f;
  blue.roughness_factor = 0.8f;
  h.write_material(blue, 1);

  // Bone 1 rotated 90 degrees about X: if the bar draws through the skinned
  // vertex stage its top half bends away from the camera.
  SceneMatrix bent[2] = {scene_identity_matrix(), make_rotation_x(1.5707963f)};
  h.write_bones(bent, 2);

  // Placed so the two objects cannot overlap the image's vertical midline.
  // At 45 deg FOV and z = -4 the half-width is 4*tan(22.5) ~= 1.66 world
  // units, so a 128 px half-width maps to 1.66 units. The bar (0.4 wide)
  // sits at x=-0.6 -> pixels ~[67, 98]; the cube (0.7 wide) at x=1.0 ->
  // ~[178, 232]. Both stay in their own half, which is what makes the
  // left/right comparison below meaningful.
  ScenePbrObject bar{};
  bar.mesh = &h.bar_mesh;
  bar.model = make_translation(-0.6f, 0.0f, -4.0f);
  bar.material_index = 0;

  ScenePbrObject cube{};
  cube.mesh = &h.cube_mesh;
  cube.model = make_multiply(make_translation(1.0f, -0.5f, -4.0f),
                            make_scale(0.35f));
  cube.material_index = 1;

  // --- Pass 1: the bar marked skinned -----------------------------------
  VulkanPbrScene skinned_scene = h.make_scene();
  ScenePbrObject skinned_bar = bar;
  skinned_bar.skinned = true;
  skinned_scene.objects = {skinned_bar, cube};
  const auto skinned_result = h.render_frame(skinned_scene);

  // --- Pass 2: the same bar marked static --------------------------------
  VulkanPbrScene static_scene = h.make_scene();
  static_scene.objects = {bar, cube};  // skinned stays false
  const auto static_result = h.render_frame(static_scene);

  // --- Pass 3: skinned bar only, for a footprint comparison -------------
  VulkanPbrScene bar_only_scene = h.make_scene();
  bar_only_scene.objects = {skinned_bar};
  const auto bar_only = h.render_frame(bar_only_scene);

  // --- Pass 4: static cube only ------------------------------------------
  VulkanPbrScene cube_only_scene = h.make_scene();
  cube_only_scene.objects = {cube};
  const auto cube_only = h.render_frame(cube_only_scene);

  h.cleanup();

  ASSERT_TRUE(skinned_result.submitted) << "mixed skinned frame did not submit";
  ASSERT_TRUE(static_result.submitted) << "mixed static frame did not submit";
  ASSERT_TRUE(bar_only.submitted);
  ASSERT_TRUE(cube_only.submitted);

  // (1) Both objects contribute: the mixed pass covers strictly more pixels
  // than either object alone.
  EXPECT_GT(skinned_result.non_clear_pixels, bar_only.non_clear_pixels)
      << "static cube missing from the mixed frame";
  EXPECT_GT(skinned_result.non_clear_pixels, cube_only.non_clear_pixels)
      << "skinned bar missing from the mixed frame";

  // (2) The switch took effect: with the same bone matrices, the bar bends
  // only when it is marked skinned. This is the load-bearing assertion -- if
  // the flag were ignored, or bound to the wrong pipeline, these would match.
  EXPECT_NE(skinned_result.hash, static_result.hash)
      << "ScenePbrObject::skinned had no effect: the bar rendered identically "
         "through the skinned and the static pipeline";

  // (3) The switch is scoped to the flagged object. The cube is placed well
  // to the right of the bar, so the right-hand half of the two mixed passes
  // must be byte-identical: the bar's pipeline choice cannot disturb it.
  ASSERT_EQ(skinned_result.pixels.size(), kRes * kRes);
  ASSERT_EQ(static_result.pixels.size(), kRes * kRes);
  std::size_t right_diffs = 0;
  for (std::uint32_t y = 0; y < kRes; ++y) {
    for (std::uint32_t x = kRes / 2U; x < kRes; ++x) {
      if (skinned_result.pixels[y * kRes + x] != static_result.pixels[y * kRes + x])
        ++right_diffs;
    }
  }
  EXPECT_EQ(right_diffs, 0U)
      << "the right half of the frame differs between the skinned and static "
         "passes (" << right_diffs
      << " px); the per-object pipeline switch is leaking across objects";

  // And the difference really is on the left, where the bar is.
  std::size_t left_diffs = 0;
  for (std::uint32_t y = 0; y < kRes; ++y) {
    for (std::uint32_t x = 0; x < kRes / 2U; ++x) {
      if (skinned_result.pixels[y * kRes + x] != static_result.pixels[y * kRes + x])
        ++left_diffs;
    }
  }
  EXPECT_GT(left_diffs, 0U)
      << "the skinned and static frames are identical on the bar's half; the "
         "skinned vertex stage was not used";

  // (4) Both passes drew something real.
  EXPECT_GT(bar_only.non_clear_pixels, 500U) << "skinned bar did not render";
  EXPECT_GT(cube_only.non_clear_pixels, 200U) << "static cube did not render";
}

#endif  // WARPLOOM_HAS_VULKAN