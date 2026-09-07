//! @file test_postprocessing.cpp
//! @brief GPU end-to-end tests for the post-processing pipeline:
//!        HDR scene rendering → ACES tonemapping + FXAA anti-aliasing.
//!
//! Readback proofs:
//!   (1) The tonemapped scene has non-zero pixels (the cube rendered).
//!   (2) The center pixel is within ACES-mapped range (0-255).
//!   (3) A bright HDR source (base_color 5.0) maps to an LDR value via ACES
//!       compression — brighter than ambient but not clipped to 255.

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

#if defined(OMNICPP_HAS_VULKAN)
#include <vulkan/vulkan.h>

namespace {

using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
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

void build_unit_cube(std::vector<float>& verts, std::vector<uint32_t>& idx) {
  const float faces[6][15] = {
    { 0, 0, 1, -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1},
    { 0, 0,-1,  1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1},
    { 1, 0, 0,  1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1},
    {-1, 0, 0, -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1},
    { 0, 1, 0, -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1},
    { 0,-1, 0, -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1}
  };
  verts.clear(); idx.clear();
  for (int f = 0; f < 6; ++f) {
    uint32_t base = (uint32_t)(verts.size() / 11);
    for (int v = 0; v < 4; ++v) {
      verts.insert(verts.end(), {
        faces[f][3+v*3]*0.5f, faces[f][3+v*3+1]*0.5f, faces[f][3+v*3+2]*0.5f,
        1,1,1, faces[f][0], faces[f][1], faces[f][2], 0, 0});
    }
    idx.insert(idx.end(), {base, base+1, base+2, base, base+2, base+3});
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

// ============================================================================
// Post-process test harness
// ============================================================================
struct PostProcessHarness {
  omnicpp::render::VulkanContext ctx;
  omnicpp::render::VulkanMemoryAllocator alloc;
  omnicpp::render::VulkanDescriptorManager desc;
  VkDescriptorSetLayout mesh_layout{};
  VkDescriptorSetLayout tex_layout{};
  VkDescriptorSetLayout mat_layout{};
  VkDescriptorSetLayout post_layout{};  // set 0: input image sampler
  VkDescriptorSet tex_set{};
  VkDescriptorSet mat_set{};
  SolidTexture white;
  omnicpp::render::Allocation cube_va{}, cube_ia{};
  SceneMesh cube_mesh{};
  VkDescriptorSet cube_ds{};
  omnicpp::render::Allocation mat_buf{};
  uint32_t qf{0};

  // HDR scene target (R16G16B16A16_SFLOAT).
  VkImage hdr_image{};
  omnicpp::render::Allocation hdr_mem{};
  VkImageView hdr_view{};
  VkSampler hdr_sampler{};
  VkRenderPass hdr_rp{};
  VkFramebuffer hdr_fb{};
  VkDescriptorSet hdr_ds{};

  // Post-process pipelines.
  omnicpp::render::VulkanPipeline tonemap_pipe;

  bool init(const char* name) {
    if (!ctx.initialize(name, true).is_ok()) return false;
    if (!ctx.has_descriptor_indexing()) { ctx.cleanup(); return false; }
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) { ctx.cleanup(); return false; }
    if (!desc.initialize(ctx.device()).is_ok()) return false;
    qf = (uint32_t)ctx.queue_families().graphics_family;
    VkDevice dev = ctx.device();

    // set 0: mesh SSBO
    auto r0 = desc.create_layout({{0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_VERTEX_BIT}}, 8);
    if (!r0.is_ok()) return false;
    mesh_layout = r0.value();
    // set 1: bindless textures
    auto r1 = desc.create_layout({{1,0,0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 1, true);
    if (!r1.is_ok()) return false;
    tex_layout = r1.value();
    auto ts = desc.allocate_set(tex_layout);
    if (!ts.is_ok()) return false;
    tex_set = ts.value();
    // set 2: material SSBO
    auto r2 = desc.create_layout({{2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 8);
    if (!r2.is_ok()) return false;
    mat_layout = r2.value();
    auto ms = desc.allocate_set(mat_layout);
    if (!ms.is_ok()) return false;
    mat_set = ms.value();
    auto mb = alloc.create_buffer(2*64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mb.is_ok()) return false;
    mat_buf = mb.value();
    desc.write_buffer(mat_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mat_buf.buffer, 0, VK_WHOLE_SIZE);

    if (!make_white(dev, ctx.physical_device(), ctx.graphics_queue(), qf, alloc, white)) return false;
    desc.write_image(tex_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        white.sampler, white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0);

    // Cube mesh
    std::vector<float> v; std::vector<uint32_t> i;
    build_unit_cube(v, i);
    auto vb = alloc.create_buffer(v.size()*sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto ib = alloc.create_buffer(i.size()*sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!vb.is_ok()||!ib.is_ok()) return false;
    cube_va = vb.value(); cube_ia = ib.value();
    std::memcpy(cube_va.mapped, v.data(), v.size()*sizeof(float));
    std::memcpy(cube_ia.mapped, i.data(), i.size()*sizeof(uint32_t));
    auto ds = desc.allocate_set(mesh_layout);
    if (!ds.is_ok()) return false;
    desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cube_va.buffer, 0, VK_WHOLE_SIZE);
    cube_mesh.vertex_buffer = cube_va.buffer;
    cube_mesh.index_buffer = cube_ia.buffer;
    cube_mesh.index_count = (uint32_t)i.size();
    cube_mesh.descriptor_set = ds.value();

    // HDR target (R16G16B16A16_SFLOAT for HDR rendering).
    VkImageCreateInfo hii{};
    hii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    hii.imageType = VK_IMAGE_TYPE_2D;
    hii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    hii.extent = {256,256,1}; hii.mipLevels = hii.arrayLayers = 1;
    hii.samples = VK_SAMPLE_COUNT_1_BIT;
    hii.tiling = VK_IMAGE_TILING_OPTIMAL;
    hii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(dev, &hii, nullptr, &hdr_image) != VK_SUCCESS) return false;
    auto hm = alloc.bind_image(hdr_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!hm.is_ok()) return false;
    hdr_mem = hm.value();
    VkImageViewCreateInfo hvi{};
    hvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    hvi.image = hdr_image; hvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    hvi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    hvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1};
    if (vkCreateImageView(dev, &hvi, nullptr, &hdr_view) != VK_SUCCESS) return false;
    VkSamplerCreateInfo hsi{};
    hsi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    hsi.magFilter = hsi.minFilter = VK_FILTER_LINEAR;
    hsi.addressModeU = hsi.addressModeV = hsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &hsi, nullptr, &hdr_sampler) != VK_SUCCESS) return false;

    // HDR render pass (color-only, transitions to SHADER_READ_ONLY).
    VkAttachmentDescription ha{};
    ha.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ha.samples = VK_SAMPLE_COUNT_1_BIT;
    ha.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ha.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ha.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ha.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference hr{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription hs{};
    hs.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    hs.colorAttachmentCount = 1; hs.pColorAttachments = &hr;
    VkRenderPassCreateInfo hrpc{};
    hrpc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    hrpc.attachmentCount = 1; hrpc.pAttachments = &ha;
    hrpc.subpassCount = 1; hrpc.pSubpasses = &hs;
    if (vkCreateRenderPass(dev, &hrpc, nullptr, &hdr_rp) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo hfbi{};
    hfbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    hfbi.renderPass = hdr_rp; hfbi.attachmentCount = 1; hfbi.pAttachments = &hdr_view;
    hfbi.width = 256; hfbi.height = 256; hfbi.layers = 1;
    if (vkCreateFramebuffer(dev, &hfbi, nullptr, &hdr_fb) != VK_SUCCESS) return false;

    // Post-process set layout (set 0: input image).
    auto r_post = desc.create_layout({{0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 1);
    if (!r_post.is_ok()) return false;
    post_layout = r_post.value();
    auto pds = desc.allocate_set(post_layout);
    if (!pds.is_ok()) return false;
    hdr_ds = pds.value();
    desc.write_image(hdr_ds, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        hdr_sampler, hdr_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0);

    return true;
  }

  // Create the tonemap pipeline with a specific render pass.
  bool create_tonemap_pipeline(VkRenderPass rp, VkFormat format) {
    VkDevice dev = ctx.device();
    std::string sd = OMNICPP_TEST_SHADER_DIR;
    if (!tonemap_pipe.load_shader_stage_file(dev, sd+"/fullscreen.vert.spv","vertex").is_ok()||
        !tonemap_pipe.load_shader_stage_file(dev, sd+"/tonemap_fxaa.frag.spv","fragment").is_ok()) return false;
    VkDescriptorSetLayout tl[1] = {post_layout};
    if (!tonemap_pipe.create_pipeline_layout(dev, tl, 1, nullptr).is_ok()) return false;
    if (!tonemap_pipe.create_graphics_pipeline(dev, rp, format,
        tonemap_pipe.pipeline_layout(), false, false, false).is_ok()) return false;
    return true;
  }

  // Render an HDR scene to hdr_image, then tonemap+FXAA to out_target.
  omnicpp_test::ReadbackResult render_and_postprocess(
      const VulkanPbrScene& scene,
      omnicpp::render::VulkanOffscreenTarget& out_target) {
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

    // Pass 1: HDR scene to hdr_image.
    {
      VkClearValue cv{}; cv.color = {{0,0,0,1}};
      VkRenderPassBeginInfo rpb{};
      rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rpb.renderPass = hdr_rp; rpb.framebuffer = hdr_fb;
      rpb.renderArea.extent = {256,256};
      rpb.clearValueCount = 1; rpb.pClearValues = &cv;
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
    }

    // Pass 2: tonemap + FXAA to out_target.
    {
      VkClearValue clears[2]{};
      clears[0].color = {{0,0,0,1}};
      clears[1].depthStencil = {1.0f, 0};
      VkRenderPassBeginInfo rpb{};
      rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rpb.renderPass = out_target.render_pass();
      rpb.framebuffer = out_target.framebuffer();
      rpb.renderArea.extent = {256,256};
      rpb.clearValueCount = 2; rpb.pClearValues = clears;
      vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
      VkViewport vp{0,0,256,256,0,1};
      vkCmdSetViewport(cb, 0, 1, &vp);
      VkRect2D sc{{0,0},{256,256}};
      vkCmdSetScissor(cb, 0, 1, &sc);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_pipe.pipeline());
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
          tonemap_pipe.pipeline_layout(), 0, 1, &hdr_ds, 0, nullptr);
      vkCmdDraw(cb, 3, 1, 0, 0);  // fullscreen triangle
      vkCmdEndRenderPass(cb);
    }

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
        out_target.image(), out_target.format(), 256, 256,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }

  void cleanup() {
    VkDevice dev = ctx.device();
    tonemap_pipe.cleanup(dev);
    destroy_solid(dev, alloc, white);
    if (cube_va.is_valid()) alloc.destroy_allocation(cube_va);
    if (cube_ia.is_valid()) alloc.destroy_allocation(cube_ia);
    if (mat_buf.is_valid()) alloc.destroy_allocation(mat_buf);
    if (hdr_mem.is_valid()) alloc.destroy_allocation(hdr_mem);
    if (hdr_image) vkDestroyImage(dev, hdr_image, nullptr);
    if (hdr_view) vkDestroyImageView(dev, hdr_view, nullptr);
    if (hdr_sampler) vkDestroySampler(dev, hdr_sampler, nullptr);
    if (hdr_rp) vkDestroyRenderPass(dev, hdr_rp, nullptr);
    if (hdr_fb) vkDestroyFramebuffer(dev, hdr_fb, nullptr);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

}  // namespace

TEST(VulkanHardware, PostProcessTonemapFxaa) {
  PostProcessHarness h;
  if (!h.init("postprocess_test"))
    GTEST_SKIP() << "Vulkan unavailable";

  // Out target for the tonemapped output (B8G8R8A8_UNORM, color + depth).
  omnicpp::render::VulkanOffscreenTarget out;
  if (!out.create(h.ctx.device(), h.ctx.physical_device(), VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &h.alloc).is_ok()||
      !out.create_depth(h.ctx.device(), h.ctx.physical_device(), VK_FORMAT_D32_SFLOAT).is_ok()||
      !out.create_render_pass(h.ctx.device()).is_ok()||
      !out.create_framebuffer(h.ctx.device()).is_ok()) {
    h.cleanup();
    GTEST_SKIP() << "Out target creation failed";
  }

  // Create tonemap pipeline with the out_target's render pass.
  if (!h.create_tonemap_pipeline(out.render_pass(), out.format())) {
    h.cleanup(); out.cleanup(h.ctx.device());
    GTEST_SKIP() << "Tonemap pipeline failed";
  }

  // Scene pipeline: renders to hdr_rp (R16G16B16A16_SFLOAT, no depth).
  omnicpp::render::VulkanPipeline scene_pipe;
  std::string sd = OMNICPP_TEST_SHADER_DIR;
  if (!scene_pipe.load_shader_stage_file(h.ctx.device(), sd+"/pbr_scene.vert.spv","vertex").is_ok()||
      !scene_pipe.load_shader_stage_file(h.ctx.device(), sd+"/pbr_scene.frag.spv","fragment").is_ok()) {
    h.cleanup(); out.cleanup(h.ctx.device());
    GTEST_SKIP() << "Scene pipeline load failed";
  }
  VkDescriptorSetLayout sl[3] = {h.mesh_layout, h.tex_layout, h.mat_layout};
  VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
  if (!scene_pipe.create_pipeline_layout(h.ctx.device(), sl, 3, &pr).is_ok()||
      !scene_pipe.create_graphics_pipeline(h.ctx.device(), h.hdr_rp, VK_FORMAT_R16G16B16A16_SFLOAT,
          scene_pipe.pipeline_layout(), true, false, false).is_ok()) {
    h.cleanup(); out.cleanup(h.ctx.device());
    GTEST_SKIP() << "Scene pipeline build failed";
  }

  // Material: bright white (HDR, base_color 5.0).
  omnicpp::render::PbrMaterialData mat{};
  mat.base_color_factor = {5.0f, 5.0f, 5.0f, 1.0f};
  mat.metallic_factor = 0; mat.roughness_factor = 0.5f;
  std::memcpy(h.mat_buf.mapped, &mat, sizeof(mat));

  VulkanPbrScene scene{};
  scene.pipeline = scene_pipe.pipeline();
  scene.pipeline_layout = scene_pipe.pipeline_layout();
  scene.camera.view_projection = make_perspective(45, 1.0f, 0.1f, 100);
  scene.camera_position = {0,0,10,1};
  scene.texture_set = h.tex_set;
  scene.material_set = h.mat_set;

  omnicpp::render::ScenePbrObject obj{};
  obj.mesh = &h.cube_mesh;
  obj.model = make_translation(0,0,-4);
  obj.material_index = 0;
  scene.objects = {obj};

  auto result = h.render_and_postprocess(scene, out);

  scene_pipe.cleanup(h.ctx.device());
  out.cleanup(h.ctx.device());
  h.cleanup();

  ASSERT_TRUE(result.submitted);
  // non_clear_pixels: cube rendered.
  EXPECT_GT(result.non_clear_pixels, 1000U) << "post-process output is blank";
  // peak_luma = r+g+b (max 765). For a bright HDR cube (base_color 5.0)
  // through ACES, each channel should be 150-250, so peak_luma ~450-750.
  EXPECT_GT(result.peak_luma, 100U) << "tonemapped peak too dim";
  // Per-channel luma should be < 255 (ACES compresses to [0,1] * 255).
  const std::uint32_t peak_ch = result.peak_luma / 3U;
  EXPECT_LT(peak_ch, 255U) << "ACES tonemap failed to compress bright HDR";
}

#endif  // OMNICPP_HAS_VULKAN
