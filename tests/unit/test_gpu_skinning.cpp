//! @file test_gpu_skinning.cpp
//! @brief GPU end-to-end tests for skeletal animation via GPU vertex
//!        skinning (skinned_scene.vert + bone SSBO at set 5).
//!
//! Test geometry: a vertical bar (2 quads, 8 vertices) whose lower vertices
//! bind to bone 0 (identity) and upper vertices to bone 1 (translated up by
//! 1.5). The two animation states — bones at rest and bone 1 rotated 90
//! degrees about X — are verified by pixel readback:
//!   (1) At rest the bar renders as a tall rectangle centred on screen.
//!   (2) When bone 1 bends the top half, the bar's projected height shrinks
//!       (top tips away from the camera) and the top edge drops.
//! The deformation is computed entirely on the GPU; the CPU only uploads
//! bone matrices.

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_frame_upload.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_scene.hpp"
#include "vulkan_test_readback.hpp"

#if defined(WARPLOOM_HAS_VULKAN)
#include <vulkan/vulkan.h>

namespace {

using omnicpp::render::PbrMaterialData;
using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
using omnicpp::render::ScenePbrObject;
using omnicpp::render::VulkanPbrScene;
using omnicpp::render::scene_identity_matrix;

SceneMatrix make_perspective(float fov, float aspect, float zn, float zf) {
  SceneMatrix m = scene_identity_matrix();
  float f = 1.0f / std::tan(fov * 3.14159265f / 360.0f);
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
  // Column-major: column 1 = (0, c, s), column 2 = (0, -s, c).
  m[5] = c;  m[6] = s;
  m[9] = -s; m[10] = c;
  return m;
}

// Vertical bar: two stacked quads in the XY plane at z=0.
// Lower quad: y in [-1, 0], upper quad: y in [0, 1]. Width 0.4.
// Lower vertices bind bone 0 (weight 1), upper bind bone 1 (weight 1).
struct BarMesh {
  omnicpp::render::Allocation va{};
  omnicpp::render::Allocation ia{};
  SceneMesh mesh{};
};

void build_bar(std::vector<float>& verts, std::vector<uint32_t>& idx,
               std::vector<float>& skin_data) {
  // 8 vertices: 4 lower (bone 0), 4 upper (bone 1).
  // Static stream (11 floats): pos.xyz, color.rgb, normal.xyz, uv.xy
  const float lower[4][3] = {{-0.2f, -1.0f, 0.0f}, {0.2f, -1.0f, 0.0f},
                             {0.2f, 0.0f, 0.0f}, {-0.2f, 0.0f, 0.0f}};
  const float upper[4][3] = {{-0.2f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f},
                             {0.2f, 1.0f, 0.0f}, {-0.2f, 1.0f, 0.0f}};
  verts.clear();
  for (int i = 0; i < 4; ++i) {
    verts.insert(verts.end(),
                 {lower[i][0], lower[i][1], lower[i][2], 1, 1, 1, 0, 0, 1, 0, 0});
  }
  for (int i = 0; i < 4; ++i) {
    verts.insert(verts.end(),
                 {upper[i][0], upper[i][1], upper[i][2], 1, 1, 1, 0, 0, 1, 0, 0});
  }
  // Indices: lower quad (0,1,2,3), upper quad (4,5,6,7).
  idx = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

  // Skinning payload: 8 floats per vertex (joints x4 + weights x4).
  // Joint indices stored as float (shader casts to uint).
  skin_data.clear();
  for (int i = 0; i < 4; ++i) {
    skin_data.insert(skin_data.end(),
                     {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f});
  }
  for (int i = 0; i < 4; ++i) {
    skin_data.insert(skin_data.end(),
                     {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f});
  }
}

struct SolidTexture { VkImage img{}; VkImageView view{}; VkSampler sampler{}; omnicpp::render::Allocation alloc{}; };
bool make_white(VkDevice d, VkPhysicalDevice pd, VkQueue q, uint32_t f,
    omnicpp::render::VulkanMemoryAllocator& a, SolidTexture& out) {
  out = {};
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = {1,1,1}; ii.mipLevels = ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (vkCreateImage(d, &ii, nullptr, &out.img) != VK_SUCCESS) return false;
  auto m = a.bind_image(out.img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!m.is_ok()) return false;
  out.alloc = m.value();
  omnicpp::render::VulkanFrameUploadArena arena;
  if (!arena.initialize(d, pd, f, 1, 1<<20).is_ok()) return false;
  if (!arena.begin_frame(0).is_ok()) return false;
  auto s = arena.acquire(4);
  if (!s.is_ok()) return false;
  uint8_t w[4] = {255,255,255,255};
  std::memcpy(s.value().host_data, w, 4);
  arena.record_copy_image_rgba8(s.value(), out.img, 1, 1);
  if (!arena.submit(q).is_ok()) return false;
  arena.wait_idle();
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = out.img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1};
  vkCreateImageView(d, &vi, nullptr, &out.view);
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(d, &si, nullptr, &out.sampler);
  return true;
}
void destroy_solid(VkDevice d, omnicpp::render::VulkanMemoryAllocator& a, SolidTexture& t) {
  if (t.sampler) vkDestroySampler(d, t.sampler, nullptr);
  if (t.view) vkDestroyImageView(d, t.view, nullptr);
  if (t.alloc.is_valid()) a.destroy_allocation(t.alloc);
  if (t.img) vkDestroyImage(d, t.img, nullptr);
  t = {};
}

