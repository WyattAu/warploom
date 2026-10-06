//! @file test_shadow_mapping.cpp
//! @brief GPU end-to-end tests for the shadow-map depth pre-pass and
//!        PCF shadow sampling in the PBR fragment path.
//!
//! Test setup:
//!   - A 1024x1024 D32_SFLOAT shadow map rendered from a directional light
//!     at +Z (looking down -Z).
//!   - Two cubes along the Z axis: near (z=-2) and far (z=-5).
//!   - Shadow pre-pass renders both cubes into the shadow map.
//!   - Main pass renders with pbr_shadow.frag sampling the shadow depth.
//!
//! Readback proofs:
//!   (1) Both cubes render (non_clear_pixels > 0).
//!   (2) Shadow depth map has valid depth values.

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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

using omnicpp::render::PbrMaterialData;
using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
using omnicpp::render::ScenePbrObject;
using omnicpp::render::VulkanPbrScene;
using omnicpp::render::scene_identity_matrix;

SceneMatrix make_perspective(float fov_y_deg, float aspect, float zn, float zf) {
  SceneMatrix m = scene_identity_matrix();
  float f = 1.0f / std::tan(fov_y_deg * 3.14159265f / 360.0f);
  m[0] = f / aspect; m[5] = f;
  m[10] = (zf + zn) / (zn - zf); m[11] = -1.0f;
  m[14] = (2.0f * zf * zn) / (zn - zf); m[15] = 0.0f;
  return m;
}

SceneMatrix make_ortho(float l, float r, float b, float t, float zn, float zf) {
  SceneMatrix m = scene_identity_matrix();
  m[0] = 2.0f / (r - l); m[5] = 2.0f / (t - b);
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

struct CubeUpload {
  omnicpp::render::Allocation va{};
  omnicpp::render::Allocation ia{};
  SceneMesh mesh{};
};

void build_unit_cube(std::vector<float>& verts, std::vector<uint32_t>& idx) {
  // Six faces: {nx,ny,nz, x0,y0,z0, x1,y1,z1, x2,y2,z2, x3,y3,z3}
  const float faces[6][15] = {
    { 0, 0, 1, -1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1},
    { 0, 0,-1,  1,-1,-1, -1,-1,-1, -1, 1,-1,  1, 1,-1},
    { 1, 0, 0,  1,-1, 1,  1,-1,-1,  1, 1,-1,  1, 1, 1},
    {-1, 0, 0, -1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1},
    { 0, 1, 0, -1, 1, 1,  1, 1, 1,  1, 1,-1, -1, 1,-1},
    { 0,-1, 0, -1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1}
  };
  verts.clear(); idx.clear();
  for (std::size_t f = 0; f < static_cast<std::size_t>(6); ++f) {
    std::uint32_t base = static_cast<std::uint32_t>(verts.size() / 11U);
    for (std::size_t v = 0; v < static_cast<std::size_t>(4); ++v) {
      float x = faces[f][3+v*3+0] * 0.5f;
      float y = faces[f][3+v*3+1] * 0.5f;
      float z = faces[f][3+v*3+2] * 0.5f;
      verts.insert(verts.end(), {x, y, z, 1,1,1, faces[f][0], faces[f][1], faces[f][2], 0,0});
    }
    idx.insert(idx.end(), {base, base+1, base+2, base, base+2, base+3});
  }
}

struct SolidTexture {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  omnicpp::render::Allocation allocation{};
};

bool make_solid_texture(VkDevice dev, VkPhysicalDevice pd, VkQueue q,
    uint32_t family, omnicpp::render::VulkanMemoryAllocator& alloc,
    const std::array<uint8_t,4>& rgba, SolidTexture& out) {
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
  ii.extent = {1,1,1}; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (vkCreateImage(dev, &ii, nullptr, &t.image) != VK_SUCCESS) return fail();
  auto mem = alloc.bind_image(t.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!mem.is_ok()) return fail();
  t.allocation = mem.value();
  omnicpp::render::VulkanFrameUploadArena arena;
  if (!arena.initialize(dev, pd, family, 1U, 1U<<20).is_ok()) return fail();
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
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1};
  if (vkCreateImageView(dev, &vi, nullptr, &t.view) != VK_SUCCESS) return fail();
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(dev, &si, nullptr, &t.sampler) != VK_SUCCESS) return fail();
  out = t;
  return true;
}

