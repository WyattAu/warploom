//! @file test_rt_shadows.cpp
//! @brief E1 GPU proof: ray-query hard shadows against the raster PCF path.
//!
//! One scene rendered twice through the SAME record_pbr_frame graph,
//! differing only in the shadow mechanism:
//!   Path A: pbr_shadow.frag — 1024^2 depth map + PCF sampler (set 3).
//!   Path B: pbr_rt_shadow.frag — ray query against a scene TLAS (set 3).
//! Scene: a near occluder cube (unit, at z=-2) and a far cube (0.6^3, at
//! (-1.205, -2.115, -5)). Under the shader's light direction
//! kLightDir = normalize(0.3, 0.65, 0.7) a CPU analytic segment/AABB check
//! proves the far cube's camera-facing face center is occluded by the near
//! cube, and the whole far cube lies inside the occluder's shadow column.
//! The light view-projection is a true look-at along kLightDir, so BOTH
//! paths must agree:
//!   - near cube lit (direct PBR lighting, PCF factor 1, ray unoccluded),
//!   - far cube hard-shadowed (ambient only).
//! Proofs:
//!   1. A compute tracer returns hit world positions for camera rays: both
//!      cubes are present in the TLAS with the right custom indices, and
//!      far-cube hits are CPU-verified occluded along the light ray.
//!   2. Both paths render the scene; near cube is lit on both with matching
//!      luminance (identical direct lighting).
//!   3. Far cube is shadowed on BOTH paths (parity between map and rays).
//!   4. Zero validation errors (VUIDs) — the run executes under
//!      VK_LAYER_KHRONOS_validation in the validation preset.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "warploom/render/vulkan_acceleration_structure.hpp"
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_frame_upload.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/render/vulkan_scene.hpp"
#include "vulkan_test_readback.hpp"

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>

namespace {

using omnicpp::render::Allocation;
using omnicpp::render::BlasBuildInput;
using omnicpp::render::PbrMaterialData;
using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
using omnicpp::render::ScenePbrObject;
using omnicpp::render::TlasInstance;
using omnicpp::render::VulkanAccelerationStructureBuilder;
using omnicpp::render::VulkanContext;
using omnicpp::render::VulkanDescriptorManager;
using omnicpp::render::VulkanMemoryAllocator;
using omnicpp::render::VulkanOffscreenTarget;
using omnicpp::render::VulkanPbrScene;
using omnicpp::render::VulkanPipeline;
using omnicpp::render::VulkanScratchPool;
using omnicpp::render::scene_identity_matrix;

constexpr std::uint32_t kShadowRes = 1024;
constexpr std::uint32_t kImg = 256;       // offscreen render resolution
constexpr std::uint32_t kTracerDim = 64;  // tracer grid: 64x64 rays

SceneMatrix make_perspective(float fov_y_deg, float aspect, float zn, float zf) {
  SceneMatrix m = scene_identity_matrix();
  const float f = 1.0f / std::tan(fov_y_deg * 3.14159265f / 360.0f);
  // Vulkan y-flip in p[1][1]: engine assets are authored CCW in y-up space so
  // "the projection's y-flip lands them CCW in framebuffer space" (see the
  // mannequin generator and gpu-driven shaders). Without the flip,
  // VK_FRONT_FACE_COUNTER_CLOCKWISE + back culling shades cube INTERIORS
  // (normals face away from the light -> ambient-only shading on every path).
  m[0] = f / aspect; m[5] = -f;
  m[10] = (zf + zn) / (zn - zf); m[11] = -1.0f;
  m[14] = (2.0f * zf * zn) / (zn - zf); m[15] = 0.0f;
  return m;
}

SceneMatrix make_ortho(float l, float r, float b, float t, float zn, float zf) {
  SceneMatrix m = scene_identity_matrix();
  // Vulkan y-flip, same convention as make_perspective: without it the light
  // pre-pass culls front faces and the map stores BACK-face depths, so every
  // front-face fragment fails the LESS_OR_EQUAL compare (everything shadowed).
  m[0] = 2.0f / (r - l); m[5] = -2.0f / (t - b);
  m[10] = 1.0f / (zn - zf);
  m[12] = -(r + l) / (r - l); m[13] = -(t + b) / (t - b);
  m[14] = zn / (zn - zf);
  return m;
}

SceneMatrix make_translation(float x, float y, float z) {
  SceneMatrix m = scene_identity_matrix();
  m[12] = x; m[13] = y; m[14] = z;
  return m;
}

SceneMatrix mat4_multiply(const SceneMatrix& a, const SceneMatrix& b) {
  SceneMatrix out{};
  for (std::size_t col = 0; col < static_cast<std::size_t>(4); ++col) {
    for (std::size_t row = 0; row < static_cast<std::size_t>(4); ++row) {
      float acc = 0.0f;
      for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) {
        acc += a[k * 4 + row] * b[col * 4 + k];
      }
      out[col * 4 + row] = acc;
    }
  }
  return out;
}

std::array<float, 3> normalize3(const std::array<float, 3>& v) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return {v[0] / len, v[1] / len, v[2] / len};
}

std::array<float, 3> cross3(const std::array<float, 3>& a,
                            const std::array<float, 3>& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

float dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

//! Look-at view matrix (column-major, GL convention: camera looks down -Z of
//! its own space; points in front get negative view z).
SceneMatrix make_look_at(const std::array<float, 3>& eye,
                         const std::array<float, 3>& target,
                         const std::array<float, 3>& up_world) {
  const std::array<float, 3> fwd = normalize3({target[0] - eye[0],
                                               target[1] - eye[1],
                                               target[2] - eye[2]});
  const std::array<float, 3> right = normalize3(cross3(fwd, up_world));
  const std::array<float, 3> up = cross3(right, fwd);
  SceneMatrix m = scene_identity_matrix();
  m[0] = right[0]; m[1] = up[0]; m[2] = -fwd[0];
  m[4] = right[1]; m[5] = up[1]; m[6] = -fwd[1];
  m[8] = right[2]; m[9] = up[2]; m[10] = -fwd[2];
  m[12] = -dot3(right, eye);
  m[13] = -dot3(up, eye);
  m[14] = dot3(fwd, eye);
  return m;
}

//! 12-triangle non-indexed box (36 vertices, 3 floats each, half-extents h).
std::vector<float> make_box_triangles(float hx, float hy, float hz) {
  static const std::uint32_t c[36] = {
      0, 4, 5, 0, 5, 1,  4, 6, 7, 4, 7, 5,
      6, 2, 3, 6, 3, 7,  2, 0, 1, 2, 1, 3,
      1, 5, 7, 1, 7, 3,  2, 6, 4, 2, 4, 0};
  const float h[3] = {hx, hy, hz};
  std::vector<float> out;
  out.reserve(36U * 3U);
  for (const std::uint32_t vi : c) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(3); ++k) {
      out.push_back(((vi >> k) & 1U) != 0U ? h[k] : -h[k]);
    }
  }
  return out;
}

std::array<float, 12> translated_instance(float tx, float ty, float tz) {
  return {1.0f, 0.0f, 0.0f, tx, 0.0f, 1.0f, 0.0f, ty,
          0.0f, 0.0f, 1.0f, tz};
}