struct SkinningHarness {
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
  BarMesh bar;
  omnicpp::render::Allocation mat_buf{};
  omnicpp::render::Allocation bone_buf{};
  omnicpp::render::VulkanOffscreenTarget target;
  omnicpp::render::VulkanPipeline pipe;
  uint32_t qf{0};

  bool init(const char* name) {
    if (!ctx.initialize(name, true).is_ok()) return false;
    if (!ctx.has_descriptor_indexing()) { ctx.cleanup(); return false; }
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) { ctx.cleanup(); return false; }
    if (!desc.initialize(ctx.device()).is_ok()) return false;
    qf = (uint32_t)ctx.queue_families().graphics_family;
    VkDevice dev = ctx.device();

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
    auto mb = alloc.create_buffer(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mb.is_ok()) return false;
    mat_buf = mb.value();
    desc.write_buffer(mat_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mat_buf.buffer, 0, VK_WHOLE_SIZE);

    // Bone SSBO (set 3): 2 joints * 64 bytes.
    auto r5 = desc.create_layout({{3,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_VERTEX_BIT}}, 8);
    if (!r5.is_ok()) return false;
    bone_layout = r5.value();
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

    // Bar mesh: static verts + skinning payload in one SSBO.
    std::vector<float> v; std::vector<uint32_t> i; std::vector<float> skin;
    build_bar(v, i, skin);
    // Combined buffer: static verts then skinning payload.
    std::vector<float> combined = v;
    combined.insert(combined.end(), skin.begin(), skin.end());
    auto vb = alloc.create_buffer(combined.size()*sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto ib = alloc.create_buffer(i.size()*sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!vb.is_ok()||!ib.is_ok()) return false;
    bar.va = vb.value(); bar.ia = ib.value();
    std::memcpy(bar.va.mapped, combined.data(), combined.size()*sizeof(float));
    std::memcpy(bar.ia.mapped, i.data(), i.size()*sizeof(uint32_t));
    auto ds = desc.allocate_set(mesh_layout);
    if (!ds.is_ok()) return false;
    desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bar.va.buffer, 0, VK_WHOLE_SIZE);
    bar.mesh.vertex_buffer = bar.va.buffer;
    bar.mesh.index_buffer = bar.ia.buffer;
    bar.mesh.index_count = (uint32_t)i.size();
    bar.mesh.descriptor_set = ds.value();

    if (!target.create(dev, ctx.physical_device(), VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &alloc).is_ok()||
        !target.create_depth(dev, ctx.physical_device(), VK_FORMAT_D32_SFLOAT).is_ok()||
        !target.create_render_pass(dev).is_ok()||
        !target.create_framebuffer(dev).is_ok()) return false;