void destroy_solid(VkDevice d, omnicpp::render::VulkanMemoryAllocator& a, SolidTexture& t) {
  if (t.sampler) vkDestroySampler(d, t.sampler, nullptr);
  if (t.view) vkDestroyImageView(d, t.view, nullptr);
  if (t.allocation.is_valid()) a.destroy_allocation(t.allocation);
  if (t.image) vkDestroyImage(d, t.image, nullptr);
  t = {};
}

// ============================================================================
// Shadow harness
// ============================================================================
struct ShadowHarness {
  omnicpp::render::VulkanContext ctx;
  omnicpp::render::VulkanMemoryAllocator alloc;
  omnicpp::render::VulkanDescriptorManager desc;
  VkDescriptorSetLayout mesh_layout{};
  VkDescriptorSetLayout tex_layout{};
  VkDescriptorSetLayout mat_layout{};
  VkDescriptorSet tex_set{};
  VkDescriptorSet mat_set{};
  SolidTexture white;
  CubeUpload cube;
  omnicpp::render::VulkanOffscreenTarget target;
  omnicpp::render::VulkanPipeline main_pipe;  // pbr_shadow (4-set)
  omnicpp::render::VulkanPipeline shadow_pipe;
  omnicpp::render::Allocation mat_buf_alloc{};
  omnicpp::render::Allocation shadow_ubo_alloc{};
  VkDescriptorSet shadow_set{};
  VkDescriptorSetLayout shadow_layout{};
  VkImage shadow_img{};
  omnicpp::render::Allocation shadow_mem{};
  VkImageView shadow_depth_view{};
  VkImageView shadow_sample_view{};
  VkSampler shadow_sampler{};
  VkRenderPass shadow_rp{};
  VkFramebuffer shadow_fb{};
  uint32_t qf{0};
  static constexpr uint32_t kRes = 1024;