struct BufferPair {
  Allocation va{};
  Allocation ia{};
  SceneMesh mesh{};
};

//! 24-vertex indexed cube mesh (pbr_scene.vert layout: position.xyz,
//! color.rgb, normal.xyz, uv.xy). Half-extent 0.5.
BufferPair make_cube_mesh(VulkanMemoryAllocator& alloc,
                          VulkanDescriptorManager& desc,
                          VkDescriptorSetLayout mesh_layout) {
  const float faces[6][15] = {
      {0, 0, 1,  -1, -1, 1,  1, -1, 1,  1, 1, 1,  -1, 1, 1},
      {0, 0, -1, 1, -1, -1, -1, -1, -1, -1, 1, -1, 1, 1, -1},
      {1, 0, 0,  1, -1, 1,  1, -1, -1, 1, 1, -1, 1, 1, 1},
      {-1, 0, 0, -1, -1, -1, -1, -1, 1, -1, 1, 1, -1, 1, -1},
      {0, 1, 0,  -1, 1, 1,  1, 1, 1,  1, 1, -1, -1, 1, -1},
      {0, -1, 0, -1, -1, -1, 1, -1, -1, 1, -1, 1, -1, -1, 1}};
  std::vector<float> v;
  std::vector<std::uint32_t> i;
  for (std::size_t f = 0; f < static_cast<std::size_t>(6); ++f) {
    const std::uint32_t base = static_cast<std::uint32_t>(v.size() / 11U);
    for (std::size_t vert = 0; vert < static_cast<std::size_t>(4); ++vert) {
      v.push_back(faces[f][3 + vert * 3 + 0] * 0.5f);
      v.push_back(faces[f][3 + vert * 3 + 1] * 0.5f);
      v.push_back(faces[f][3 + vert * 3 + 2] * 0.5f);
      v.push_back(1.0f); v.push_back(1.0f); v.push_back(1.0f);
      v.push_back(faces[f][0]);
      v.push_back(faces[f][1]);
      v.push_back(faces[f][2]);
      v.push_back(0.0f); v.push_back(0.0f);
    }
    i.insert(i.end(), {base, base + 1U, base + 2U, base, base + 2U, base + 3U});
  }
  BufferPair out{};
  auto vb = alloc.create_buffer(v.size() * sizeof(float),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto ib = alloc.create_buffer(i.size() * sizeof(std::uint32_t),
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!vb.is_ok() || !ib.is_ok()) return out;
  out.va = vb.value();
  out.ia = ib.value();
  std::memcpy(out.va.mapped, v.data(), v.size() * sizeof(float));
  std::memcpy(out.ia.mapped, i.data(), i.size() * sizeof(std::uint32_t));
  auto ds = desc.allocate_set(mesh_layout);
  if (!ds.is_ok()) { BufferPair empty; return empty; }
  if (!desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         out.va.buffer, 0, VK_WHOLE_SIZE).is_ok()) {
    BufferPair empty;
    return empty;
  }
  out.mesh.vertex_buffer = out.va.buffer;
  out.mesh.index_buffer = out.ia.buffer;
  out.mesh.index_count = static_cast<std::uint32_t>(i.size());
  out.mesh.descriptor_set = ds.value();
  return out;
}

struct SolidTexture {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  Allocation allocation{};
};

bool make_solid_texture(VkDevice dev, VkPhysicalDevice pd, VkQueue q,
    std::uint32_t family, VulkanMemoryAllocator& alloc,
    const std::array<std::uint8_t, 4>& rgba, SolidTexture& out) {
  out = {};
  SolidTexture t{};
  auto fail = [&]() {
    if (t.allocation.is_valid()) alloc.destroy_allocation(t.allocation);
    if (t.view) vkDestroyImageView(dev, t.view, nullptr);
    if (t.sampler) vkDestroySampler(dev, t.sampler, nullptr);
    if (t.image) vkDestroyImage(dev, t.image, nullptr);
    return false;
  };
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {1, 1, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (vkCreateImage(dev, &ii, nullptr, &t.image) != VK_SUCCESS) return fail();
  auto mem = alloc.bind_image(t.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!mem.is_ok()) return fail();
  t.allocation = mem.value();
  omnicpp::render::VulkanFrameUploadArena arena;
  if (!arena.initialize(dev, pd, family, 1U, 1U << 20).is_ok()) return fail();
  if (!arena.begin_frame(0).is_ok()) return fail();
  auto span = arena.acquire(4);
  if (!span.is_ok()) return fail();
  std::memcpy(span.value().host_data, rgba.data(), 4);
  arena.record_copy_image_rgba8(span.value(), t.image, 1, 1);
  if (!arena.submit(q).is_ok()) return fail();
  arena.wait_idle();
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = t.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(dev, &vi, nullptr, &t.view) != VK_SUCCESS) return fail();
  VkSamplerCreateInfo sp{};
  sp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sp.magFilter = sp.minFilter = VK_FILTER_NEAREST;
  sp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sp.addressModeU = sp.addressModeV = sp.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(dev, &sp, nullptr, &t.sampler) != VK_SUCCESS) {
    return fail();
  }
  out = t;
  return true;
}

void destroy_solid(VkDevice d, VulkanMemoryAllocator& a, SolidTexture& t) {
  if (t.sampler) vkDestroySampler(d, t.sampler, nullptr);
  if (t.view) vkDestroyImageView(d, t.view, nullptr);
  if (t.allocation.is_valid()) a.destroy_allocation(t.allocation);
  if (t.image) vkDestroyImage(d, t.image, nullptr);
  t = {};
}

struct RtShadowHarness {
  VulkanContext ctx;
  VulkanMemoryAllocator alloc;
  VulkanDescriptorManager desc;
  VkDescriptorSetLayout mesh_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout tex_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout mat_layout{VK_NULL_HANDLE};
  VkDescriptorSet tex_set{VK_NULL_HANDLE};
  VkDescriptorSet mat_set{VK_NULL_HANDLE};
  SolidTexture white;
  BufferPair cube;
  VulkanOffscreenTarget target;
  VulkanPipeline shadow_pipe;
  Allocation mat_buf_alloc{};
  Allocation shadow_ubo_alloc{};
  VkDescriptorSet shadow_set{VK_NULL_HANDLE};
  VkDescriptorSetLayout shadow_layout{VK_NULL_HANDLE};
  VkImage shadow_img{VK_NULL_HANDLE};
  Allocation shadow_mem{};
  VkImageView shadow_depth_view{VK_NULL_HANDLE};
  VkImageView shadow_sample_view{VK_NULL_HANDLE};
  VkSampler shadow_sampler{VK_NULL_HANDLE};
  VkRenderPass shadow_rp{VK_NULL_HANDLE};
  VkFramebuffer shadow_fb{VK_NULL_HANDLE};
  std::uint32_t qf{0};

  bool init(const char* name) {
    if (!ctx.initialize(name, true).is_ok()) return false;
    if (!ctx.has_ray_tracing()) { ctx.cleanup(); return false; }
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) {
      ctx.cleanup();
      return false;
    }
    if (!desc.initialize(ctx.device()).is_ok()) return false;
    qf = static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

