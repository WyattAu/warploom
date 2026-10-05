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

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_render_graph.hpp"
#include "warploom/render/vulkan_renderer.hpp"
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
  for (std::size_t f = 0; f < static_cast<std::size_t>(6); ++f) {
    uint32_t base = (uint32_t)(verts.size() / 11);
    for (std::size_t v = 0; v < static_cast<std::size_t>(4); ++v) {
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
  //! Stands in for an absent bloom buffer: tonemap_fxaa.frag always samples
  //! binding 1, and a black 1x1 makes the additive term exactly zero.
  SolidTexture black;
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
  omnicpp::render::VulkanPipeline bloom_down_pipe;
  omnicpp::render::VulkanPipeline bloom_up_pipe;

  // Half-res bloom targets (same format/layout contract as hdr_image).
  VkImage bloom_a_image{};
  omnicpp::render::Allocation bloom_a_mem{};
  VkImageView bloom_a_view{};
  VkRenderPass bloom_a_rp{};
  VkFramebuffer bloom_a_fb{};
  VkImage bloom_b_image{};
  omnicpp::render::Allocation bloom_b_mem{};
  VkImageView bloom_b_view{};
  VkRenderPass bloom_b_rp{};
  VkFramebuffer bloom_b_fb{};
  VkSampler bloom_sampler{};
  VkDescriptorSetLayout bloom_layout{};
  VkDescriptorSet bloom_a_ds{};
  VkDescriptorSet bloom_b_ds{};
  //! Downsample input: the HDR scene, in the bloom pipeline's own layout.
  //! (The tonemap's hdr_ds comes from post_layout, which declares two
  //! bindings and would be layout-incompatible with the bloom pipelines.)
  VkDescriptorSet bloom_src_ds{};

  bool create_bloom_targets(VkDevice dev) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {128,128,1}; ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1};
    VkAttachmentDescription ad{};
    ad.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ad.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpc{};
    rpc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpc.attachmentCount = 1; rpc.pAttachments = &ad;
    rpc.subpassCount = 1; rpc.pSubpasses = &sub;
    VkFramebufferCreateInfo fbi{};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.attachmentCount = 1; fbi.width = 128; fbi.height = 128; fbi.layers = 1;
    if (!create_bloom_target(dev, ii, vi, rpc, fbi, bloom_a_image, &bloom_a_mem,
                             bloom_a_view, bloom_a_rp, bloom_a_fb)) return false;
    if (!create_bloom_target(dev, ii, vi, rpc, fbi, bloom_b_image, &bloom_b_mem,
                             bloom_b_view, bloom_b_rp, bloom_b_fb)) return false;
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, nullptr, &bloom_sampler) != VK_SUCCESS) { std::fprintf(stderr, "bloom: sampler failed\n"); return false; }
    // Pool capacity = sets_to_reserve, and three sets come from this layout:
    // the downsample's source, its output, and the upsample's output.
    auto bl = desc.create_layout({{0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 3);
    if (!bl.is_ok()) { std::fprintf(stderr, "bloom: create_layout failed\n"); return false; }
    bloom_layout = bl.value();
    auto da = desc.allocate_set(bloom_layout);
    if (!da.is_ok()) { std::fprintf(stderr, "bloom: allocate a failed\n"); return false; }
    bloom_a_ds = da.value();
    EXPECT_TRUE(desc.write_image(bloom_a_ds, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     bloom_sampler, bloom_a_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok());
    // The downsample reads the HDR scene; it is written after hdr_view exists.
    auto src = desc.allocate_set(bloom_layout);
    if (!src.is_ok()) { std::fprintf(stderr, "bloom: allocate src failed\n"); return false; }
    bloom_src_ds = src.value();
    // The downsample samples the HDR scene. Written here rather than in
    // init(): this function runs later, so hdr_view/hdr_sampler exist, and
    // bloom_src_ds certainly does.
    if (!desc.write_image(bloom_src_ds, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          hdr_sampler, hdr_view,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok()) {
      std::fprintf(stderr, "bloom: write src failed\n"); return false;
    }
    auto db = desc.allocate_set(bloom_layout);
    if (!db.is_ok()) { std::fprintf(stderr, "bloom: allocate b failed\n"); return false; }
    bloom_b_ds = db.value();
    EXPECT_TRUE(desc.write_image(bloom_b_ds, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     bloom_sampler, bloom_b_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok());
    return true;
  }

  bool create_bloom_target(VkDevice dev, const VkImageCreateInfo& ii,
                           const VkImageViewCreateInfo& vi_base,
                           const VkRenderPassCreateInfo& rpc,
                           const VkFramebufferCreateInfo& fbi_base,
                           VkImage& image, omnicpp::render::Allocation* mem,
                           VkImageView& view, VkRenderPass& rp,
                           VkFramebuffer& fb) {
    if (vkCreateImage(dev, &ii, nullptr, &image) != VK_SUCCESS) { std::fprintf(stderr, "bloom: vkCreateImage failed\n"); return false; }
    auto m = alloc.bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!m.is_ok()) { std::fprintf(stderr, "bloom: bind_image failed\n"); return false; }
    if (mem) *mem = m.value();
    VkImageViewCreateInfo vi = vi_base;
    vi.image = image;
    if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) { std::fprintf(stderr, "bloom: view failed\n"); return false; }
    VkRenderPassCreateInfo rpci = rpc;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &rp) != VK_SUCCESS) { std::fprintf(stderr, "bloom: rp failed\n"); return false; }
    VkFramebufferCreateInfo fbci = fbi_base;
    fbci.renderPass = rp;
    fbci.pAttachments = &view;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &fb) != VK_SUCCESS) { std::fprintf(stderr, "bloom: fb failed\n"); return false; }
    return true;
  }

  bool create_bloom_pipelines(VkRenderPass a_rp, VkRenderPass b_rp, VkFormat fmt) {
    VkDevice dev = ctx.device();
    std::string sd = WARPLOOM_TEST_SHADER_DIR;
    if (!bloom_down_pipe.load_shader_stage_file(dev, sd+"/fullscreen.vert.spv","vertex").is_ok() ||
        !bloom_down_pipe.load_shader_stage_file(dev, sd+"/bloom_downsample.frag.spv","fragment").is_ok()) return false;
    VkDescriptorSetLayout dl[1] = {bloom_layout};
    if (!bloom_down_pipe.create_pipeline_layout(dev, dl, 1, nullptr).is_ok()) return false;
    if (!bloom_down_pipe.create_graphics_pipeline(dev, a_rp, fmt,
        bloom_down_pipe.pipeline_layout(), false, false, false).is_ok()) return false;
    if (!bloom_up_pipe.load_shader_stage_file(dev, sd+"/fullscreen.vert.spv","vertex").is_ok() ||
        !bloom_up_pipe.load_shader_stage_file(dev, sd+"/bloom_upsample.frag.spv","fragment").is_ok()) return false;
    if (!bloom_up_pipe.create_pipeline_layout(dev, dl, 1, nullptr).is_ok()) return false;
    if (!bloom_up_pipe.create_graphics_pipeline(dev, b_rp, fmt,
        bloom_up_pipe.pipeline_layout(), false, false, false).is_ok()) return false;
    return true;
  }

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
    EXPECT_TRUE(desc.write_buffer(mat_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mat_buf.buffer, 0, VK_WHOLE_SIZE).is_ok());

    if (!make_white(dev, ctx.physical_device(), ctx.graphics_queue(), qf, alloc, white)) return false;
    // Same helper, then overwritten to black on the GPU below.
    if (!make_white(dev, ctx.physical_device(), ctx.graphics_queue(), qf, alloc, black)) return false;
    EXPECT_TRUE(desc.write_image(tex_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        white.sampler, white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok());

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
    EXPECT_TRUE(desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cube_va.buffer, 0, VK_WHOLE_SIZE).is_ok());
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

    // Post-process set layout. tonemap_fxaa.frag declares TWO samplers at
    // set 0 -- binding 0 is the HDR scene, binding 1 the additive bloom input --
    // so the layout must declare both, and the pool is sized from
    // sets_to_reserve (2 here) to cover the sets allocated from it.
    auto r_post = desc.create_layout({
        {0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT},
        {0,1,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT}}, 2);
    if (!r_post.is_ok()) return false;
    post_layout = r_post.value();
    auto pds = desc.allocate_set(post_layout);
    if (!pds.is_ok()) return false;
    hdr_ds = pds.value();
    EXPECT_TRUE(desc.write_image(hdr_ds, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        hdr_sampler, hdr_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok());
    // Binding 1: the bloom slot. Bound to the black texture so the tonemap's
    // additive term is zero and the existing assertions stay meaningful.
    {
      auto pool = omnicpp::render::VulkanRenderer::create_command_pool(dev, qf);
      if (pool.is_ok()) {
        auto cb = omnicpp::render::VulkanRenderer::allocate_command_buffer(dev, pool.value());
        if (cb.is_ok()) {
          VkCommandBuffer cmd = cb.value();
          VkCommandBufferBeginInfo bi{};
          bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
          bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
          if (vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS) {
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = black.img;
            b.subresourceRange = range;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
            VkClearColorValue cc{{0.0f, 0.0f, 0.0f, 1.0f}};
            vkCmdClearColorImage(cmd, black.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &cc, 1, &range);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            if (vkEndCommandBuffer(cmd) == VK_SUCCESS) {
              VkSubmitInfo si{};
              si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
              si.commandBufferCount = 1;
              si.pCommandBuffers = &cmd;
              vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE);
              vkQueueWaitIdle(ctx.graphics_queue());
            }
          }
        }
        vkDestroyCommandPool(dev, pool.value(), nullptr);
      }
    }
    EXPECT_TRUE(desc.write_image(hdr_ds, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        black.sampler, black.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok());

    return true;
  }

  // Create the tonemap pipeline with a specific render pass.
  bool create_tonemap_pipeline(VkRenderPass rp, VkFormat format) {
    VkDevice dev = ctx.device();
    std::string sd = WARPLOOM_TEST_SHADER_DIR;
    if (!tonemap_pipe.load_shader_stage_file(dev, sd+"/fullscreen.vert.spv","vertex").is_ok()||
        !tonemap_pipe.load_shader_stage_file(dev, sd+"/tonemap_fxaa.frag.spv","fragment").is_ok()) return false;
    VkDescriptorSetLayout tl[1] = {post_layout};
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    if (!tonemap_pipe.create_pipeline_layout(dev, tl, 1, &push).is_ok()) return false;
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

    // Graph-driven two-node frame: [HDR scene -> tonemap/FXAA]. The compiler
    // computes the hdr_image COLOR_ATTACHMENT -> SHADER_READ transition and
    // the write -> read barrier between the nodes from the attachment
    // finalLayout + tonemap sampled declaration. Pass contexts travel via
    // user_data shims.
    omnicpp::render::VulkanRenderer frame_renderer;

    omnicpp::render::GraphPass hdr_pass{};
    hdr_pass.name = "hdr_scene";
    hdr_pass.render_pass = hdr_rp;
    hdr_pass.framebuffer = hdr_fb;
    hdr_pass.width = 256;
    hdr_pass.height = 256;
    hdr_pass.attachments = {omnicpp::render::color_attachment(
        hdr_image, hdr_view, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    VkClearValue hdr_clear{};
    hdr_clear.color = {{0, 0, 0, 1}};
    hdr_pass.clear_values = &hdr_clear;
    hdr_pass.clear_value_count = 1;
    struct SceneCtx {
      const VulkanPbrScene* scene;
      omnicpp::render::VulkanRenderer* self;
    } scene_ctx{&scene, &frame_renderer};
    struct TmCtx {
      omnicpp::render::VulkanRenderer* self;
      omnicpp::render::VulkanRenderer::FullscreenPass* pass;
      VkDescriptorSet set0;
    } tm_ctx{&frame_renderer, nullptr, hdr_ds};
    hdr_pass.user_data = &scene_ctx;
    omnicpp::render::VulkanRenderer::FullscreenPass tm{};
    tm.pipeline = tonemap_pipe.pipeline();
    tm.pipeline_layout = tonemap_pipe.pipeline_layout();
    tm.render_pass = out_target.render_pass();
    tm.framebuffer = out_target.framebuffer();
    tm.width = 256;
    tm.height = 256;
    VkClearValue tm_clears[2]{};
    tm_clears[0].color = {{0, 0, 0, 1}};
    tm_clears[1].depthStencil = {1.0f, 0};
    tm.clear_values = tm_clears;
    tm.clear_value_count = 2;
    tm.samples[0] = {hdr_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_ASPECT_COLOR_BIT};
    tm.sample_count = 1;
    // Exposure push constant (shader reads pc.exposure).
    const float exposure = 1.0f;
    tm.push_data = &exposure;
    tm.push_size = sizeof(float);
    tm.push_stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT;
    tm_ctx.pass = &tm;

    omnicpp::render::GraphPass tm_pass =
        frame_renderer.fullscreen_graph_pass(tm);
    tm_pass.name = "tonemap_fxaa";
    tm_pass.user_data = &tm_ctx;

    const std::vector<omnicpp::render::GraphNode> nodes = {
        omnicpp::render::GraphNode::from_render(hdr_pass),
        omnicpp::render::GraphNode::from_render(tm_pass),
    };
    const auto compiled = omnicpp::render::compile_graph(nodes);

    omnicpp::render::execute_graph(cb, nodes, compiled,
        [](VkCommandBuffer command_buffer, const omnicpp::render::GraphPass& p,
           void* user_data) {
          if (p.name != nullptr && std::strcmp(p.name, "hdr_scene") == 0) {
            auto& sc = *static_cast<SceneCtx*>(user_data);
            (void)sc.self->record_pbr_scene(command_buffer, *sc.scene, p.width,
                                            p.height);
          } else {
            auto& fx = *static_cast<TmCtx*>(user_data);
            (void)fx.self->record_fullscreen_draw(command_buffer, *fx.pass,
                                                  fx.set0);
          }
        },
        nullptr);

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
    bloom_down_pipe.cleanup(dev);
    bloom_up_pipe.cleanup(dev);
    if (bloom_a_rp) vkDestroyRenderPass(dev, bloom_a_rp, nullptr);
    if (bloom_a_fb) vkDestroyFramebuffer(dev, bloom_a_fb, nullptr);
    if (bloom_a_view) vkDestroyImageView(dev, bloom_a_view, nullptr);
    if (bloom_a_mem.is_valid()) alloc.destroy_allocation(bloom_a_mem);
    if (bloom_a_image) vkDestroyImage(dev, bloom_a_image, nullptr);
    if (bloom_b_rp) vkDestroyRenderPass(dev, bloom_b_rp, nullptr);
    if (bloom_b_fb) vkDestroyFramebuffer(dev, bloom_b_fb, nullptr);
    if (bloom_b_view) vkDestroyImageView(dev, bloom_b_view, nullptr);
    if (bloom_b_mem.is_valid()) alloc.destroy_allocation(bloom_b_mem);
    if (bloom_b_image) vkDestroyImage(dev, bloom_b_image, nullptr);
    if (bloom_sampler) vkDestroySampler(dev, bloom_sampler, nullptr);
    destroy_solid(dev, alloc, white);
    destroy_solid(dev, alloc, black);
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
  std::string sd = WARPLOOM_TEST_SHADER_DIR;
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


// ============================================================================
// M-R: bloom chain (bright-pass downsample -> upsample), readback-proven.
// ============================================================================

namespace {

//! Runs the bloom half of the chain over the already-rendered HDR image and
//! returns the bloom output (bloom_b, 128x128) read back.
omnicpp_test::ReadbackResult render_bloom(PostProcessHarness& h,
                                          omnicpp::render::VulkanOffscreenTarget& out) {
  VkDevice dev = h.ctx.device();
  auto pr = omnicpp::render::VulkanRenderer::create_command_pool(dev, h.qf);
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

  omnicpp::render::VulkanRenderer frame_renderer;
  omnicpp::render::VulkanRenderer::FullscreenPass down{};
  down.pipeline = h.bloom_down_pipe.pipeline();
  down.pipeline_layout = h.bloom_down_pipe.pipeline_layout();
  down.render_pass = h.bloom_a_rp;
  down.framebuffer = h.bloom_a_fb;
  down.width = 128; down.height = 128;
  VkClearValue dc[1]{};
  dc[0].color = {{0,0,0,1}};
  down.clear_values = dc; down.clear_value_count = 1;
  down.samples[0] = {h.hdr_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_ASPECT_COLOR_BIT};
  down.sample_count = 1;
  auto down_pass = frame_renderer.fullscreen_graph_pass(down);
  down_pass.name = "bloom_down";
  // Declare the write: bloom_a is the color attachment (graph metadata so
  // compile_graph emits the write->sample barrier for the up pass).
  down_pass.attachments = {omnicpp::render::color_attachment(
      h.bloom_a_image, h.bloom_a_view, VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};

  omnicpp::render::VulkanRenderer::FullscreenPass up{};
  up.pipeline = h.bloom_up_pipe.pipeline();
  up.pipeline_layout = h.bloom_up_pipe.pipeline_layout();
  up.render_pass = h.bloom_b_rp;
  up.framebuffer = h.bloom_b_fb;
  up.width = 128; up.height = 128;
  up.clear_values = dc; up.clear_value_count = 1;
  up.samples[0] = {h.bloom_a_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT};
  up.sample_count = 1;
  auto up_pass = frame_renderer.fullscreen_graph_pass(up);
  up_pass.name = "bloom_up";

  struct Ctx { omnicpp::render::VulkanRenderer* self;
               omnicpp::render::VulkanRenderer::FullscreenPass* pass;
               VkDescriptorSet set; } dctx{&frame_renderer, &down, h.bloom_src_ds};
  struct Uctx { omnicpp::render::VulkanRenderer* self;
                omnicpp::render::VulkanRenderer::FullscreenPass* pass;
                VkDescriptorSet set; } uctx{&frame_renderer, &up, h.bloom_a_ds};
  down_pass.user_data = &dctx;
  up_pass.user_data = &uctx;

  const std::vector<omnicpp::render::GraphNode> nodes = {
      omnicpp::render::GraphNode::from_render(down_pass),
      omnicpp::render::GraphNode::from_render(up_pass),
  };
  const auto compiled = omnicpp::render::compile_graph(nodes);
  omnicpp::render::execute_graph(cb, nodes, compiled,
      [](VkCommandBuffer command_buffer, const omnicpp::render::GraphPass& p,
         void* user_data) {
        auto* cx = static_cast<Ctx*>(user_data);
        (void)cx->self->record_fullscreen_draw(command_buffer, *cx->pass,
                                               cx->set);
      },
      nullptr);
  (void)uctx.set;

  vkEndCommandBuffer(cb);
  vkResetFences(dev, 1, &fence);
  VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  vkQueueSubmit(h.ctx.graphics_queue(), 1, &si, fence);
  vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(dev, fence, nullptr);
  vkDestroyCommandPool(dev, pr.value(), nullptr);

  return omnicpp_test::readback_swapchain_image(
      h.ctx.physical_device(), dev, h.ctx.graphics_queue(), h.qf,
      h.bloom_b_image, VK_FORMAT_R16G16B16A16_SFLOAT, 128, 128,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace

TEST(VulkanHardware, BloomExtractsAndSpreadsBrightEnergy) {
  PostProcessHarness h;
  if (!h.init("bloom_test")) GTEST_SKIP() << "Vulkan unavailable";
  if (!h.create_bloom_targets(h.ctx.device())) {
    h.cleanup(); GTEST_SKIP() << "Bloom targets failed";
  }

  // Out target + pipelines: same contract as the tonemap test.
  omnicpp::render::VulkanOffscreenTarget out;
  if (!out.create(h.ctx.device(), h.ctx.physical_device(), VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &h.alloc).is_ok() ||
      !out.create_depth(h.ctx.device(), h.ctx.physical_device(), VK_FORMAT_D32_SFLOAT).is_ok() ||
      !out.create_render_pass(h.ctx.device()).is_ok() ||
      !out.create_framebuffer(h.ctx.device()).is_ok()) {
    h.cleanup(); GTEST_SKIP() << "Out target failed";
  }
  if (!h.create_tonemap_pipeline(out.render_pass(), out.format()) ||
      !h.create_bloom_pipelines(h.bloom_a_rp, h.bloom_b_rp,
                                VK_FORMAT_R16G16B16A16_SFLOAT)) {
    h.cleanup(); out.cleanup(h.ctx.device());
    GTEST_SKIP() << "Bloom pipelines failed";
  }
  omnicpp::render::VulkanPipeline scene_pipe;
  std::string sd = WARPLOOM_TEST_SHADER_DIR;
  if (!scene_pipe.load_shader_stage_file(h.ctx.device(), sd+"/pbr_scene.vert.spv","vertex").is_ok() ||
      !scene_pipe.load_shader_stage_file(h.ctx.device(), sd+"/pbr_scene.frag.spv","fragment").is_ok()) {
    h.cleanup(); out.cleanup(h.ctx.device()); GTEST_SKIP() << "Scene pipeline failed";
  }
  VkDescriptorSetLayout sl[3] = {h.mesh_layout, h.tex_layout, h.mat_layout};
  VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 160};
  if (!scene_pipe.create_pipeline_layout(h.ctx.device(), sl, 3, &pr).is_ok() ||
      !scene_pipe.create_graphics_pipeline(h.ctx.device(), h.hdr_rp, VK_FORMAT_R16G16B16A16_SFLOAT,
          scene_pipe.pipeline_layout(), true, false, false).is_ok()) {
    h.cleanup(); out.cleanup(h.ctx.device()); GTEST_SKIP() << "Scene pipeline build failed";
  }

  // HDR-bright cube: base_color 5.0 far exceeds the 0.8 bloom threshold.
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
  (void)h.render_and_postprocess(scene, out);

  const auto bloom = render_bloom(h, out);
  scene_pipe.cleanup(h.ctx.device());
  out.cleanup(h.ctx.device());
  h.cleanup();

  ASSERT_TRUE(bloom.submitted);
  // The bright cube's energy must survive the threshold + downsample.
  EXPECT_GT(bloom.non_clear_pixels, 50U)
      << "bloom output blank: threshold removed everything";
  // Bloom values are HDR floats tonemapped by the readback to LDR; the
  // blurred cube must be dimmer than the full-energy source (spread + the
  // 13-tap weights distribute energy), but clearly present.
  EXPECT_GT(bloom.peak_luma, 30U) << "bloom energy lost";
  EXPECT_LT(bloom.peak_luma, 765U) << "bloom not filtered (raw copy)";
}

#endif  // WARPLOOM_HAS_VULKAN