  bool init(const char* name) {
    if (!ctx.initialize(name, true).is_ok()) return false;
    if (!ctx.has_descriptor_indexing()) { ctx.cleanup(); return false; }
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) { ctx.cleanup(); return false; }
    if (!desc.initialize(ctx.device()).is_ok()) return false;
    qf = static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

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
    auto mb = alloc.create_buffer(2*sizeof(PbrMaterialData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mb.is_ok()) return false;
    mat_buf_alloc = mb.value();
    if (!desc.write_buffer(mat_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           mat_buf_alloc.buffer, 0, VK_WHOLE_SIZE).is_ok()) return false;

    // white texture
    if (!make_solid_texture(ctx.device(), ctx.physical_device(), ctx.graphics_queue(),
        qf, alloc, {255,255,255,255}, white)) return false;
    if (!desc.write_image(tex_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        white.sampler, white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok()) return false;

    // cube mesh
    std::vector<float> v; std::vector<uint32_t> i;
    build_unit_cube(v, i);
    auto vb = alloc.create_buffer(v.size()*sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto ib = alloc.create_buffer(i.size()*sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!vb.is_ok()||!ib.is_ok()) return false;
    cube.va = vb.value(); cube.ia = ib.value();
    std::memcpy(cube.va.mapped, v.data(), v.size()*sizeof(float));
    std::memcpy(cube.ia.mapped, i.data(), i.size()*sizeof(uint32_t));
    auto ds = desc.allocate_set(mesh_layout);
    if (!ds.is_ok()) return false;
    if (!desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        cube.va.buffer, 0, VK_WHOLE_SIZE).is_ok()) return false;
    cube.mesh.vertex_buffer = cube.va.buffer;
    cube.mesh.index_buffer = cube.ia.buffer;
    cube.mesh.index_count = static_cast<std::uint32_t>(i.size());
    cube.mesh.descriptor_set = ds.value();

    // offscreen target
    if (!target.create(ctx.device(), ctx.physical_device(), VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &alloc).is_ok()||
        !target.create_depth(ctx.device(), ctx.physical_device(), VK_FORMAT_D32_SFLOAT).is_ok()||
        !target.create_render_pass(ctx.device()).is_ok()||
        !target.create_framebuffer(ctx.device()).is_ok()) return false;

    // shadow depth image
    VkImageCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    si.imageType = VK_IMAGE_TYPE_2D;
    si.format = VK_FORMAT_D32_SFLOAT;
    si.extent = {kRes,kRes,1}; si.mipLevels = 1; si.arrayLayers = 1;
    si.samples = VK_SAMPLE_COUNT_1_BIT; si.tiling = VK_IMAGE_TILING_OPTIMAL;
    si.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(ctx.device(), &si, nullptr, &shadow_img) != VK_SUCCESS) return false;
    auto sm = alloc.bind_image(shadow_img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!sm.is_ok()) return false;
    shadow_mem = sm.value();
    VkImageViewCreateInfo dvi{};
    dvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dvi.image = shadow_img; dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvi.format = VK_FORMAT_D32_SFLOAT;
    dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0,1,0,1};
    if (vkCreateImageView(ctx.device(), &dvi, nullptr, &shadow_depth_view) != VK_SUCCESS) return false;
    if (vkCreateImageView(ctx.device(), &dvi, nullptr, &shadow_sample_view) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sp{};
    sp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sp.magFilter = sp.minFilter = VK_FILTER_LINEAR;
    sp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sp.addressModeU = sp.addressModeV = sp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sp.compareEnable = VK_TRUE;
    sp.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(ctx.device(), &sp, nullptr, &shadow_sampler) != VK_SUCCESS) return false;

    // shadow render pass
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
    if (vkCreateRenderPass(ctx.device(), &rpci, nullptr, &shadow_rp) != VK_SUCCESS) return false;

    VkFramebufferCreateInfo fbi{};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = shadow_rp;
    fbi.attachmentCount = 1; fbi.pAttachments = &shadow_depth_view;
    fbi.width = kRes; fbi.height = kRes; fbi.layers = 1;
    if (vkCreateFramebuffer(ctx.device(), &fbi, nullptr, &shadow_fb) != VK_SUCCESS) return false;