    auto r0 = desc.create_layout({{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   VK_SHADER_STAGE_VERTEX_BIT}}, 8);
    if (!r0.is_ok()) return false;
    mesh_layout = r0.value();
    auto r1 = desc.create_layout({{1, 0, 0,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   VK_SHADER_STAGE_FRAGMENT_BIT}}, 1, true);
    if (!r1.is_ok()) return false;
    tex_layout = r1.value();
    auto ts = desc.allocate_set(tex_layout);
    if (!ts.is_ok()) return false;
    tex_set = ts.value();
    auto r2 = desc.create_layout({{2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   VK_SHADER_STAGE_FRAGMENT_BIT}}, 8);
    if (!r2.is_ok()) return false;
    mat_layout = r2.value();
    auto ms = desc.allocate_set(mat_layout);
    if (!ms.is_ok()) return false;
    mat_set = ms.value();
    auto mb = alloc.create_buffer(2 * sizeof(PbrMaterialData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mb.is_ok()) return false;
    mat_buf_alloc = mb.value();
    if (!desc.write_buffer(mat_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           mat_buf_alloc.buffer, 0, VK_WHOLE_SIZE).is_ok()) {
      return false;
    }

    if (!make_solid_texture(ctx.device(), ctx.physical_device(),
                            ctx.graphics_queue(), qf, alloc,
                            {255, 255, 255, 255}, white)) {
      return false;
    }
    if (!desc.write_image(tex_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          white.sampler, white.view,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok()) {
      return false;
    }

    cube = make_cube_mesh(alloc, desc, mesh_layout);
    if (cube.mesh.index_count == 0U) return false;

    if (!target.create(ctx.device(), ctx.physical_device(),
                       VK_FORMAT_B8G8R8A8_UNORM, kImg, kImg, &alloc).is_ok() ||
        !target.create_depth(ctx.device(), ctx.physical_device(),
                             VK_FORMAT_D32_SFLOAT).is_ok() ||
        !target.create_render_pass(ctx.device()).is_ok() ||
        !target.create_framebuffer(ctx.device()).is_ok()) {
      return false;
    }

    // Shadow depth image + sampler + pass + set (path A only).
    VkImageCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    si.imageType = VK_IMAGE_TYPE_2D;
    si.format = VK_FORMAT_D32_SFLOAT;
    si.extent = {kShadowRes, kShadowRes, 1};
    si.mipLevels = 1; si.arrayLayers = 1;
    si.samples = VK_SAMPLE_COUNT_1_BIT; si.tiling = VK_IMAGE_TILING_OPTIMAL;
    si.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(ctx.device(), &si, nullptr, &shadow_img) != VK_SUCCESS) {
      return false;
    }
    auto sm = alloc.bind_image(shadow_img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!sm.is_ok()) return false;
    shadow_mem = sm.value();
    VkImageViewCreateInfo dvi{};
    dvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dvi.image = shadow_img; dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvi.format = VK_FORMAT_D32_SFLOAT;
    dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(ctx.device(), &dvi, nullptr, &shadow_depth_view) !=
        VK_SUCCESS) {
      return false;
    }
    if (vkCreateImageView(ctx.device(), &dvi, nullptr, &shadow_sample_view) !=
        VK_SUCCESS) {
      return false;
    }
    VkSamplerCreateInfo sp{};
    sp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sp.magFilter = sp.minFilter = VK_FILTER_LINEAR;
    sp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sp.addressModeU = sp.addressModeV = sp.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sp.compareEnable = VK_TRUE;
    sp.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(ctx.device(), &sp, nullptr, &shadow_sampler) !=
        VK_SUCCESS) {
      return false;
    }

    VkAttachmentDescription ad{};
    ad.format = VK_FORMAT_D32_SFLOAT;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ad.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference dr{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd{};
    sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sd.colorAttachmentCount = 0;
    sd.pDepthStencilAttachment = &dr;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1; rpci.pAttachments = &ad;
    rpci.subpassCount = 1; rpci.pSubpasses = &sd;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    if (vkCreateRenderPass(ctx.device(), &rpci, nullptr, &shadow_rp) !=
        VK_SUCCESS) {
      return false;
    }
    VkFramebufferCreateInfo fbi{};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = shadow_rp;
    fbi.attachmentCount = 1; fbi.pAttachments = &shadow_depth_view;
    fbi.width = kShadowRes; fbi.height = kShadowRes; fbi.layers = 1;
    if (vkCreateFramebuffer(ctx.device(), &fbi, nullptr, &shadow_fb) !=
        VK_SUCCESS) {
      return false;
    }

    auto ub = alloc.create_buffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!ub.is_ok()) return false;
    shadow_ubo_alloc = ub.value();

    auto r3 = desc.create_layout({
        {3, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, 1, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT}}, 1);
    if (!r3.is_ok()) return false;
    shadow_layout = r3.value();
    auto ss = desc.allocate_set(shadow_layout);
    if (!ss.is_ok()) return false;
    shadow_set = ss.value();
    if (!desc.write_buffer(shadow_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           shadow_ubo_alloc.buffer, 0, 64).is_ok()) {
      return false;
    }
    if (!desc.write_image(shadow_set, 1,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          shadow_sampler, shadow_sample_view,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0)
             .is_ok()) {
      return false;
    }

    // Depth-only shadow pipeline (128-byte push: light VP + model).
    const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
    if (!shadow_pipe.load_shader_stage_file(ctx.device(),
            shader_dir + "/shadow.vert.spv", "vertex").is_ok() ||
        !shadow_pipe.load_shader_stage_file(ctx.device(),
            shader_dir + "/shadow.frag.spv", "fragment").is_ok()) {
      return false;
    }
    VkDescriptorSetLayout solo[1] = {mesh_layout};
