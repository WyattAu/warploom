//! @file test_rt_reflections.cpp
//! @brief E2 GPU proof: ray-query reflections in the lit raster path.
//!
//! Scene: a gray mirror floor slab (top at y = -0.5) and a red unit cube
//! resting on it (center (0, 0, -3)). The reflection fragment shader casts
//! one ray along reflect(-v, n) against the scene TLAS and tints the surface
//! with a shade of the hit instance's color (SSBO indexed by
//! instanceCustomIndex, same indexing as the TLAS build).
//!
//! Ground truth is the classic virtual-image construction: the mirror image
//! of the cube center B = (0, 0, -3) across the plane y = -0.5 is
//! B' = (0, -1, -3). The camera ray aimed at B' crosses the mirror at
//! P = (0, -0.5, -1.818) (below the cube, so the floor is the first hit) and
//! the reflected up-ray travels exactly toward B, entering the cube's front
//! face. Therefore the pixel at project(B') must be RED-dominant, while a
//! control floor pixel at Q = (1.2, -0.5, -1.0) — whose reflected ray misses
//! everything — must stay neutral gray. Both claims are CPU-verified with a
//! slab/AABB segment test before the GPU render.
//!
//! Proofs:
//!   1. CPU analytic: reflected ray at P hits the cube AABB; at Q it misses.
//!   2. Direct view of the cube renders red (material path sanity).
//!   3. Mirror pixel shows the reflected cube: red >> green/blue.
//!   4. Control floor pixel: channels within tolerance of each other.
//!   5. Zero validation errors (run under VK_LAYER_KHRONOS_validation).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/render/vulkan_acceleration_structure.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_frame_upload.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_scene.hpp"
#include "vulkan_test_readback.hpp"

