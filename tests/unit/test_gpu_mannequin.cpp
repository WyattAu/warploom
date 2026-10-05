//! @file test_gpu_mannequin.cpp
//! @brief GPU end-to-end proof for the skeletal glTF path: the mannequin
//!        asset on disk (scripts/generate_mannequin.py) is imported through
//!        import_gltf_animation_document, its walk-cycle animation is
//!        sampled on the CPU, and skinning matrices are uploaded to the
//!        proven skinned_scene.vert pipeline. Pixel-readback proofs:
//!          (1) the rest pose renders the standing figure (visible pixels);
//!          (2) the mid-stride pose renders a different image (legs swing);
//!          (3) leg region pixel counts differ between poses (skeletal
//!              deformation, not a whole-image shift).
//!
//! The CPU computes joints = global_rest(j)^-1 * global_pose(j) directly
//! (cross-checking the engine helper), so a wrong matrix composition or a
//! wrong SSBO upload shows up as identical images rather than a pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "warploom/asset/gltf_animation.hpp"
#include "warploom/asset/gltf_importer.hpp"
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

#ifndef WARPLOOM_TEST_ASSET_DIR
#define WARPLOOM_TEST_ASSET_DIR "assets/models"
#endif

namespace {

using omnicpp::asset::GltfAnimationDocument;
using omnicpp::asset::GltfChannel;
using omnicpp::asset::GltfSampler;
using omnicpp::asset::GltfSamplerInterpolation;
using omnicpp::render::Allocation;
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

struct MannequinAsset {
  std::string json;
  std::vector<char> bin;
  bool loaded{false};
};

MannequinAsset load_asset() {
  MannequinAsset asset;
  const std::string dir =
      WARPLOOM_TEST_ASSET_DIR[0] != 0 ? WARPLOOM_TEST_ASSET_DIR
                                     : "assets/models";
  std::ifstream json_file(dir + "/mannequin.gltf", std::ios::binary);
  if (!json_file) return asset;
  std::vector<char> json_bytes((std::istreambuf_iterator<char>(json_file)),
                               std::istreambuf_iterator<char>());
  std::ifstream bin_file(dir + "/mannequin.bin", std::ios::binary);
  if (!bin_file) return asset;
  std::vector<char> bin_bytes((std::istreambuf_iterator<char>(bin_file)),
                              std::istreambuf_iterator<char>());
  asset.json.assign(json_bytes.begin(), json_bytes.end());
  asset.bin = bin_bytes;
  asset.loaded = true;
  return asset;
}

//! Sample every channel of the clip into the node TRS components.
void apply_pose(GltfAnimationDocument& doc,
                const omnicpp::asset::GltfAnimationImport& anim,
                float time) {
  for (const auto& channel : anim.channels) {
    float out[4];
    omnicpp::asset::sample_gltf_channel(anim.samplers[channel.sampler], time,
                                        out);
    auto& node = doc.nodes[channel.target_node];
    switch (channel.path) {
      case GltfChannel::Path::Translation:
        node.translation[0] = out[0];
        node.translation[1] = out[1];
        node.translation[2] = out[2];
        break;
      case GltfChannel::Path::Rotation:
        node.rotation[0] = out[0];
        node.rotation[1] = out[1];
        node.rotation[2] = out[2];
        node.rotation[3] = out[3];
        break;
      case GltfChannel::Path::Scale:
        node.scale[0] = out[0];
        node.scale[1] = out[1];
        node.scale[2] = out[2];
        break;
    }
  }
}

//! Column-major global matrices composed down the forest (independent of the
//! engine helper so the GPU test cross-checks the CPU math too).
void compute_globals(const GltfAnimationDocument& doc,
                     std::vector<SceneMatrix>& globals) {
  globals.assign(doc.nodes.size(), scene_identity_matrix());
  struct Local { float t[3]; float r[4]; float s[3]; };
  std::vector<Local> locals(doc.nodes.size());
  for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
    const auto& n = doc.nodes[i];
    std::memcpy(locals[i].t, n.translation, sizeof(locals[i].t));
    std::memcpy(locals[i].r, n.rotation, sizeof(locals[i].r));
    std::memcpy(locals[i].s, n.scale, sizeof(locals[i].s));
  }
  auto compose = [](const Local& l) {
    const float x = l.r[0], y = l.r[1], z = l.r[2], w = l.r[3];
    SceneMatrix m = scene_identity_matrix();
    m[0]  = (1 - 2 * (y * y + z * z)) * l.s[0];
    m[1]  = 2 * (x * y + z * w) * l.s[0];
    m[2]  = 2 * (x * z - y * w) * l.s[0];
    m[4]  = 2 * (x * y - z * w) * l.s[1];
    m[5]  = (1 - 2 * (x * x + z * z)) * l.s[1];
    m[6]  = 2 * (y * z + x * w) * l.s[1];
    m[8]  = 2 * (x * z + y * w) * l.s[2];
    m[9]  = 2 * (y * z - x * w) * l.s[2];
    m[10] = (1 - 2 * (x * x + y * y)) * l.s[2];
    m[12] = l.t[0]; m[13] = l.t[1]; m[14] = l.t[2];
    return m;
  };
  auto mul = [](const SceneMatrix& a, const SceneMatrix& b) {
    SceneMatrix out{};
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
          sum += a[r + 4 * k] * b[k + 4 * c];
        }
        out[r + 4 * c] = sum;
      }
    }
    return out;
  };
  // Parent-before-child evaluation from the roots.
  std::vector<std::uint8_t> done(doc.nodes.size(), 0);
  std::vector<std::size_t> stack;
  for (std::size_t root = 0; root < doc.nodes.size(); ++root) {
    if (doc.nodes[root].parent != omnicpp::asset::kGltfNoParent) continue;
    stack.push_back(root);
    while (!stack.empty()) {
      const std::size_t cur = stack.back();
      stack.pop_back();
      if (done[cur] != 0) continue;
      SceneMatrix local = compose(locals[cur]);
      const std::size_t parent = doc.nodes[cur].parent;
      globals[cur] = parent == omnicpp::asset::kGltfNoParent
                         ? local
                         : mul(globals[parent], local);
      done[cur] = 1;
      for (const std::size_t child : doc.nodes[cur].children) {
        if (done[child] == 0) stack.push_back(child);
      }
    }
  }
}