// 144 bytes: shadow.vert's push block gained a trailing uvec4 so it stays
    // layout-compatible with shadow_skinned.vert and one pipeline layout can serve
    // both vertex stages. A 128-byte range leaves the shader's block outside the
    // layout (VUID-VkGraphicsPipelineCreateInfo-layout-10069).
        VkPushConstantRange spush{VK_SHADER_STAGE_VERTEX_BIT, 0, 144};
    if (!shadow_pipe.create_pipeline_layout(ctx.device(), solo, 1, &spush)
             .is_ok() ||
        !shadow_pipe.create_graphics_pipeline(ctx.device(), shadow_rp,
            VK_FORMAT_D32_SFLOAT, shadow_pipe.pipeline_layout(), true, true,
            true, /*depth_bias_slope=*/8.0f, /*dynamic_depth_bias=*/true).is_ok()) {
      return false;
    }
    return true;
  }

  void write_light_vp(const SceneMatrix& lvp) {
    std::memcpy(shadow_ubo_alloc.mapped, lvp.data(), 64);
  }
  void write_materials(const PbrMaterialData* m, std::uint32_t n) {
    std::memcpy(mat_buf_alloc.mapped, m, n * sizeof(PbrMaterialData));
  }

  void cleanup() {
    shadow_pipe.cleanup(ctx.device());
    target.cleanup(ctx.device());
    destroy_solid(ctx.device(), alloc, white);
    if (cube.va.is_valid()) alloc.destroy_allocation(cube.va);
    if (cube.ia.is_valid()) alloc.destroy_allocation(cube.ia);
    if (mat_buf_alloc.is_valid()) alloc.destroy_allocation(mat_buf_alloc);
    if (shadow_ubo_alloc.is_valid())
      alloc.destroy_allocation(shadow_ubo_alloc);
    if (shadow_mem.is_valid()) alloc.destroy_allocation(shadow_mem);
    if (shadow_img) vkDestroyImage(ctx.device(), shadow_img, nullptr);
    if (shadow_depth_view)
      vkDestroyImageView(ctx.device(), shadow_depth_view, nullptr);
    if (shadow_sample_view)
      vkDestroyImageView(ctx.device(), shadow_sample_view, nullptr);
    if (shadow_sampler) vkDestroySampler(ctx.device(), shadow_sampler, nullptr);
    if (shadow_rp) vkDestroyRenderPass(ctx.device(), shadow_rp, nullptr);
    if (shadow_fb) vkDestroyFramebuffer(ctx.device(), shadow_fb, nullptr);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

bool submit_and_wait(VkDevice dev, VkQueue queue, VkCommandBuffer cb) {
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence{VK_NULL_HANDLE};
  if (vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS) return false;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
  if (r == VK_SUCCESS) {
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  }
  vkDestroyFence(dev, fence, nullptr);
  return r == VK_SUCCESS;
}

//! CPU ground truth: does segment o + s*L (s in (eps, far)) hit the AABB
//! (center c, half-extents h)? Slab method.
bool segment_hits_box(const std::array<float, 3>& o,
                      const std::array<float, 3>& L,
                      const std::array<float, 3>& c,
                      const std::array<float, 3>& h) {
  float tmin = 0.001f;
  float tmax = 1.0e30f;
  for (std::size_t k = 0; k < static_cast<std::size_t>(3); ++k) {
    if (std::fabs(L[k]) < 1.0e-9f) {
      if (o[k] < c[k] - h[k] || o[k] > c[k] + h[k]) return false;
      continue;
    }
    const float inv = 1.0f / L[k];
    float t0 = (c[k] - h[k] - o[k]) * inv;
    float t1 = (c[k] + h[k] - o[k]) * inv;
    if (t0 > t1) std::swap(t0, t1);
    tmin = std::max(tmin, t0);
    tmax = std::min(tmax, t1);
    if (tmin > tmax) return false;
  }
  return true;
}

}  // namespace