namespace {

using omnicpp::render::Allocation;
using omnicpp::render::BlasBuildInput;
using omnicpp::render::BottomLevelAS;
using omnicpp::render::PbrMaterialData;
using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
using omnicpp::render::ScenePbrObject;
using omnicpp::render::TlasInstance;
using omnicpp::render::TopLevelAS;
using omnicpp::render::VulkanAccelerationStructureBuilder;
using omnicpp::render::VulkanContext;
using omnicpp::render::VulkanDescriptorManager;
using omnicpp::render::VulkanMemoryAllocator;
using omnicpp::render::VulkanOffscreenTarget;
using omnicpp::render::VulkanPbrScene;
using omnicpp::render::VulkanPipeline;
using omnicpp::render::VulkanScratchPool;

constexpr std::uint32_t kImg = 256;

SceneMatrix scene_identity_matrix() {
  return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
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

//! Perspective with the engine's Vulkan y-flip in p[1][1] (see
//! test_rt_shadows.cpp — without it front faces are culled and interiors are
//! shaded).
SceneMatrix make_perspective(float fov_y_deg, float aspect, float zn,
                             float zf) {
  SceneMatrix m = scene_identity_matrix();
  const float f = 1.0f / std::tan(fov_y_deg * 3.14159265f / 360.0f);
  m[0] = f / aspect;
  m[5] = -f;
  m[10] = (zf + zn) / (zn - zf);
  m[11] = -1.0f;
  m[14] = (2.0f * zf * zn) / (zn - zf);
  m[15] = 0.0f;
  return m;
}

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

SceneMatrix make_translation(float x, float y, float z) {
  SceneMatrix m = scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

SceneMatrix make_scale(float x, float y, float z) {
  SceneMatrix m = scene_identity_matrix();
  m[0] = x;
  m[5] = y;
  m[10] = z;
  return m;
}

SceneMatrix mat4_multiply(const SceneMatrix& a, const SceneMatrix& b) {
  SceneMatrix out{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float acc = 0.0f;
      for (int k = 0; k < 4; ++k) {
        acc += a[static_cast<std::size_t>(k) * 4U +
                 static_cast<std::size_t>(row)] *
               b[static_cast<std::size_t>(col) * 4U +
                 static_cast<std::size_t>(k)];
      }
      out[static_cast<std::size_t>(col) * 4U + static_cast<std::size_t>(row)] =
          acc;
    }
  }
  return out;
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
    for (int k = 0; k < 3; ++k) {
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
//! color.rgb, normal.xyz, uv.xy). Half-extent 0.5. Scaled via model matrix.
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
  for (int f = 0; f < 6; ++f) {
    const std::uint32_t base = static_cast<std::uint32_t>(v.size() / 11U);
    for (int vert = 0; vert < 4; ++vert) {
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
  if (!ds.is_ok()) return {};
  if (!desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         out.va.buffer, 0, VK_WHOLE_SIZE).is_ok()) {
    return {};
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
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(L[static_cast<std::size_t>(k)]) < 1.0e-9f) {
      if (std::fabs(o[static_cast<std::size_t>(k)] -
                    c[static_cast<std::size_t>(k)]) >
          h[static_cast<std::size_t>(k)]) {
        return false;
      }
      continue;
    }
    const float inv = 1.0f / L[static_cast<std::size_t>(k)];
    float t0 = (c[static_cast<std::size_t>(k)] -
                h[static_cast<std::size_t>(k)] -
                o[static_cast<std::size_t>(k)]) * inv;
    float t1 = (c[static_cast<std::size_t>(k)] +
                h[static_cast<std::size_t>(k)] -
                o[static_cast<std::size_t>(k)]) * inv;
    if (t0 > t1) std::swap(t0, t1);
    tmin = std::max(tmin, t0);
    tmax = std::min(tmax, t1);
    if (tmin > tmax) return false;
  }
  return true;
}

struct ReflectionHarness {
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
  Allocation mat_buf_alloc{};
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
    auto mb = alloc.create_buffer(2 * sizeof(omnicpp::render::PbrMaterialData),
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
    return true;
  }

  void write_materials(const omnicpp::render::PbrMaterialData* m,
                       std::uint32_t n) {
    std::memcpy(mat_buf_alloc.mapped, m, n * sizeof(*m));
  }

  void cleanup() {
    target.cleanup(ctx.device());
    destroy_solid(ctx.device(), alloc, white);
    if (cube.va.is_valid()) alloc.destroy_allocation(cube.va);
    if (cube.ia.is_valid()) alloc.destroy_allocation(cube.ia);
    if (mat_buf_alloc.is_valid()) alloc.destroy_allocation(mat_buf_alloc);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

TEST(rt_reflections, floor_mirror_shows_ray_queried_cube) {
  ReflectionHarness h;
  ASSERT_TRUE(h.init("rt_reflections"));

  // ---- Scene constants (analytic ground truth above) -----------------------
  const std::array<float, 3> cube_center = {0.0f, 0.0f, -3.0f};
  const std::array<float, 3> cube_half = {0.5f, 0.5f, 0.5f};
  const std::array<float, 3> floor_center = {0.0f, -0.55f, -3.0f};
  const std::array<float, 3> floor_half = {3.0f, 0.05f, 3.0f};
  const std::array<float, 3> cam_eye = {0.0f, 1.2f, 2.2f};
  const std::array<float, 3> virtual_image = {0.0f, -1.0f, -3.0f};  // B'
  const std::array<float, 3> control_point = {1.2f, -0.5f, -1.0f};  // Q

  // CPU proof of the reflection claims (mirror plane y = -0.5 flips d_y):
  {
    // Camera ray toward B' hits the floor first (below the cube's bottom).
    std::array<float, 3> d = {virtual_image[0] - cam_eye[0],
                              virtual_image[1] - cam_eye[1],
                              virtual_image[2] - cam_eye[2]};
    const float t = (-0.5f - cam_eye[1]) / d[1];
    const std::array<float, 3> P = {cam_eye[0] + t * d[0],
                                    -0.5f,
                                    cam_eye[2] + t * d[2]};
    EXPECT_NEAR(P[0], 0.0f, 1.0e-3f);
    EXPECT_NEAR(P[2], -1.818f, 1.0e-2f);
    // Reflected up-ray travels toward the real cube center: must hit it.
    std::array<float, 3> r = {d[0], -d[1], d[2]};
    EXPECT_TRUE(segment_hits_box(P, r, cube_center, cube_half))
        << "analytic: mirror pixel must reflect into the cube";
    // Control: reflected up-ray at Q must miss everything.
    std::array<float, 3> dq = {control_point[0] - cam_eye[0],
                               control_point[1] - cam_eye[1],
                               control_point[2] - cam_eye[2]};
    std::array<float, 3> rq = {dq[0], -dq[1], dq[2]};
    EXPECT_FALSE(segment_hits_box({control_point[0], -0.5f, control_point[2]},
                                  rq, cube_center, cube_half))
        << "analytic: control floor point must reflect into nothing";
  }

  const SceneMatrix cam_view = make_look_at(cam_eye, {0.0f, -0.2f, -3.0f},
                                            {0, 1, 0});
  const SceneMatrix cam_vp =
      mat4_multiply(make_perspective(45.0f, 1.0f, 0.1f, 100.0f), cam_view);

  omnicpp::render::PbrMaterialData mats[2]{};  mats[0].base_color_factor = {0.75f, 0.75f, 0.75f, 1.0f};  // gray floor
  mats[0].roughness_factor = 0.2f;
  mats[0].metallic_factor = 0.0f;
  mats[1].base_color_factor = {0.9f, 0.1f, 0.1f, 1.0f};     // red cube
  mats[1].roughness_factor = 0.5f;
  mats[1].metallic_factor = 0.0f;
  h.write_materials(mats, 2);

  // ---- Acceleration structures --------------------------------------------
  const std::vector<float> slab = make_box_triangles(
      floor_half[0], floor_half[1], floor_half[2]);
  const std::vector<float> box = make_box_triangles(
      cube_half[0], cube_half[1], cube_half[2]);
  auto geom_floor = h.alloc.create_buffer(
      slab.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto geom_cube = h.alloc.create_buffer(
      box.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geom_floor.is_ok());
  ASSERT_TRUE(geom_cube.is_ok());
  std::memcpy(geom_floor.value().mapped, slab.data(),
              slab.size() * sizeof(float));
  std::memcpy(geom_cube.value().mapped, box.data(), box.size() * sizeof(float));

  auto address_of = [&](VkBuffer buffer) -> std::uint64_t {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return static_cast<std::uint64_t>(
        vkGetBufferDeviceAddress(h.ctx.device(), &info));
  };

  BlasBuildInput in_floor{};
  in_floor.vertex_buffer_address = address_of(geom_floor.value().buffer);
  in_floor.triangle_count = 12U;
  in_floor.max_vertex = 12U * 3U - 1U;
  ASSERT_NE(in_floor.vertex_buffer_address, 0U);
  BlasBuildInput in_cube{};
  in_cube.vertex_buffer_address = address_of(geom_cube.value().buffer);
  in_cube.triangle_count = 12U;
  in_cube.max_vertex = 12U * 3U - 1U;
  ASSERT_NE(in_cube.vertex_buffer_address, 0U);

  VulkanAccelerationStructureBuilder builder;
  auto blas_floor = builder.create_blas(h.ctx.device(), h.alloc, in_floor);
  auto blas_cube = builder.create_blas(h.ctx.device(), h.alloc, in_cube);
  ASSERT_TRUE(blas_floor.is_ok());
  ASSERT_TRUE(blas_cube.is_ok());
  auto tlas = builder.create_tlas(h.ctx.device(), h.alloc, 2U);
  ASSERT_TRUE(tlas.is_ok());

  std::array<TlasInstance, 2> instances{};
  {
    const std::array<float, 12> xf = translated_instance(
        floor_center[0], floor_center[1], floor_center[2]);
    std::memcpy(instances[0].transform, xf.data(), sizeof(xf));
  }
  instances[0].instance_custom_index = 1U;
  instances[0].blas_device_address = blas_floor.value().device_address;
  {
    const std::array<float, 12> xf = translated_instance(
        cube_center[0], cube_center[1], cube_center[2]);
    std::memcpy(instances[1].transform, xf.data(), sizeof(xf));
  }
  instances[1].instance_custom_index = 2U;
  instances[1].blas_device_address = blas_cube.value().device_address;

  const auto sizes_floor =
      VulkanAccelerationStructureBuilder::query_blas_sizes(h.ctx.device(),
                                                           in_floor);
  const auto sizes_cube =
      VulkanAccelerationStructureBuilder::query_blas_sizes(h.ctx.device(),
                                                           in_cube);
  const auto tlas_sizes =
      VulkanAccelerationStructureBuilder::query_tlas_sizes(h.ctx.device(), 2U);
  const std::uint64_t scratch_bytes =
      std::max({sizes_floor.buildScratchSize, sizes_cube.buildScratchSize,
                tlas_sizes.buildScratchSize});
  VulkanScratchPool scratch_pool;
  auto scratch_addr =
      scratch_pool.acquire(h.alloc, h.ctx.device(), scratch_bytes);
  ASSERT_TRUE(scratch_addr.is_ok());

  // Build: BLAS floor -> barrier -> BLAS cube -> barrier -> TLAS.
  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      h.ctx.device(), h.qf);
  ASSERT_TRUE(pool.is_ok());
  auto cb = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      h.ctx.device(), pool.value());
  ASSERT_TRUE(cb.is_ok());
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb.value(), &bi), VK_SUCCESS);
  ASSERT_TRUE(builder.cmd_build_blas(cb.value(), h.ctx.device(),
                                     blas_floor.value(), in_floor,
                                     scratch_addr.value()).is_ok());
  VkMemoryBarrier blas_barrier{};
  blas_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  blas_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  blas_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  vkCmdPipelineBarrier(cb.value(),
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       0, 1, &blas_barrier, 0, nullptr, 0, nullptr);
  ASSERT_TRUE(builder.cmd_build_blas(cb.value(), h.ctx.device(),
                                     blas_cube.value(), in_cube,
                                     scratch_addr.value()).is_ok());
  vkCmdPipelineBarrier(cb.value(),
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       0, 1, &blas_barrier, 0, nullptr, 0, nullptr);
  ASSERT_TRUE(builder.cmd_build_tlas(cb.value(), h.ctx.device(),
                                     tlas.value(), instances.data(), 2U,
                                     scratch_addr.value()).is_ok());
  ASSERT_EQ(vkEndCommandBuffer(cb.value()), VK_SUCCESS);
  ASSERT_TRUE(submit_and_wait(h.ctx.device(), h.ctx.graphics_queue(),
                              cb.value()));

  // ---- Pipelines ------------------------------------------------------------
  const std::string sd = WARPLOOM_TEST_SHADER_DIR;
  VulkanPipeline pipe;
  ASSERT_TRUE(pipe.load_shader_stage_file(h.ctx.device(),
      sd + "/pbr_scene.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(pipe.load_shader_stage_file(h.ctx.device(),
      sd + "/pbr_rt_reflect.frag.spv", "fragment").is_ok());

  // Set 3: AS (binding 0) + instance colors SSBO (binding 1).
  auto reflect_layout = h.desc.create_layout({
      {3, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {3, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}}, 1);
  ASSERT_TRUE(reflect_layout.is_ok());
  auto reflect_set = h.desc.allocate_set(reflect_layout.value());
  ASSERT_TRUE(reflect_set.is_ok());

  // Instance colors: [0] unused, [1] gray floor, [2] red cube.
  auto colors = h.alloc.create_buffer(
      3 * sizeof(float) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(colors.is_ok());
  {
    const std::array<float, 12> palette = {
        0.0f, 0.0f, 0.0f, 0.0f,
        0.75f, 0.75f, 0.75f, 1.0f,
        0.9f, 0.1f, 0.1f, 1.0f};
    std::memcpy(colors.value().mapped, palette.data(), sizeof(palette));
  }
  ASSERT_TRUE(h.desc.write_acceleration_structure(
      reflect_set.value(), 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
      tlas.value().handle, 0).is_ok());
  ASSERT_TRUE(h.desc.write_buffer(
      reflect_set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      colors.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  VkDescriptorSetLayout layouts[4] = {h.mesh_layout, h.tex_layout,
                                      h.mat_layout, reflect_layout.value()};
  VkPushConstantRange push{
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
  ASSERT_TRUE(pipe.create_pipeline_layout(h.ctx.device(), layouts, 4,
                                          &push).is_ok());
  ASSERT_TRUE(pipe.create_graphics_pipeline(h.ctx.device(),
      h.target.render_pass(), h.target.format(), pipe.pipeline_layout(),
      true, true, true).is_ok());

  // ---- Render ---------------------------------------------------------------
  omnicpp::render::VulkanPbrScene scene{};  scene.pipeline = pipe.pipeline();
  scene.pipeline_layout = pipe.pipeline_layout();
  scene.camera.view_projection = cam_vp;
  scene.camera_position = {cam_eye[0], cam_eye[1], cam_eye[2], 1.0f};
  scene.texture_set = h.tex_set;
  scene.material_set = h.mat_set;
  scene.shadow_set = reflect_set.value();
  scene.shadow_set_slot = 3U;

  ScenePbrObject floor_obj{};
  floor_obj.mesh = &h.cube.mesh;  // scaled to the slab by the model matrix
  floor_obj.model = mat4_multiply(
      make_translation(floor_center[0], floor_center[1], floor_center[2]),
      make_scale(floor_half[0] * 2.0f, floor_half[1] * 2.0f,
                 floor_half[2] * 2.0f));
  floor_obj.material_index = 0;
  ScenePbrObject cube_obj{};
  cube_obj.mesh = &h.cube.mesh;
  cube_obj.model = make_translation(cube_center[0], cube_center[1],
                                    cube_center[2]);
  cube_obj.material_index = 1;
  scene.objects = {floor_obj, cube_obj};

  auto pr = omnicpp::render::VulkanRenderer::create_command_pool(
      h.ctx.device(), h.qf);
  ASSERT_TRUE(pr.is_ok());
  auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      h.ctx.device(), pr.value());
  ASSERT_TRUE(cr.is_ok());
  VkCommandBuffer rcb = cr.value();
  VkCommandBufferBeginInfo rbi{};
  rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(rcb, &rbi);

  omnicpp::render::VulkanRenderer renderer;
  omnicpp::render::VulkanRenderer::PbrFrameTargets targets{};
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
  ASSERT_TRUE(frame_r.is_ok());
  vkEndCommandBuffer(rcb);
  ASSERT_TRUE(submit_and_wait(h.ctx.device(), h.ctx.graphics_queue(), rcb));
  vkDestroyCommandPool(h.ctx.device(), pr.value(), nullptr);

  const auto rb = omnicpp_test::readback_swapchain_image(
      h.ctx.physical_device(), h.ctx.device(), h.ctx.graphics_queue(), h.qf,
      h.target.image(), h.target.format(), kImg, kImg,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*store_pixels=*/true);
  ASSERT_EQ(rb.pixels.size(), static_cast<std::size_t>(kImg) * kImg);
  EXPECT_GT(rb.non_clear_pixels, 800U) << "scene rendered nothing";

  // ---- Probes ----------------------------------------------------------------
  auto project = [&](const std::array<float, 3>& p) -> std::pair<int, int> {
    // cam_vp * p -> NDC. The projection already carries the Vulkan y-flip
    // (m[5] = -f), and framebuffer row 0 is the top (NDC y = -1), so the
    // pixel row maps directly: py = (y_ndc * 0.5 + 0.5) * H. No extra flip.
    const auto& m = cam_vp;
    const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    const float x = cx / cw;
    const float y = cy / cw;
    const int px = static_cast<int>((x * 0.5f + 0.5f) * float(kImg));
    const int py = static_cast<int>((y * 0.5f + 0.5f) * float(kImg));
    return {px, py};
  };
  auto channels = [&](const omnicpp_test::ReadbackResult& r, int px, int py) {
    const std::uint32_t packed =
        r.pixels[static_cast<std::size_t>(py) * kImg + px];
    return std::array<float, 3>{static_cast<float>(packed & 0xffU) / 255.0f,
                                static_cast<float>((packed >> 8) & 0xffU) /
                                    255.0f,
                                static_cast<float>((packed >> 16) & 0xffU) /
                                    255.0f};
  };

  const auto [mx, my] = project(virtual_image);
  ASSERT_GE(mx, 1); ASSERT_LT(mx, static_cast<int>(kImg) - 1);
  ASSERT_GE(my, 1); ASSERT_LT(my, static_cast<int>(kImg) - 1);
  const auto [qx, qy] = project(control_point);
  ASSERT_GE(qx, 1); ASSERT_LT(qx, static_cast<int>(kImg) - 1);
  ASSERT_GE(qy, 1); ASSERT_LT(qy, static_cast<int>(kImg) - 1);
  const auto [cx2, cy2] = project(cube_center);

  const auto mirror = channels(rb, mx, my);
  const auto control = channels(rb, qx, qy);
  const auto direct = channels(rb, cx2, cy2);

  if (std::getenv("WARPLOOM_RT_REFLECT_DEBUG") != nullptr) {
    // Empirical: reddest pixel in the frame + probe values.
    int best_x = -1, best_y = -1;
    float best_gap = -1.0f;
    for (int py = 0; py < static_cast<int>(kImg); ++py) {
      for (int px = 0; px < static_cast<int>(kImg); ++px) {
        const auto c = channels(rb, px, py);
        const float gap = c[0] - std::max(c[1], c[2]);
        if (gap > best_gap) {
          best_gap = gap;
          best_x = px;
          best_y = py;
        }
      }
    }
    std::printf("reddest pixel: (%d,%d) gap=%.3f  predicted cube center (%d,%d)"
                " mirror (%d,%d) control (%d,%d)\n",
                best_x, best_y, best_gap, cx2, cy2, mx, my, qx, qy);
    const auto bc = channels(rb, best_x, best_y);
    std::printf("reddest rgb=(%.3f,%.3f,%.3f)  direct=(%.3f,%.3f,%.3f)"
                "  mirror=(%.3f,%.3f,%.3f)  control=(%.3f,%.3f,%.3f)\n",
                bc[0], bc[1], bc[2], direct[0], direct[1], direct[2],
                mirror[0], mirror[1], mirror[2], control[0], control[1],
                control[2]);
    // 16px-cell classification map: R = red-dominant (cube/reflection),
    // f = bright gray (floor), . = dark/miss, ' ' = clear.
    for (int cy = 0; cy < static_cast<int>(kImg) / 16; ++cy) {
      std::string row;
      for (int cx = 0; cx < static_cast<int>(kImg) / 16; ++cx) {
        const auto c = channels(rb, cx * 16 + 8, cy * 16 + 8);
        const float gap = c[0] - std::max(c[1], c[2]);
        const float lum = 0.3f * c[0] + 0.6f * c[1] + 0.1f * c[2];
        if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f) {
          row += ' ';
        } else if (gap > 0.08f) {
          row += 'R';
        } else if (lum > 0.35f) {
          row += 'f';
        } else {
          row += '.';
        }
      }
      std::printf("%3d|%s|\n", cy * 16, row.c_str());
    }
    std::fflush(stdout);
  }

  // Direct view of the cube: red-dominant (material path sanity).
  EXPECT_GT(direct[0], direct[1] + 0.10f)
      << "direct cube view not red: r=" << direct[0] << " g=" << direct[1];
  // Mirror pixel: reflection of the red cube.
  EXPECT_GT(mirror[0], mirror[1] + 0.06f)
      << "mirror pixel not red: r=" << mirror[0] << " g=" << mirror[1]
      << " b=" << mirror[2];
  EXPECT_GT(mirror[0], mirror[2] + 0.06f) << "mirror pixel not red (b)";
  // Control floor pixel: neutral (miss -> dim sky tint only).
  EXPECT_LT(std::fabs(control[0] - control[1]), 0.05f)
      << "control floor pixel tinted: r=" << control[0]
      << " g=" << control[1] << " b=" << control[2];
  EXPECT_LT(std::fabs(control[0] - control[2]), 0.05f)
      << "control floor pixel tinted (b)";

  // Cleanup (move-then-destroy, per the builder's non-copyable handles).
  BottomLevelAS out_floor = std::move(blas_floor.value());
  BottomLevelAS out_cube = std::move(blas_cube.value());
  TopLevelAS out_t = std::move(tlas.value());
  Allocation out_gf = std::move(geom_floor.value());
  Allocation out_gc = std::move(geom_cube.value());
  builder.destroy_blas(h.ctx.device(), h.alloc, out_floor);
  builder.destroy_blas(h.ctx.device(), h.alloc, out_cube);
  builder.destroy_tlas(h.ctx.device(), h.alloc, out_t);
  h.alloc.destroy_allocation(out_gf);
  h.alloc.destroy_allocation(out_gc);
  scratch_pool.cleanup(h.alloc);
  Allocation out_colors = std::move(colors.value());
  h.alloc.destroy_allocation(out_colors);
  pipe.cleanup(h.ctx.device());
  vkDestroyCommandPool(h.ctx.device(), pool.value(), nullptr);
  h.cleanup();
}

}  // namespace