struct SolidTexture {
  VkImage img{};
  VkImageView view{};
  VkSampler sampler{};
  Allocation alloc{};
};

bool make_white(VkDevice d, VkPhysicalDevice pd, VkQueue q, uint32_t f,
                omnicpp::render::VulkanMemoryAllocator& a,
                SolidTexture& out) {
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
  if (vkCreateImage(d, &ii, nullptr, &out.img) != VK_SUCCESS) return false;
  auto m = a.bind_image(out.img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!m.is_ok()) return false;
  out.alloc = m.value();
  omnicpp::render::VulkanFrameUploadArena arena;
  if (!arena.initialize(d, pd, f, 1, 1 << 20).is_ok()) return false;
  if (!arena.begin_frame(0).is_ok()) return false;
  auto s = arena.acquire(4);
  if (!s.is_ok()) return false;
  uint8_t w[4] = {255, 255, 255, 255};
  std::memcpy(s.value().host_data, w, 4);
  arena.record_copy_image_rgba8(s.value(), out.img, 1, 1);
  if (!arena.submit(q).is_ok()) return false;
  arena.wait_idle();
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(d, &vi, nullptr, &out.view);
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(d, &si, nullptr, &out.sampler);
  return true;
}

void destroy_solid(VkDevice d, omnicpp::render::VulkanMemoryAllocator& a,
                   SolidTexture& t) {
  if (t.sampler) vkDestroySampler(d, t.sampler, nullptr);
  if (t.view) vkDestroyImageView(d, t.view, nullptr);
  if (t.alloc.is_valid()) a.destroy_allocation(t.alloc);
  if (t.img) vkDestroyImage(d, t.img, nullptr);
  t = {};
}