// ============================================================================
// Proof: ray-query hard shadows vs PCF on one scene.
// ============================================================================
TEST(rt_shadows, ray_query_hard_shadow_matches_pcf) {
  if (!VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  RtShadowHarness h;
  if (!h.init("rt_shadow_test")) {
    GTEST_SKIP() << "Vulkan/RT unavailable";
  }

  // ---- Scene constants -----------------------------------------------------
  const std::array<float, 3> kLight = normalize3({0.3f, 0.65f, 0.7f});
  const std::array<float, 3> near_pos = {0.0f, 0.0f, -2.0f};
  const std::array<float, 3> near_h = {0.5f, 0.5f, 0.5f};
  // Far cube placed so its camera-facing face center's ray toward the light
  // lands at ~(0.007, 0.007, -2) — inside the near cube's light column:
  // face_center + t*kLight hits the near box at t = 2.7/kLight.z.
  const std::array<float, 3> far_pos = {-1.15f, -2.5f, -5.0f};
  const float far_half = 0.3f;
  const std::array<float, 3> cam_eye = {0.0f, 0.0f, 10.0f};
  const std::array<float, 3> face_center = {far_pos[0], far_pos[1],
                                            far_pos[2] + far_half};

  // Load-bearing analytic claim: the far face center IS occluded along the
  // light direction by the near cube (else the test geometry is wrong).
  ASSERT_TRUE(segment_hits_box(face_center, kLight, near_pos, near_h))
      << "face center must be analytically occluded";

  // The engine pushes view_projection straight into gl_Position, so the
  // matrix MUST be proj*view composed — a bare perspective would place the
  // effective camera at the world origin and invalidate every pixel probe.
  const SceneMatrix cam_view = make_look_at(cam_eye, {0.0f, 0.0f, 0.0f}, {0, 1, 0});
  const SceneMatrix cam_vp =
      mat4_multiply(make_perspective(45.0f, 1.0f, 0.1f, 100.0f), cam_view);
  // True light view: look-at along kLightDir centered on the occluder, so
  // the depth map captures the occluder's shadow column correctly. The light
  // sits on the TO-LIGHT side (shaders use l = kLightDir as the direction
  // from the surface toward the light).
  const std::array<float, 3> light_eye = {
      near_pos[0] + kLight[0] * 12.0f, near_pos[1] + kLight[1] * 12.0f,
      near_pos[2] + kLight[2] * 12.0f};
  const SceneMatrix light_view = make_look_at(light_eye, near_pos, {0, 1, 0});
  const SceneMatrix light_ortho =
      make_ortho(-4.0f, 4.0f, -4.0f, 4.0f, -20.0f, 20.0f);
  const SceneMatrix light_vp = mat4_multiply(light_ortho, light_view);
  h.write_light_vp(light_vp);

  PbrMaterialData mat{};
  mat.base_color_factor = {0.8f, 0.8f, 0.8f, 1.0f};
  mat.metallic_factor = 0.0f;
  mat.roughness_factor = 0.5f;
  h.write_materials(&mat, 1);

  // ---- TLAS: two translated box instances ----------------------------------
  const std::vector<float> box_near = make_box_triangles(0.5f, 0.5f, 0.5f);
  const std::vector<float> box_far =
      make_box_triangles(far_half, far_half, far_half);
  auto geom_near = h.alloc.create_buffer(
      box_near.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto geom_far = h.alloc.create_buffer(
      box_far.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geom_near.is_ok() && geom_far.is_ok());
  std::memcpy(geom_near.value().mapped, box_near.data(),
              box_near.size() * sizeof(float));
  std::memcpy(geom_far.value().mapped, box_far.data(),
              box_far.size() * sizeof(float));

  auto address_of = [&](VkBuffer buffer) -> std::uint64_t {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return static_cast<std::uint64_t>(
        vkGetBufferDeviceAddress(h.ctx.device(), &info));
  };

  BlasBuildInput in_near{};
  in_near.vertex_buffer_address = address_of(geom_near.value().buffer);
  in_near.triangle_count = 12U;
  in_near.max_vertex = 12U * 3U - 1U;
  BlasBuildInput in_far{};
  in_far.vertex_buffer_address = address_of(geom_far.value().buffer);
  in_far.triangle_count = 12U;
  in_far.max_vertex = 12U * 3U - 1U;
  ASSERT_NE(in_near.vertex_buffer_address, 0U);
  ASSERT_NE(in_far.vertex_buffer_address, 0U);

  VulkanAccelerationStructureBuilder builder;
  auto blas_near = builder.create_blas(h.ctx.device(), h.alloc, in_near);
  auto blas_far = builder.create_blas(h.ctx.device(), h.alloc, in_far);
  ASSERT_TRUE(blas_near.is_ok());
  ASSERT_TRUE(blas_far.is_ok());
  auto tlas = builder.create_tlas(h.ctx.device(), h.alloc, 2U);
  ASSERT_TRUE(tlas.is_ok());
  std::array<TlasInstance, 2> instances{};
  {
    const std::array<float, 12> xf =
        translated_instance(near_pos[0], near_pos[1], near_pos[2]);
    std::memcpy(instances[0].transform, xf.data(), sizeof(xf));
  }
  instances[0].instance_custom_index = 7U;
  instances[0].blas_device_address = blas_near.value().device_address;
  {
    const std::array<float, 12> xf =
        translated_instance(far_pos[0], far_pos[1], far_pos[2]);
    std::memcpy(instances[1].transform, xf.data(), sizeof(xf));
  }
  instances[1].instance_custom_index = 9U;
  instances[1].blas_device_address = blas_far.value().device_address;

  // Scratch: sized to the largest single build, per the first-contact test.
  const auto sizes_near =
      VulkanAccelerationStructureBuilder::query_blas_sizes(h.ctx.device(),
                                                           in_near);
  const auto sizes_far =
      VulkanAccelerationStructureBuilder::query_blas_sizes(h.ctx.device(),
                                                           in_far);
  const auto tlas_sizes =
      VulkanAccelerationStructureBuilder::query_tlas_sizes(h.ctx.device(), 2U);
  const std::uint64_t scratch_bytes =
      std::max({sizes_near.buildScratchSize, sizes_far.buildScratchSize,
                tlas_sizes.buildScratchSize});
  VulkanScratchPool scratch_pool;
  auto scratch_addr =
      scratch_pool.acquire(h.alloc, h.ctx.device(), scratch_bytes);
  ASSERT_TRUE(scratch_addr.is_ok());

  // ---- Pipelines -----------------------------------------------------------
  const std::string sd = WARPLOOM_TEST_SHADER_DIR;

  VulkanPipeline pipe_a;  // PCF
  ASSERT_TRUE(pipe_a.load_shader_stage_file(h.ctx.device(),
      sd + "/pbr_scene.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipe_a.load_shader_stage_file(h.ctx.device(),
      sd + "/pbr_shadow.frag.spv", "fragment").is_ok());
  VkDescriptorSetLayout layouts_a[4] = {h.mesh_layout, h.tex_layout,
                                        h.mat_layout, h.shadow_layout};
  VkPushConstantRange push_a{
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
  ASSERT_TRUE(pipe_a.create_pipeline_layout(h.ctx.device(), layouts_a, 4,
                                            &push_a).is_ok());
  ASSERT_TRUE(pipe_a.create_graphics_pipeline(h.ctx.device(),
      h.target.render_pass(), h.target.format(), pipe_a.pipeline_layout(),
      true, true, true).is_ok());

  auto as_layout = h.desc.create_layout(
      {{3, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        VK_SHADER_STAGE_FRAGMENT_BIT}}, 1);
  ASSERT_TRUE(as_layout.is_ok());
  auto as_set = h.desc.allocate_set(as_layout.value());
  ASSERT_TRUE(as_set.is_ok());
  VulkanPipeline pipe_b;  // RT shadow
  ASSERT_TRUE(pipe_b.load_shader_stage_file(h.ctx.device(),
      sd + "/pbr_scene.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipe_b.load_shader_stage_file(h.ctx.device(),
      sd + "/pbr_rt_shadow.frag.spv", "fragment").is_ok());
  VkDescriptorSetLayout layouts_b[4] = {h.mesh_layout, h.tex_layout,
                                        h.mat_layout, as_layout.value()};
  ASSERT_TRUE(pipe_b.create_pipeline_layout(h.ctx.device(), layouts_b, 4,
                                            &push_a).is_ok());
  ASSERT_TRUE(pipe_b.create_graphics_pipeline(h.ctx.device(),
      h.target.render_pass(), h.target.format(), pipe_b.pipeline_layout(),
      true, true, true).is_ok());
  ASSERT_TRUE(h.desc.write_acceleration_structure(
      as_set.value(), 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
      tlas.value().handle, 0).is_ok());

  // ---- Tracer compute pass (TLAS ground-truth sanity) ----------------------
  VulkanPipeline tracer;
  ASSERT_TRUE(tracer.load_shader_stage_file(h.ctx.device(),
      sd + "/rt_shadows_a_vs_b.comp.spv", "compute").is_ok());
  auto tracer_layout = h.desc.create_layout(
      {{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT},
       {0, 1, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        VK_SHADER_STAGE_COMPUTE_BIT}}, 1);
  ASSERT_TRUE(tracer_layout.is_ok());
  VkPushConstantRange tracer_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
  ASSERT_TRUE(tracer.create_pipeline_layout(h.ctx.device(),
      &tracer_layout.value(), 1, &tracer_push).is_ok());
  ASSERT_TRUE(tracer.create_compute_pipeline(h.ctx.device(),
      tracer.pipeline_layout()).is_ok());
  auto tracer_set = h.desc.allocate_set(tracer_layout.value());
  ASSERT_TRUE(tracer_set.is_ok());
  constexpr std::uint32_t kRays = kTracerDim * kTracerDim;
  auto payload = h.alloc.create_buffer(kRays * 8 * sizeof(float),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(payload.is_ok());
  ASSERT_TRUE(h.desc.write_buffer(tracer_set.value(), 0,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, payload.value().buffer, 0,
      VK_WHOLE_SIZE).is_ok());
  ASSERT_TRUE(h.desc.write_acceleration_structure(
      tracer_set.value(), 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
      tlas.value().handle, 0).is_ok());

  // ---- Command buffers + submissions ---------------------------------------
  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      h.ctx.device(), h.qf);
  ASSERT_TRUE(pool.is_ok());
  auto cb_build = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      h.ctx.device(), pool.value());
  auto cb_trace = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      h.ctx.device(), pool.value());
  ASSERT_TRUE(cb_build.is_ok() && cb_trace.is_ok());
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  // Build: BLAS near, barrier, BLAS far, barrier, TLAS.
  ASSERT_EQ(vkBeginCommandBuffer(cb_build.value(), &bi), VK_SUCCESS);
  ASSERT_TRUE(builder
                  .cmd_build_blas(cb_build.value(), h.ctx.device(),
                                  blas_near.value(), in_near,
                                  scratch_addr.value())
                  .is_ok());
  {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cb_build.value(),
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
  }
  ASSERT_TRUE(builder
                  .cmd_build_blas(cb_build.value(), h.ctx.device(),
                                  blas_far.value(), in_far,
                                  scratch_addr.value())
                  .is_ok());
  {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cb_build.value(),
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
  }
  ASSERT_TRUE(builder.cmd_build_tlas(cb_build.value(), h.ctx.device(),
                                     tlas.value(), instances.data(), 2U,
                                     scratch_addr.value()).is_ok());
  ASSERT_EQ(vkEndCommandBuffer(cb_build.value()), VK_SUCCESS);
  ASSERT_TRUE(submit_and_wait(h.ctx.device(), h.ctx.graphics_queue(),
                              cb_build.value()));

  // Trace: camera-ray grid, results read back for CPU verification.
  ASSERT_EQ(vkBeginCommandBuffer(cb_trace.value(), &bi), VK_SUCCESS);
  {
    auto* p = static_cast<float*>(payload.value().mapped);
    for (std::uint32_t i = 0; i < kRays; ++i) {
      p[8 * i + 0] = cam_eye[0];
      p[8 * i + 1] = cam_eye[1];
      p[8 * i + 2] = cam_eye[2];
      p[8 * i + 3] = 0.0f;
      p[8 * i + 4] = 0.0f;
      p[8 * i + 5] = 0.0f;
      p[8 * i + 6] = 0.0f;
      p[8 * i + 7] = 0.0f;
    }
  }
  vkCmdBindPipeline(cb_trace.value(), VK_PIPELINE_BIND_POINT_COMPUTE,
                    tracer.pipeline());
  VkDescriptorSet tsets[1] = {tracer_set.value()};
  vkCmdBindDescriptorSets(cb_trace.value(), VK_PIPELINE_BIND_POINT_COMPUTE,
                          tracer.pipeline_layout(), 0, 1, tsets, 0, nullptr);
  {
    const float tan_half = std::tan(45.0f * 3.14159265f / 360.0f);
    std::array<float, 16> pc{};
    // Projection flips NDC y (Vulkan convention), so the tracer's virtual
    // image plane must too: cam_up.w = -tan_half makes +y rays go UP.
    const std::array<float, 4> cam_right = {1.0f, 0.0f, 0.0f, tan_half};
    const std::array<float, 4> cam_up = {0.0f, 1.0f, 0.0f, -tan_half};
    const std::array<float, 4> cam_fwd = {0.0f, 0.0f, -1.0f, 1.0f};
    std::memcpy(&pc[0], cam_right.data(), 16);
    std::memcpy(&pc[4], cam_up.data(), 16);
    std::memcpy(&pc[8], cam_fwd.data(), 16);
    const std::uint32_t dims[2] = {kTracerDim, kTracerDim};
    std::memcpy(&pc[12], dims, 8);
    vkCmdPushConstants(cb_trace.value(), tracer.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, pc.data());
  }
  vkCmdDispatch(cb_trace.value(), kTracerDim / 16U, kTracerDim / 16U, 1U);
  ASSERT_EQ(vkEndCommandBuffer(cb_trace.value()), VK_SUCCESS);
  ASSERT_TRUE(submit_and_wait(h.ctx.device(), h.ctx.graphics_queue(),
                              cb_trace.value()));

  // CPU verification of the tracer results.
  std::uint32_t near_hits = 0;
  std::uint32_t far_hits = 0;
  std::uint32_t near_occluded = 0;
  std::uint32_t far_occluded = 0;
  {
    const auto* p = static_cast<const float*>(payload.value().mapped);
    for (std::uint32_t i = 0; i < kRays; ++i) {
      const std::uint32_t custom = static_cast<std::uint32_t>(p[8 * i + 7]);
      const std::array<float, 3> hit = {p[8 * i + 4], p[8 * i + 5],
                                        p[8 * i + 6]};
      if (custom == 7U) {
        ++near_hits;
        if (segment_hits_box(hit, kLight, near_pos, near_h)) ++near_occluded;
        continue;
      }
      if (custom != 9U) continue;
      ++far_hits;
      if (segment_hits_box(hit, kLight, near_pos, near_h)) ++far_occluded;
    }
  }
  EXPECT_GT(near_hits, 0U) << "tracer never hit the near cube";
  EXPECT_GT(far_hits, 0U) << "tracer never hit the far cube";
  if (std::getenv("WARPLOOM_RT_SHADOW_DEBUG") != nullptr) {
    const auto* p = static_cast<const float*>(payload.value().mapped);
    std::printf("tracer grid (custom index per ray, .=miss):\n");
    for (std::uint32_t row = 0; row < kTracerDim; ++row) {
      std::string line;
      for (std::uint32_t col = 0; col < kTracerDim; ++col) {
        const std::uint32_t custom =
            static_cast<std::uint32_t>(p[8 * (row * kTracerDim + col) + 7]);
        line += custom == 0U   ? '.'
                : custom == 7U ? 'A'
                : custom == 9U ? 'B'
                               : '?';
      }
      std::printf("  |%s|\n", line.c_str());
    }
    std::printf("near_hits=%u far_hits=%u near_occ=%u far_occ=%u\n",
                near_hits, far_hits, near_occluded, far_occluded);
    for (std::uint32_t i = 0; i < kRays; ++i) {
      const std::uint32_t custom =
          static_cast<std::uint32_t>(p[8 * i + 7]);
      if (custom != 0U) {
        std::printf(
            "ray %2u (r%u,c%u) custom=%u hit=(%.3f, %.3f, %.3f)\n", i,
            i / kTracerDim, i % kTracerDim, custom,
            static_cast<double>(p[8 * i + 4]),
            static_cast<double>(p[8 * i + 5]),
            static_cast<double>(p[8 * i + 6]));
      }
    }
    std::fflush(stdout);
  }
  // Discrimination proof: the near cube's camera-facing face is fully lit
  // (nothing between it and the light) and the far cube's face is fully
  // inside the occluder's light column — per-ray analytic classification
  // must match exactly.
  EXPECT_EQ(near_occluded, 0U) << "lit near-cube surface classified occluded";
  EXPECT_EQ(far_hits, far_occluded)
      << "far-cube surface outside the analytic shadow column";

  // ---- Render both lit paths through record_pbr_frame ----------------------
  auto render_path = [&](VulkanPipeline& pipe, VkDescriptorSet shadow_desc,
                         bool with_shadow_prepass)
      -> omnicpp_test::ReadbackResult {
    VulkanPbrScene scene{};
    scene.pipeline = pipe.pipeline();
    scene.pipeline_layout = pipe.pipeline_layout();
    scene.camera.view_projection = cam_vp;
    scene.camera_position = {cam_eye[0], cam_eye[1], cam_eye[2], 1.0f};
    scene.texture_set = h.tex_set;
    scene.material_set = h.mat_set;
    scene.shadow_set = shadow_desc;
    scene.shadow_set_slot = 3U;
    if (with_shadow_prepass) {
      scene.shadow_pipeline = h.shadow_pipe.pipeline();
      scene.shadow_pipeline_layout = h.shadow_pipe.pipeline_layout();
      scene.shadow_light_vp = light_vp;
      scene.shadow_width = kShadowRes;
      scene.shadow_height = kShadowRes;
    }
    ScenePbrObject a{};
    a.mesh = &h.cube.mesh;
    a.model = make_translation(near_pos[0], near_pos[1], near_pos[2]);
    a.material_index = 0;
    ScenePbrObject b{};
    b.mesh = &h.cube.mesh;
    b.model = make_translation(far_pos[0], far_pos[1], far_pos[2]);
    b.material_index = 0;
    scene.objects = {a, b};

    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(
        h.ctx.device(), h.qf);
    if (!pr.is_ok()) return {};
    auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        h.ctx.device(), pr.value());
    if (!cr.is_ok()) {
      vkDestroyCommandPool(h.ctx.device(), pr.value(), nullptr);
      return {};
    }
    VkCommandBuffer rcb = cr.value();
    VkCommandBufferBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(rcb, &rbi);

    omnicpp::render::VulkanRenderer renderer;
    omnicpp::render::VulkanRenderer::PbrFrameTargets targets{};
    if (with_shadow_prepass) {
      targets.shadow_render_pass = h.shadow_rp;
      targets.shadow_framebuffer = h.shadow_fb;
      targets.shadow_image = h.shadow_img;
      targets.shadow_format = VK_FORMAT_D32_SFLOAT;
      targets.shadow_width = kShadowRes;
      targets.shadow_height = kShadowRes;
    }
    targets.render_pass = h.target.render_pass();
    targets.framebuffer = h.target.framebuffer();
    targets.width = kImg;
    targets.height = kImg;
    VkClearValue frame_clears[2]{};
    frame_clears[0].color = {{0, 0, 0, 1}};
    frame_clears[1].depthStencil = {1.0f, 0};
    targets.clear_values = frame_clears;
    targets.clear_value_count = 2;
    const auto frame_r = renderer.record_pbr_frame(rcb, scene, targets);
    if (!frame_r.is_ok()) {
      vkEndCommandBuffer(rcb);
      vkDestroyCommandPool(h.ctx.device(), pr.value(), nullptr);
      return {};
    }
    vkEndCommandBuffer(rcb);
    submit_and_wait(h.ctx.device(), h.ctx.graphics_queue(), rcb);
    vkDestroyCommandPool(h.ctx.device(), pr.value(), nullptr);
    return omnicpp_test::readback_swapchain_image(
        h.ctx.physical_device(), h.ctx.device(), h.ctx.graphics_queue(), h.qf,
        h.target.image(), h.target.format(), kImg, kImg,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*store_pixels=*/true);
  };

  const omnicpp_test::ReadbackResult ra =
      render_path(pipe_a, h.shadow_set, true);
  const omnicpp_test::ReadbackResult rb =
      render_path(pipe_b, as_set.value(), false);
  ASSERT_TRUE(ra.submitted) << "PCF path failed to render";
  ASSERT_TRUE(rb.submitted) << "RT path failed to render";
  // Correct camera (perspective*view, y-flip): near face ~676 px + far cube
  // ~576 px => ~1199 non-clear pixels on both paths. Measured empirically and
  // cross-checked against the projected silhouettes; parity must be exact.
  EXPECT_GT(ra.non_clear_pixels, 1000U) << "PCF path rendered nothing";
  EXPECT_GT(rb.non_clear_pixels, 1000U) << "RT path rendered nothing";
  EXPECT_EQ(ra.non_clear_pixels, rb.non_clear_pixels)
      << "paths disagree on rasterized coverage";

  // ---- Pixel classification -------------------------------------------------
  // Project a world point to a pixel: NDC x = wx/(d*tan), d = eye_z - wz
  // (camera looks down -Z from cam_eye). The projection flips NDC y (Vulkan
  // convention, see make_perspective), so up in world = up in image:
  // py = (1 - (y+1)/2) * H.
  auto project = [&](const std::array<float, 3>& p) -> std::pair<int, int> {
    const float d = cam_eye[2] - p[2];
    const float tan_half = std::tan(45.0f * 3.14159265f / 360.0f);
    const float x = p[0] / (d * tan_half);
    const float y = p[1] / (d * tan_half);
    const int px = static_cast<int>((x * 0.5f + 0.5f) * float(kImg));
    const int py = static_cast<int>((1.0f - (y * 0.5f + 0.5f)) * float(kImg));
    return {px, py};
  };
  auto luminance = [&](const omnicpp_test::ReadbackResult& r, int px, int py) -> float {
    if (px < 0 || px >= static_cast<int>(kImg) || py < 0 ||
        py >= static_cast<int>(kImg)) {
      ADD_FAILURE() << "probe projected off-image: (" << px << ", " << py << ")";
      return 0.0f;
    }
    if (r.pixels.size() != static_cast<std::size_t>(kImg) * kImg) {
      ADD_FAILURE() << "readback size mismatch: " << r.pixels.size();
      return 0.0f;
    }
    const std::uint32_t packed =
        r.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(kImg) +
                 static_cast<std::size_t>(px)];
    const float rf = static_cast<float>(packed & 0xffU) / 255.0f;
    const float gf = static_cast<float>((packed >> 8) & 0xffU) / 255.0f;
    const float bf = static_cast<float>((packed >> 16) & 0xffU) / 255.0f;
    return 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
  };
  // TEMP DIAGNOSTIC: exact geometry of the PCF readback. Prints per-object
  // bounding boxes of non-black pixels (row extents at each occupied row are
  // summarized as min/max), plus luminance at the probe sites.
  if (std::getenv("WARPLOOM_RT_SHADOW_DEBUG") != nullptr) {
    std::size_t min_px = kImg, max_px = 0, min_py = kImg, max_py = 0;
    bool any_lit = false;
    const std::size_t img = static_cast<std::size_t>(kImg);
    for (std::size_t py = 0; py < img; ++py) {
      std::size_t row_min = kImg, row_max = 0;
      bool row_any = false;
      for (std::size_t px = 0; px < img; ++px) {
        const std::uint32_t packed =
            ra.pixels[static_cast<std::size_t>(py) * kImg + px];
        const float rf = static_cast<float>(packed & 0xffU) / 255.0f;
        const float gf = static_cast<float>((packed >> 8) & 0xffU) / 255.0f;
        const float bf = static_cast<float>((packed >> 16) & 0xffU) / 255.0f;
        const float lum = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
        if (lum > 0.02f) {
          row_min = row_any ? std::min(row_min, px) : px;
          row_max = row_any ? std::max(row_max, px) : px;
          row_any = true;
          any_lit = true;
          min_py = any_lit ? std::min(min_py, py) : py;
          max_py = any_lit ? std::max(max_py, py) : py;
        }
      }
      if (row_any) {
        min_px = std::min(min_px, row_min);
        max_px = std::max(max_px, row_max);
        if (py % 8 == 0 || row_min != row_max) {
          std::printf("row %3zu: px [%3zu..%3zu]\n", py, row_min, row_max);
        }
      }
    }
    std::printf("overall bbox: px [%zu..%zu] py [%zu..%zu]\n", min_px,
                max_px, min_py, max_py);
    std::printf("probe near (128,128) lum=%.4f packed=0x%08x (r=%u g=%u b=%u)\n",
                static_cast<double>(luminance(ra, 128, 128)), ra.pixels[128U * kImg + 128U],
                ra.pixels[128U * kImg + 128U] & 0xffU,
                (ra.pixels[128U * kImg + 128U] >> 8) & 0xffU,
                (ra.pixels[128U * kImg + 128U] >> 16) & 0xffU);

    // Shadow-map dump: linear -> staging -> host, sample a grid of depths.
    {
      auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
          h.ctx.device(), h.qf);
      auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          h.ctx.device(), pool_r.value());
      auto stage = h.alloc.create_buffer(
          kShadowRes * kShadowRes * 4,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (pool_r.is_ok() && cb_r.is_ok() && stage.is_ok()) {
        VkCommandBufferBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb_r.value(), &rbi);
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_dst.image = h.shadow_img;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb_r.value(),
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_dst);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.imageExtent = {kShadowRes, kShadowRes, 1};
        vkCmdCopyImageToBuffer(cb_r.value(), h.shadow_img,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               stage.value().buffer, 1, &region);
        vkEndCommandBuffer(cb_r.value());
        submit_and_wait(h.ctx.device(), h.ctx.graphics_queue(), cb_r.value());
        const auto* d = static_cast<const float*>(stage.value().mapped);
        float mn = 1.0f, mx = 0.0f;
        std::uint32_t written = 0;
        for (std::uint32_t y = 0; y < kShadowRes; y += 16) {
          for (std::uint32_t x = 0; x < kShadowRes; x += 16) {
            const float v = d[y * kShadowRes + x];
            if (v > 0.0f && v < 1.0f) ++written;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
          }
        }
        std::printf(
            "shadow map: sampled<1.0 count=%u  min=%.4f max=%.4f\n", written,
            static_cast<double>(mn), static_cast<double>(mx));
        // Central 64x64 block (the occluder should occupy ~the middle):
        float c_mn = 1.0f, c_mx = 0.0f;
        for (std::uint32_t y = 480; y < 544; ++y) {
          for (std::uint32_t x = 480; x < 544; ++x) {
            c_mn = std::min(c_mn, d[y * kShadowRes + x]);
            c_mx = std::max(c_mx, d[y * kShadowRes + x]);
          }
        }
        std::printf("shadow map center block: min=%.4f max=%.4f\n", static_cast<double>(c_mn), static_cast<double>(c_mx));
        // Occupied bbox (non-background) of the whole map.
        {
          int bmn_x = kShadowRes, bmx_x = -1, bmn_y = kShadowRes, bmx_y = -1;
          for (std::uint32_t y = 0; y < kShadowRes; ++y) {
            for (std::uint32_t x = 0; x < kShadowRes; ++x) {
              if (d[y * kShadowRes + x] < 0.999f) {
                bmn_x = std::min<int>(bmn_x, static_cast<int>(x));
                bmx_x = std::max<int>(bmx_x, static_cast<int>(x));
                bmn_y = std::min<int>(bmn_y, static_cast<int>(y));
                bmx_y = std::max<int>(bmx_y, static_cast<int>(y));
              }
            }
          }
          std::printf("shadow map occupied bbox: x [%d..%d] y [%d..%d]\n",
                      bmn_x, bmx_x, bmn_y, bmx_y);
        }
        // Fragment lookup site: shadowCoord=(0.478,0.541) -> texel(489,554)
        // on the 1024^2 map. 3x3 around it, plus a vertical depth profile.
        {
          const std::uint32_t lx = 489U, ly = 554U;
          std::printf("map at lookup texel (%u,%u) 3x3:\n", lx, ly);
          for (std::uint32_t y = ly - 1U; y <= ly + 1U; ++y) {
            std::printf(
                "  %.5f %.5f %.5f\n",
                static_cast<double>(d[(y) * kShadowRes + lx - 1U]),
                static_cast<double>(d[y * kShadowRes + lx]),
                static_cast<double>(d[y * kShadowRes + lx + 1U]));
          }
          std::printf("vertical profile x=%u, y 530..590 step 4:\n", lx);
          for (std::uint32_t y = 530U; y <= 590U; y += 4U) {
            std::printf("  y=%3u  %.5f\n", y,
                        static_cast<double>(d[y * kShadowRes + lx]));
          }
        }
        std::printf("shadow map ASCII (32x32, char = depth band):\n");
        for (std::uint32_t y = 0; y < kShadowRes; y += 32) {
          std::string line;
          for (std::uint32_t x = 0; x < kShadowRes; x += 32) {
            const float v = d[y * kShadowRes + x];
            line += v >= 0.999f ? '.'
                    : v >= 0.9f ? '9'
                    : v >= 0.8f ? '8'
                    : v >= 0.7f ? '7'
                    : v >= 0.6f ? '6'
                                : 'x';
          }
          std::printf("  |%s|\n", line.c_str());
        }
        std::fflush(stdout);
      }
      if (cb_r.is_ok())
        vkDestroyCommandPool(h.ctx.device(), pool_r.value(), nullptr);
      if (stage.is_ok()) {
        Allocation st = stage.value();
        h.alloc.destroy_allocation(st);
      }
    }
    std::fflush(stdout);
  }


  const auto [nx, ny] = project({0.0f, 0.0f, -1.5f});   // near cube front face
  const auto [fx, fy] = project(face_center);           // far cube +Z face

  const float near_a = luminance(ra, nx, ny);
  const float near_b = luminance(rb, nx, ny);
  const float far_a = luminance(ra, fx, fy);
  const float far_b = luminance(rb, fx, fy);

  // Near cube: lit on both paths, and the direct lighting matches (identical
  // shading math; PCF factor is 1.0 on lit texels, ray query unoccluded).
  EXPECT_GT(near_a, 0.30f) << "PCF path: near cube not lit";
  EXPECT_GT(near_b, 0.30f) << "RT path: near cube not lit";
  EXPECT_LT(std::fabs(near_a - near_b), 0.05f)
      << "lit shading diverges between paths";

  // Far cube: hard-shadowed on BOTH paths (ambient-only floor ~0.18, lit
  // would be ~0.47 — the thresholds sit in the gap).
  EXPECT_LT(far_a, 0.25f) << "PCF path: far cube not shadowed";
  EXPECT_LT(far_b, 0.25f) << "RT path: far cube not shadowed";
  EXPECT_GT(far_a, 0.10f) << "PCF path: far cube below ambient floor";
  EXPECT_GT(far_b, 0.10f) << "RT path: far cube below ambient floor";

  omnicpp::render::BottomLevelAS out_near = blas_near.value();
  omnicpp::render::BottomLevelAS out_far = blas_far.value();
  omnicpp::render::TopLevelAS out_t = tlas.value();
  Allocation out_gn = geom_near.value();
  Allocation out_gf = geom_far.value();
  builder.destroy_blas(h.ctx.device(), h.alloc, out_near);
  builder.destroy_blas(h.ctx.device(), h.alloc, out_far);
  builder.destroy_tlas(h.ctx.device(), h.alloc, out_t);
  h.alloc.destroy_allocation(out_gn);
  h.alloc.destroy_allocation(out_gf);
  scratch_pool.cleanup(h.alloc);
  pipe_a.cleanup(h.ctx.device());
  pipe_b.cleanup(h.ctx.device());
  tracer.cleanup(h.ctx.device());
  vkDestroyCommandPool(h.ctx.device(), pool.value(), nullptr);
  h.cleanup();
}

#endif