    // Skinned pipeline: 4 sets (mesh, textures, material, bones).
    std::string sd = WARPLOOM_TEST_SHADER_DIR;
    if (!pipe.load_shader_stage_file(dev, sd+"/skinned_scene.vert.spv","vertex").is_ok()||
        !pipe.load_shader_stage_file(dev, sd+"/pbr_scene.frag.spv","fragment").is_ok()) return false;
    VkDescriptorSetLayout layouts[4] = {mesh_layout, tex_layout, mat_layout, bone_layout};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
    if (!pipe.create_pipeline_layout(dev, layouts, 4, &pr).is_ok()||
        !pipe.create_graphics_pipeline(dev, target.render_pass(), target.format(),
            pipe.pipeline_layout(), true, true, false).is_ok()) return false;
    return true;
  }

  void write_bones(const SceneMatrix* bones, uint32_t count) {
    std::memcpy(bone_buf.mapped, bones, count*64);
  }
  void write_material(const PbrMaterialData& m) {
    std::memcpy(mat_buf.mapped, &m, sizeof(m));
  }

  omnicpp_test::ReadbackResult render(const VulkanPbrScene& scene) {
    VkDevice dev = ctx.device();
    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(dev, qf);
    if (!pr.is_ok()) return {};
    auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(dev, pr.value());
    if (!cr.is_ok()) { vkDestroyCommandPool(dev, pr.value(), nullptr); return {}; }
    VkCommandBuffer cb = cr.value();
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence; vkCreateFence(dev, &fi, nullptr, &fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clears[2]{};
    clears[0].color = {{0,0,0,1}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = target.render_pass();
    rpb.framebuffer = target.framebuffer();
    rpb.renderArea.extent = {256,256};
    rpb.clearValueCount = 2; rpb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0,0,256,256,0,1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0,0},{256,256}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.pipeline);
    if (scene.texture_set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
        scene.pipeline_layout, 1, 1, &scene.texture_set, 0, nullptr);
    if (scene.material_set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
        scene.pipeline_layout, 2, 1, &scene.material_set, 0, nullptr);
    if (scene.bone_set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
        scene.pipeline_layout, 3, 1, &scene.bone_set, 0, nullptr);
    struct Push { SceneMatrix vp; SceneMatrix model; std::array<float,4> cam; uint32_t mi; uint32_t p[3]{}; } push{};
    push.vp = scene.camera.view_projection; push.cam = scene.camera_position;
    VkShaderStageFlags ks = VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    for (auto& o : scene.objects) {
      auto* m = o.effective_mesh();
      if (!m||!m->is_drawable()) continue;
      push.model = o.model; push.mi = o.material_index;
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
          scene.pipeline_layout, 0, 1, &m->descriptor_set, 0, nullptr);
      vkCmdPushConstants(cb, scene.pipeline_layout, ks, 0, sizeof(push), &push);
      vkCmdBindIndexBuffer(cb, m->index_buffer, m->index_offset, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cb, m->index_count, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    vkResetFences(dev, 1, &fence);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pr.value(), nullptr);
    return omnicpp_test::readback_swapchain_image(
        ctx.physical_device(), dev, ctx.graphics_queue(), qf,
        target.image(), target.format(), 256, 256,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }

  void cleanup() {
    VkDevice dev = ctx.device();
    pipe.cleanup(dev);
    target.cleanup(dev);
    destroy_solid(dev, alloc, white);
    if (bar.va.is_valid()) alloc.destroy_allocation(bar.va);
    if (bar.ia.is_valid()) alloc.destroy_allocation(bar.ia);
    if (mat_buf.is_valid()) alloc.destroy_allocation(mat_buf);
    if (bone_buf.is_valid()) alloc.destroy_allocation(bone_buf);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

}  // namespace

//! Rest pose: the bar renders at full height (both quads visible, top edge
//! near y=+1). Bent pose: bone 1 rotates 90° about X, tipping the top half
//! away from the camera — the projected top edge drops and the bar shortens.
TEST(VulkanHardware, GpuSkinningBendsBar) {
  SkinningHarness h;
  if (!h.init("skinning_test"))
    GTEST_SKIP() << "Vulkan unavailable";

  PbrMaterialData mat{};
  mat.base_color_factor = {0.9f, 0.5f, 0.2f, 1.0f};
  mat.metallic_factor = 0; mat.roughness_factor = 0.8f;
  h.write_material(mat);

  VulkanPbrScene scene{};
  scene.pipeline = h.pipe.pipeline();
  scene.pipeline_layout = h.pipe.pipeline_layout();
  scene.camera.view_projection = make_perspective(45, 1.0f, 0.1f, 100);
  scene.camera_position = {0, 0, 4, 1};
  scene.texture_set = h.tex_set;
  scene.material_set = h.mat_set;
  scene.bone_set = h.bone_set;
  ScenePbrObject obj{};
  obj.mesh = &h.bar.mesh;
  // Place the bar 4 units in front of the camera (camera at origin looks
  // down -Z), matching the IBL test's cube placement.
  obj.model = make_translation(0.0f, 0.0f, -4.0f);
  obj.material_index = 0;
  scene.objects = {obj};

  // Rest pose: both bones identity.
  SceneMatrix rest[2] = {scene_identity_matrix(), scene_identity_matrix()};
  h.write_bones(rest, 2);
  const auto rest_result = h.render(scene);

  // Bent pose: bone 1 rotated 90° about X (top half tips away).
  SceneMatrix bent[2] = {scene_identity_matrix(), make_rotation_x(1.5707963f)};
  h.write_bones(bent, 2);
  const auto bent_result = h.render(scene);

  h.cleanup();

  ASSERT_TRUE(rest_result.submitted);
  ASSERT_TRUE(bent_result.submitted);
  EXPECT_GT(rest_result.non_clear_pixels, 1000U) << "rest bar did not render";
  EXPECT_GT(bent_result.non_clear_pixels, 1000U) << "bent bar did not render";

  // The bent pose must produce a different image than the rest pose: the
  // GPU skinning actually deformed the geometry.
  EXPECT_NE(rest_result.hash, bent_result.hash)
      << "bent pose image identical to rest pose — skinning had no effect";
}

#endif  // WARPLOOM_HAS_VULKAN