//! One skinned mesh of the mannequin, GPU-ready.
struct SkinnedMeshGpu {
  omnicpp::render::Allocation va{};
  omnicpp::render::Allocation ia{};
  SceneMesh mesh{};
};

struct MannequinHarness {
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
  std::vector<SkinnedMeshGpu> meshes;
  Allocation mat_buf{};
  Allocation bone_buf{};
  omnicpp::render::VulkanOffscreenTarget target;
  omnicpp::render::VulkanPipeline pipe;
  uint32_t qf{0};

  bool init(const char* name) {
    if (!ctx.initialize(name, true).is_ok()) return false;
    if (!ctx.has_descriptor_indexing()) { ctx.cleanup(); return false; }
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) {
      ctx.cleanup();
      return false;
    }
    if (!desc.initialize(ctx.device()).is_ok()) return false;
    qf = (uint32_t)ctx.queue_families().graphics_family;
    VkDevice dev = ctx.device();

    // Pool sized for the 9 mannequin meshes plus headroom.
    auto r0 = desc.create_layout(
        {{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          VK_SHADER_STAGE_VERTEX_BIT}},
        32);
    if (!r0.is_ok()) return false;
    mesh_layout = r0.value();
    auto r1 = desc.create_layout(
        {{1, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          VK_SHADER_STAGE_FRAGMENT_BIT}},
        1, true);
    if (!r1.is_ok()) return false;
    tex_layout = r1.value();
    auto ts = desc.allocate_set(tex_layout);
    if (!ts.is_ok()) return false;
    tex_set = ts.value();
    auto r2 = desc.create_layout(
        {{2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          VK_SHADER_STAGE_FRAGMENT_BIT}},
        8);
    if (!r2.is_ok()) return false;
    mat_layout = r2.value();
    auto ms0 = desc.allocate_set(mat_layout);
    if (!ms0.is_ok()) return false;
    mat_set = ms0.value();
    auto mb0 = alloc.create_buffer(
        sizeof(PbrMaterialData),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mb0.is_ok()) return false;
    mat_buf = mb0.value();
    EXPECT_TRUE(desc.write_buffer(mat_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      mat_buf.buffer, 0, VK_WHOLE_SIZE).is_ok());
    auto r5 = desc.create_layout(
        {{3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          VK_SHADER_STAGE_VERTEX_BIT}},
        8);
    if (!r5.is_ok()) return false;
    bone_layout = r5.value();
    auto bs = desc.allocate_set(bone_layout);
    if (!bs.is_ok()) return false;
    bone_set = bs.value();

    // Bone SSBO (set 3): 15 joints * 64 bytes.
    auto bb = alloc.create_buffer(
        15 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!bb.is_ok()) return false;
    bone_buf = bb.value();
    EXPECT_TRUE(desc.write_buffer(bone_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      bone_buf.buffer, 0, VK_WHOLE_SIZE).is_ok());

    if (!make_white(dev, ctx.physical_device(), ctx.graphics_queue(), qf,
                    alloc, white)) {
      return false;
    }
    EXPECT_TRUE(desc.write_image(tex_set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     white.sampler, white.view,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0).is_ok());

    if (!target.create(dev, ctx.physical_device(), VK_FORMAT_B8G8R8A8_UNORM,
                       256, 256, &alloc).is_ok() ||
        !target.create_depth(dev, ctx.physical_device(),
                             VK_FORMAT_D32_SFLOAT).is_ok() ||
        !target.create_render_pass(dev).is_ok() ||
        !target.create_framebuffer(dev).is_ok()) {
      return false;
    }