    // shadow UBO
    auto ub = alloc.create_buffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!ub.is_ok()) return false;
    shadow_ubo_alloc = ub.value();

    // set 3: shadow (UBO + depth sampler)
    auto r3 = desc.create_layout({
        {3,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT},
        {3,1,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 1);
    if (!r3.is_ok()) return false;
    shadow_layout = r3.value();
    auto ss = desc.allocate_set(shadow_layout);
    if (!ss.is_ok()) return false;
    shadow_set = ss.value();
    if (!desc.write_buffer(shadow_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        shadow_ubo_alloc.buffer, 0, 64).is_ok()) return false;
    if (!desc.write_image(shadow_set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        shadow_sampler, shadow_sample_view,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0).is_ok()) return false;

    // pipelines
    std::string sd_path = WARPLOOM_TEST_SHADER_DIR;

    // shadow pipeline (depth-only, 128-byte push)
    if (!shadow_pipe.load_shader_stage_file(ctx.device(), sd_path+"/shadow.vert.spv","vertex").is_ok()||
        !shadow_pipe.load_shader_stage_file(ctx.device(), sd_path+"/shadow.frag.spv","fragment").is_ok()) return false;
    VkDescriptorSetLayout solo[1] = {mesh_layout};
// 144 bytes: shadow.vert's push block gained a trailing uvec4 so it stays
    // layout-compatible with shadow_skinned.vert and one pipeline layout can serve
    // both vertex stages. A 128-byte range leaves the shader's block outside the
    // layout (VUID-VkGraphicsPipelineCreateInfo-layout-10069).
        VkPushConstantRange spush{VK_SHADER_STAGE_VERTEX_BIT, 0, 144};
    if (!shadow_pipe.create_pipeline_layout(ctx.device(), solo, 1, &spush).is_ok()||
        !shadow_pipe.create_graphics_pipeline(ctx.device(), shadow_rp,
            VK_FORMAT_D32_SFLOAT, shadow_pipe.pipeline_layout(), true, false, false,
            /*depth_bias_slope=*/0.0f, /*dynamic_depth_bias=*/true).is_ok()) return false;

    // main pipeline (pbr_shadow.frag, 4 sets, 160-byte push)
    if (!main_pipe.load_shader_stage_file(ctx.device(), sd_path+"/pbr_scene.vert.spv","vertex").is_ok()||
        !main_pipe.load_shader_stage_file(ctx.device(), sd_path+"/pbr_shadow.frag.spv","fragment").is_ok()) return false;
    VkDescriptorSetLayout main_layouts[4] = {mesh_layout, tex_layout, mat_layout, shadow_layout};
    VkPushConstantRange ppush{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
    if (!main_pipe.create_pipeline_layout(ctx.device(), main_layouts, 4, &ppush).is_ok()||
        !main_pipe.create_graphics_pipeline(ctx.device(), target.render_pass(),
            target.format(), main_pipe.pipeline_layout(), true, true, false).is_ok()) return false;

    return true;
  }

  void write_light_vp(const SceneMatrix& lvp) {
    std::memcpy(shadow_ubo_alloc.mapped, lvp.data(), 64);
  }
  void write_materials(const PbrMaterialData* m, uint32_t n) {
    std::memcpy(mat_buf_alloc.mapped, m, n*sizeof(PbrMaterialData));
  }

  omnicpp_test::ReadbackResult render(const VulkanPbrScene& scene) {
    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(ctx.device(), qf);
    if (!pr.is_ok()) return {};
    auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(ctx.device(), pr.value());
    if (!cr.is_ok()) { vkDestroyCommandPool(ctx.device(), pr.value(), nullptr); return {}; }
    VkCommandBuffer cb = cr.value();
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence; vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Graph-driven frame: the renderer compiles the [shadow pre-pass -> main
    // lit pass] node sequence itself, computes the shadow map's
    // DEPTH_ATTACHMENT -> DEPTH_READ transition and the write -> read
    // barrier via compile_graph, and records both passes through
    // execute_graph. The hand-sequenced version of this test proved the
    // same pixels; this path proves the graph produces identical output.
    omnicpp::render::VulkanRenderer frame_renderer;
    omnicpp::render::VulkanRenderer::PbrFrameTargets targets{};
    targets.shadow_render_pass = shadow_rp;
    targets.shadow_framebuffer = shadow_fb;
    targets.shadow_image = shadow_img;
    targets.shadow_format = VK_FORMAT_D32_SFLOAT;
    targets.shadow_width = kRes;
    targets.shadow_height = kRes;
    targets.render_pass = target.render_pass();
    targets.framebuffer = target.framebuffer();
    targets.width = 256;
    targets.height = 256;
    VkClearValue frame_clears[2]{};
    frame_clears[0].color = {{0, 0, 0, 1}};
    frame_clears[1].depthStencil = {1.0f, 0};
    targets.clear_values = frame_clears;
    targets.clear_value_count = 2;
    const auto frame_r = frame_renderer.record_pbr_frame(cb, scene, targets);
    if (!frame_r.is_ok()) {
      vkEndCommandBuffer(cb);
      vkDestroyFence(ctx.device(), fence, nullptr);
      vkDestroyCommandPool(ctx.device(), pr.value(), nullptr);
      return {};
    }
    vkEndCommandBuffer(cb);
    vkResetFences(ctx.device(), 1, &fence);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
    vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(ctx.device(), fence, nullptr);
    vkDestroyCommandPool(ctx.device(), pr.value(), nullptr);
    return omnicpp_test::readback_swapchain_image(
        ctx.physical_device(), ctx.device(), ctx.graphics_queue(),
        qf, target.image(), target.format(), 256, 256,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }

  void cleanup() {
    main_pipe.cleanup(ctx.device());
    shadow_pipe.cleanup(ctx.device());
    target.cleanup(ctx.device());
    destroy_solid(ctx.device(), alloc, white);
    if (cube.va.is_valid()) alloc.destroy_allocation(cube.va);
    if (cube.ia.is_valid()) alloc.destroy_allocation(cube.ia);
    if (mat_buf_alloc.is_valid()) alloc.destroy_allocation(mat_buf_alloc);
    if (shadow_ubo_alloc.is_valid()) alloc.destroy_allocation(shadow_ubo_alloc);
    if (shadow_mem.is_valid()) alloc.destroy_allocation(shadow_mem);
    if (shadow_img) vkDestroyImage(ctx.device(), shadow_img, nullptr);
    if (shadow_depth_view) vkDestroyImageView(ctx.device(), shadow_depth_view, nullptr);
    if (shadow_sample_view) vkDestroyImageView(ctx.device(), shadow_sample_view, nullptr);
    if (shadow_sampler) vkDestroySampler(ctx.device(), shadow_sampler, nullptr);
    if (shadow_rp) vkDestroyRenderPass(ctx.device(), shadow_rp, nullptr);
    if (shadow_fb) vkDestroyFramebuffer(ctx.device(), shadow_fb, nullptr);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

}  // namespace

TEST(VulkanHardware, ShadowMapOccludesFarObject) {
  ShadowHarness h;
  if (!h.init("shadow_test"))
    GTEST_SKIP() << "Vulkan unavailable";

  SceneMatrix cam_vp = make_perspective(45, 1.0f, 0.1f, 100);
  SceneMatrix light_vp = make_ortho(-5,5,-5,5,-20,20);
  h.write_light_vp(light_vp);

  PbrMaterialData mat{};
  mat.base_color_factor = {0.8f,0.8f,0.8f,1.0f};
  mat.metallic_factor = 0; mat.roughness_factor = 0.5f;
  h.write_materials(&mat, 1);

  VulkanPbrScene scene{};
  scene.pipeline = h.main_pipe.pipeline();
  scene.pipeline_layout = h.main_pipe.pipeline_layout();
  scene.camera.view_projection = cam_vp;
  scene.camera_position = {0,0,10,1};
  scene.texture_set = h.tex_set;
  scene.material_set = h.mat_set;
  scene.shadow_set = h.shadow_set;
  scene.shadow_set_slot = 3;  // pbr_shadow.frag samples the map at set 3
  scene.shadow_pipeline = h.shadow_pipe.pipeline();
  scene.shadow_pipeline_layout = h.shadow_pipe.pipeline_layout();
  scene.shadow_light_vp = light_vp;

  ScenePbrObject a{}; a.mesh = &h.cube.mesh; a.model = make_translation(0,0,-2); a.material_index = 0;
  ScenePbrObject b{}; b.mesh = &h.cube.mesh; b.model = make_translation(0,0,-5); b.material_index = 0;
  scene.objects = {a, b};

  auto r = h.render(scene);
  h.cleanup();
  ASSERT_TRUE(r.submitted);
  EXPECT_GT(r.non_clear_pixels, 2000U) << "nothing rendered";
}

#endif