    std::string sd = WARPLOOM_TEST_SHADER_DIR;
    if (!pipe.load_shader_stage_file(dev, sd + "/skinned_scene.vert.spv",
                                     "vertex").is_ok() ||
        !pipe.load_shader_stage_file(dev, sd + "/pbr_scene.frag.spv",
                                     "fragment").is_ok()) {
      return false;
    }
    VkDescriptorSetLayout layouts[4] = {mesh_layout, tex_layout, mat_layout,
                                        bone_layout};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 160};
    if (!pipe.create_pipeline_layout(dev, layouts, 4, &pr).is_ok() ||
        !pipe.create_graphics_pipeline(dev, target.render_pass(),
                                       target.format(),
                                       pipe.pipeline_layout(), true, true,
                                       false).is_ok()) {
      return false;
    }
    return true;
  }

  //! Import + upload one mannequin mesh; returns false on any failure.
  bool upload_mesh(const omnicpp::asset::GltfMeshImport& import,
                   const omnicpp::asset::GltfSkinBinding& binding,
                   const PbrMaterialData& material) {
    VkDevice dev = ctx.device();
    // Combined SSBO: [static verts][joints as float x4][weights x4].
    std::vector<float> combined = import.vertices;
    const std::size_t vertex_count = import.vertex_count();
    combined.reserve(combined.size() + vertex_count * 8U);
    for (std::size_t v = 0; v < vertex_count; ++v) {
      for (std::size_t c = 0; c < 4; ++c) {
        combined.push_back(
            static_cast<float>(binding.joints[v * 4U + c]));
      }
      for (std::size_t c = 0; c < 4; ++c) {
        combined.push_back(binding.weights[v * 4U + c]);
      }
    }
    SkinnedMeshGpu gpu;
    auto vb = alloc.create_buffer(
        combined.size() * sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto ib = alloc.create_buffer(
        import.indices.size() * sizeof(std::uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!vb.is_ok() || !ib.is_ok()) return false;
    gpu.va = vb.value();
    gpu.ia = ib.value();
    std::memcpy(gpu.va.mapped, combined.data(),
                combined.size() * sizeof(float));
    std::memcpy(gpu.ia.mapped, import.indices.data(),
                import.indices.size() * sizeof(std::uint32_t));
    auto ds = desc.allocate_set(mesh_layout);
    if (!ds.is_ok()) return false;
    EXPECT_TRUE(desc.write_buffer(ds.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      gpu.va.buffer, 0, VK_WHOLE_SIZE).is_ok());
    gpu.mesh.vertex_buffer = gpu.va.buffer;
    gpu.mesh.index_buffer = gpu.ia.buffer;
    gpu.mesh.index_count = static_cast<std::uint32_t>(import.indices.size());
    gpu.mesh.descriptor_set = ds.value();

    (void)material;  // materials are shared through the single set-2 SSBO
    meshes.push_back(std::move(gpu));
    return true;
  }

  void write_material(const PbrMaterialData& material) {
    std::memcpy(mat_buf.mapped, &material, sizeof(material));
  }

  void write_bones(const SceneMatrix* bones, uint32_t count) {
    std::memcpy(bone_buf.mapped, bones, count * 64);
  }

  omnicpp_test::ReadbackResult render(const VulkanPbrScene& scene) {
    VkDevice dev = ctx.device();
    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(dev, qf);
    if (!pr.is_ok()) return {};
    auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        dev, pr.value());
    if (!cr.is_ok()) {
      vkDestroyCommandPool(dev, pr.value(), nullptr);
      return {};
    }
    VkCommandBuffer cb = cr.value();
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(dev, &fi, nullptr, &fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clears[2]{};
    clears[0].color = {{0, 0, 0, 1}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = target.render_pass();
    rpb.framebuffer = target.framebuffer();
    rpb.renderArea.extent = {256, 256};
    rpb.clearValueCount = 2;
    rpb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, 256, 256, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {256, 256}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.pipeline);
    if (scene.texture_set) {
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              scene.pipeline_layout, 1, 1, &scene.texture_set,
                              0, nullptr);
    }
    if (scene.bone_set) {
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              scene.pipeline_layout, 3, 1, &scene.bone_set, 0,
                              nullptr);
    }
    struct Push {
      SceneMatrix vp;
      SceneMatrix model;
      std::array<float, 4> cam;
      uint32_t mi;
      uint32_t p[3]{};
    } push{};
    push.vp = scene.camera.view_projection;
    push.cam = scene.camera_position;
    VkShaderStageFlags ks = VK_SHADER_STAGE_VERTEX_BIT |
                            VK_SHADER_STAGE_FRAGMENT_BIT;
    for (const auto& obj : scene.objects) {
      const auto* m = obj.effective_mesh();
      if (!m || !m->is_drawable()) continue;
      push.model = obj.model;
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              scene.pipeline_layout, 0, 1,
                              &m->descriptor_set, 0, nullptr);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              scene.pipeline_layout, 2, 1,
                              &scene.material_set, 0, nullptr);
      vkCmdPushConstants(cb, scene.pipeline_layout, ks, 0, sizeof(push),
                         &push);
      vkCmdBindIndexBuffer(cb, m->index_buffer, m->index_offset,
                           VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cb, m->index_count, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    vkResetFences(dev, 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pr.value(), nullptr);
    return omnicpp_test::readback_swapchain_image(
        ctx.physical_device(), dev, ctx.graphics_queue(), qf, target.image(),
        target.format(), 256, 256, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }

  void cleanup() {
    VkDevice dev = ctx.device();
    for (auto& m : meshes) {
      if (m.va.is_valid()) alloc.destroy_allocation(m.va);
      if (m.ia.is_valid()) alloc.destroy_allocation(m.ia);
    }
    meshes.clear();
    if (mat_buf.is_valid()) alloc.destroy_allocation(mat_buf);
    pipe.cleanup(dev);
    target.cleanup(dev);
    destroy_solid(dev, alloc, white);
    if (bone_buf.is_valid()) alloc.destroy_allocation(bone_buf);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

}  // namespace

//! The imported mannequin walks: rest vs mid-stride poses render different
//! images through the GPU skinned pipeline, with the deformation visible in
//! the leg region (lower half of the frame).
TEST(VulkanHardware, GpuMannequinWalkCycle) {
  const MannequinAsset asset = load_asset();
  if (!asset.loaded) GTEST_SKIP() << "mannequin asset not found";

  std::string error;
  auto imported = omnicpp::asset::import_gltf_animation_document(
      asset.json.data(), asset.json.size(),
      reinterpret_cast<const std::uint8_t*>(asset.bin.data()), asset.bin.size(),
      &error);
  ASSERT_TRUE(imported.is_ok()) << error;
  GltfAnimationDocument doc = std::move(imported.value());
  ASSERT_EQ(doc.skins.size(), 1U);
  ASSERT_FALSE(doc.animations.empty());
  const auto& anim = doc.animations[0];

  MannequinHarness h;
  if (!h.init("mannequin_gpu_test")) {
    GTEST_SKIP() << "Vulkan unavailable";
  }

  // Upload the three most visually distinct parts (both legs + torso share
  // the material path; head/arms are also drawn to make the figure whole).
  PbrMaterialData mat{};
  mat.base_color_factor = {0.8f, 0.6f, 0.45f, 1.0f};
  mat.metallic_factor = 0;
  mat.roughness_factor = 0.9f;
  for (std::size_t i = 0; i < doc.meshes.size(); ++i) {
    ASSERT_TRUE(h.upload_mesh(doc.meshes[i], doc.skin_bindings[i], mat))
        << "mesh " << i << " upload failed";
  }

  VulkanPbrScene scene{};
  scene.pipeline = h.pipe.pipeline();
  scene.pipeline_layout = h.pipe.pipeline_layout();
  // Camera at z=+3.2 looking at the figure standing near the origin; the
  // mannequin occupies roughly y in [0.3, 1.7] in bind space.
  scene.camera.view_projection = make_perspective(45, 1.0f, 0.1f, 100);
  scene.camera_position = {0, 1.0f, 3.2f, 1};
  h.write_material(mat);
  scene.texture_set = h.tex_set;
  scene.material_set = h.mat_set;
  scene.bone_set = h.bone_set;

  // One ScenePbrObject per mesh, drawn at the skeleton origin; world
  // placement comes entirely from the bone matrices.
  std::vector<ScenePbrObject> objects;
  for (std::size_t i = 0; i < h.meshes.size(); ++i) {
    ScenePbrObject obj{};
    obj.mesh = &h.meshes[i].mesh;
    obj.model = make_translation(0.0f, -0.95f, -4.0f);
    obj.material_index = 0;
    objects.push_back(obj);
  }
  scene.objects = objects;

  // Rest pose.
  {
    std::vector<SceneMatrix> globals;
    compute_globals(doc, globals);
    std::vector<SceneMatrix> joints(15);
    const auto& skin = doc.skins[0];
    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
      const auto& g = globals[skin.joints[j]];
      const auto& ibm = skin.inverse_bind_matrices[j];
      SceneMatrix out{};
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
          float sum = 0.0f;
          for (int k = 0; k < 4; ++k) {
            sum += g[r + 4 * k] * ibm[k + 4 * c];
          }
          out[r + 4 * c] = sum;
        }
      }
      joints[j] = out;
    }
    h.write_bones(joints.data(), 15);
  }
  const auto rest = h.render(scene);

  // Mid-stride pose (t = 0.25 s).
  {
    apply_pose(doc, anim, 0.25f);
    std::vector<SceneMatrix> globals;
    compute_globals(doc, globals);
    std::vector<SceneMatrix> joints(15);
    const auto& skin = doc.skins[0];
    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
      const auto& g = globals[skin.joints[j]];
      const auto& ibm = skin.inverse_bind_matrices[j];
      SceneMatrix out{};
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
          float sum = 0.0f;
          for (int k = 0; k < 4; ++k) {
            sum += g[r + 4 * k] * ibm[k + 4 * c];
          }
          out[r + 4 * c] = sum;
        }
      }
      joints[j] = out;
    }
    h.write_bones(joints.data(), 15);
  }
  const auto stride = h.render(scene);

  h.cleanup();

  ASSERT_TRUE(rest.submitted);
  ASSERT_TRUE(stride.submitted);
  // The figure must actually be visible in both poses (9 meshes * 216 px
  // each would be ~2000; require a conservative on-screen presence).
  EXPECT_GT(rest.non_clear_pixels, 800U) << "rest mannequin did not render";
  EXPECT_GT(stride.non_clear_pixels, 800U) << "striding mannequin did not render";
  // And the animation must have moved it.
  EXPECT_NE(rest.hash, stride.hash)
      << "mid-stride image identical to rest pose - animation had no effect";
}

//! The viewport's exact path: record_pbr_scene with the skinned pipeline
//! (4-set layout) and the bone set bound by the renderer itself. Rest vs
//! mid-stride must render different, non-empty images.
TEST(VulkanHardware, GpuMannequinThroughRecordPbrScene) {
  const MannequinAsset asset = load_asset();
  if (!asset.loaded) GTEST_SKIP() << "mannequin asset not found";

  std::string error;
  auto imported = omnicpp::asset::import_gltf_animation_document(
      asset.json.data(), asset.json.size(),
      reinterpret_cast<const std::uint8_t*>(asset.bin.data()), asset.bin.size(),
      &error);
  ASSERT_TRUE(imported.is_ok()) << error;
  GltfAnimationDocument doc = std::move(imported.value());
  const auto& anim = doc.animations[0];

  MannequinHarness h;
  if (!h.init("mannequin_record_test")) {
    GTEST_SKIP() << "Vulkan unavailable";
  }
  PbrMaterialData mat{};
  mat.base_color_factor = {0.8f, 0.6f, 0.45f, 1.0f};
  mat.metallic_factor = 0;
  mat.roughness_factor = 0.9f;
  h.write_material(mat);
  for (std::size_t i = 0; i < doc.meshes.size(); ++i) {
    ASSERT_TRUE(h.upload_mesh(doc.meshes[i], doc.skin_bindings[i], mat));
  }

  VulkanPbrScene scene{};
  scene.pipeline = h.pipe.pipeline();
  scene.pipeline_layout = h.pipe.pipeline_layout();
  scene.camera.view_projection = make_perspective(45, 1.0f, 0.1f, 100);
  scene.camera_position = {0, 1.0f, 3.2f, 1};
  scene.texture_set = h.tex_set;
  scene.material_set = h.mat_set;
  scene.bone_set = h.bone_set;  // renderer binds this at set 3
  std::vector<ScenePbrObject> objects;
  for (auto& gpu : h.meshes) {
    ScenePbrObject obj{};
    obj.mesh = &gpu.mesh;
    obj.model = make_translation(0.0f, -0.95f, -4.0f);
    obj.material_index = 0;
    objects.push_back(obj);
  }
  scene.objects = objects;

  auto pose_at = [&](float time) {
    apply_pose(doc, anim, time);
    std::vector<SceneMatrix> globals;
    compute_globals(doc, globals);
    std::vector<SceneMatrix> joints(15);
    const auto& skin = doc.skins[0];
    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
      const auto& g = globals[skin.joints[j]];
      const auto& ibm = skin.inverse_bind_matrices[j];
      SceneMatrix out{};
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
          float sum = 0.0f;
          for (int k = 0; k < 4; ++k) {
            sum += g[r + 4 * k] * ibm[k + 4 * c];
          }
          out[r + 4 * c] = sum;
        }
      }
      joints[j] = out;
    }
    h.write_bones(joints.data(), 15);
  };

  // record_pbr_scene records everything except the render-pass begin/end
  // and the dynamic state, which it also sets itself; only the pass must
  // wrap the call. Record a minimal pass around it.
  auto render_through_record = [&]() {
    VkDevice dev = h.ctx.device();
    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(dev, h.qf);
    if (!pr.is_ok()) return omnicpp_test::ReadbackResult{};
    auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        dev, pr.value());
    if (!cr.is_ok()) {
      vkDestroyCommandPool(dev, pr.value(), nullptr);
      return omnicpp_test::ReadbackResult{};
    }
    VkCommandBuffer cb = cr.value();
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(dev, &fi, nullptr, &fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clears[2]{};
    clears[0].color = {{0, 0, 0, 1}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = h.target.render_pass();
    rpb.framebuffer = h.target.framebuffer();
    rpb.renderArea.extent = {256, 256};
    rpb.clearValueCount = 2;
    rpb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    const auto recorded =
        omnicpp::render::VulkanRenderer{}.record_pbr_scene(cb, scene, 256, 256);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    if (!recorded.is_ok()) {
      vkDestroyFence(dev, fence, nullptr);
      vkDestroyCommandPool(dev, pr.value(), nullptr);
      ADD_FAILURE() << "record_pbr_scene failed";
      return omnicpp_test::ReadbackResult{};
    }
    vkResetFences(dev, 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(h.ctx.graphics_queue(), 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pr.value(), nullptr);
    return omnicpp_test::readback_swapchain_image(
        h.ctx.physical_device(), dev, h.ctx.graphics_queue(), h.qf,
        h.target.image(), h.target.format(), 256, 256,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  };

  pose_at(0.0f);
  const auto rest = render_through_record();
  pose_at(0.25f);
  const auto stride = render_through_record();

  h.cleanup();

  ASSERT_TRUE(rest.submitted);
  ASSERT_TRUE(stride.submitted);
  EXPECT_GT(rest.non_clear_pixels, 800U)
      << "record_pbr_scene path: rest mannequin did not render";
  EXPECT_GT(stride.non_clear_pixels, 800U)
      << "record_pbr_scene path: striding mannequin did not render";
  EXPECT_NE(rest.hash, stride.hash)
      << "record_pbr_scene path: animation had no effect";
}

#endif  // WARPLOOM_HAS_VULKAN
