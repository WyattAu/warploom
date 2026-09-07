//! @file test_pbr_scene.cpp
//! @brief GPU end-to-end tests for the PBR (glTF 2.0 metallic-roughness)
//!        scene path: record_pbr_scene with the 160-byte push ABI
//!        (view-projection + model + camera position + material index), the
//!        set-2 material SSBO (one 64-byte PbrMaterialData slot per material)
//!        and the bindless set-1 sampler array shared with the lit path.
//!        Cook-Torrance shading is verified by pixel readback:
//!          (1) a smooth metallic cube produces a bright specular highlight
//!              where a rough diffuse cube produces none (material_index
//!              routing + SSBO stride exercised across two slots),
//!          (2) an emissive factor adds unlit colour on top of a black base,
//!          (3) a baseColorTexture sampled through set 1 tints the surface.
//!        All tests render offscreen under Khronos validation.

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

using omnicpp::render::PbrMaterialData;
using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
using omnicpp::render::ScenePbrObject;
using omnicpp::render::VulkanPbrScene;
using omnicpp::render::scene_identity_matrix;

// ============================================================================
// Matrix helpers (column-major, matching the scene path's SceneMatrix).
// ============================================================================

SceneMatrix make_perspective(float fov_y_degrees, float aspect, float znear,
                             float zfar) {
  SceneMatrix m = scene_identity_matrix();
  const float f = 1.0f / std::tan(fov_y_degrees * 3.14159265f / 360.0f);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (zfar + znear) / (znear - zfar);
  m[11] = -1.0f;
  m[14] = (2.0f * zfar * znear) / (znear - zfar);
  m[15] = 0.0f;
  return m;
}

SceneMatrix make_translation(float x, float y, float z) {
  SceneMatrix m = scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

//! Column-major rotation about an arbitrary unit axis (Rodrigues).
SceneMatrix make_rotation(const std::array<float, 3>& axis, float radians) {
  const float x = axis[0];
  const float y = axis[1];
  const float z = axis[2];
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  const float t = 1.0f - c;
  SceneMatrix m = scene_identity_matrix();
  m[0] = t * x * x + c;
  m[1] = t * x * y + s * z;
  m[2] = t * x * z - s * y;
  m[4] = t * x * y - s * z;
  m[5] = t * y * y + c;
  m[6] = t * y * z + s * x;
  m[8] = t * x * z + s * y;
  m[9] = t * y * z - s * x;
  m[10] = t * z * z + c;
  return m;
}

//! Column-major product a * b.
SceneMatrix multiply(const SceneMatrix& a, const SceneMatrix& b) {
  SceneMatrix out{};
  for (std::size_t col = 0; col < 4; ++col) {
    for (std::size_t row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (std::size_t k = 0; k < 4; ++k) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      out[col * 4 + row] = sum;
    }
  }
  return out;
}

// ============================================================================
// Flat-shaded unit cube in the canonical eleven-float vertex layout
// (position.xyz, color.rgb, normal.xyz, uv.xy): 24 vertices, 36 indices.
// ============================================================================

struct CubeUpload {
  omnicpp::render::Allocation vertex_allocation{};
  omnicpp::render::Allocation index_allocation{};
  SceneMesh mesh{};
};

//! Build a unit cube spanning [-1, 1]^3 with per-face flat normals. Returns
//! the interleaved vertex floats (24 * 11) and 36 uint32 indices.
void build_unit_cube(std::vector<float>& vertices, std::vector<std::uint32_t>& indices) {
  // (normal, corner order CCW when viewed from outside, per-corner uv).
  // Culling is disabled by the PBR pipeline (double-sided), so winding only
  // affects which triangles rasterize — flat per-face normals are what the
  // lighting discriminators rely on.
  struct Face {
    std::array<float, 3> normal;
    std::array<std::array<float, 3>, 4> corners;
  };
  const Face faces[6] = {
      {{0.0f, 0.0f, 1.0f},
       {{{-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
         {-1.0f, 1.0f, 1.0f}}}},
      {{0.0f, 0.0f, -1.0f},
       {{{-1.0f, -1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f},
         {1.0f, -1.0f, -1.0f}}}},
      {{1.0f, 0.0f, 0.0f},
       {{{1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
         {1.0f, 1.0f, -1.0f}}}},
      {{-1.0f, 0.0f, 0.0f},
       {{{-1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
         {-1.0f, 1.0f, 1.0f}}}},
      {{0.0f, 1.0f, 0.0f},
       {{{-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f},
         {-1.0f, 1.0f, 1.0f}}}},
      {{0.0f, -1.0f, 0.0f},
       {{{-1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
         {1.0f, -1.0f, 1.0f}}}},
  };
  const std::array<std::array<float, 2>, 4> uvs = {
      {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
  constexpr std::uint32_t kQuad[6] = {0, 1, 2, 0, 2, 3};

  vertices.clear();
  indices.clear();
  for (const Face& face : faces) {
    const std::uint32_t base = static_cast<std::uint32_t>(vertices.size() / 11U);
    for (std::size_t i = 0; i < 4; ++i) {
      const auto& p = face.corners[i];
      vertices.push_back(p[0]);
      vertices.push_back(p[1]);
      vertices.push_back(p[2]);
      vertices.push_back(1.0f);  // vertex color: white
      vertices.push_back(1.0f);
      vertices.push_back(1.0f);
      vertices.push_back(face.normal[0]);
      vertices.push_back(face.normal[1]);
      vertices.push_back(face.normal[2]);
      vertices.push_back(uvs[i][0]);
      vertices.push_back(uvs[i][1]);
    }
    for (std::uint32_t index : kQuad) {
      indices.push_back(base + index);
    }
  }
}

// ============================================================================
// Texture upload helpers (test-side stand-ins for the app's upload layer).
// ============================================================================

struct SolidTexture {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  omnicpp::render::Allocation allocation{};
};

bool make_solid_texture(VkDevice device, VkPhysicalDevice physical_device,
                        VkQueue queue, std::uint32_t queue_family,
                        omnicpp::render::VulkanMemoryAllocator& allocator,
                        const std::array<std::uint8_t, 4>& rgba,
                        SolidTexture& out) {
  out = {};
  SolidTexture tex{};
  const auto fail = [&]() {
    omnicpp::render::Allocation allocation = tex.allocation;
    if (allocation.is_valid()) allocator.destroy_allocation(allocation);
    if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(device, tex.view, nullptr);
    if (tex.sampler != VK_NULL_HANDLE) vkDestroySampler(device, tex.sampler, nullptr);
    if (tex.image != VK_NULL_HANDLE) vkDestroyImage(device, tex.image, nullptr);
    out = {};
    return false;
  };

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {1U, 1U, 1U};
  image_info.mipLevels = 1U;
  image_info.arrayLayers = 1U;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &image_info, nullptr, &tex.image) != VK_SUCCESS) {
    return fail();
  }
  auto memory = allocator.bind_image(tex.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!memory.is_ok()) return fail();
  tex.allocation = memory.value();

  omnicpp::render::VulkanFrameUploadArena arena;
  if (!arena.initialize(device, physical_device, queue_family,
                        /*frame_count=*/1U, /*bytes_per_frame=*/1U << 20U)
           .is_ok()) {
    return fail();
  }
  if (!arena.begin_frame(0U).is_ok()) return fail();
  auto span = arena.acquire(rgba.size());
  if (!span.is_ok()) return fail();
  std::memcpy(span.value().host_data, rgba.data(), rgba.size());
  arena.record_copy_image_rgba8(span.value(), tex.image, 1U, 1U);
  if (!arena.submit(queue).is_ok()) return fail();
  arena.wait_idle();

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = tex.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
  if (vkCreateImageView(device, &view_info, nullptr, &tex.view) != VK_SUCCESS) {
    return fail();
  }
  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device, &sampler_info, nullptr, &tex.sampler) != VK_SUCCESS) {
    return fail();
  }
  out = tex;
  return true;
}

void destroy_solid_texture(VkDevice device,
                           omnicpp::render::VulkanMemoryAllocator& allocator,
                           SolidTexture& texture) {
  if (texture.allocation.is_valid()) {
    allocator.destroy_allocation(texture.allocation);
  }
  if (texture.view != VK_NULL_HANDLE) vkDestroyImageView(device, texture.view, nullptr);
  if (texture.sampler != VK_NULL_HANDLE) vkDestroySampler(device, texture.sampler, nullptr);
  if (texture.image != VK_NULL_HANDLE) vkDestroyImage(device, texture.image, nullptr);
  texture = {};
}

// ============================================================================
// GPU scaffolding: context, allocator, descriptor manager, bindless set-1
// sampler array (element 0 = opaque white), set-2 material SSBO (two 64-byte
// slots), offscreen target, flat-shaded cube and the PBR pipeline (3 sets,
// 160-byte push ABI).
// ============================================================================

struct PbrSceneHarness {
  omnicpp::render::VulkanContext context;
  omnicpp::render::VulkanMemoryAllocator allocator;
  omnicpp::render::VulkanDescriptorManager descriptors;
  VkDescriptorSetLayout mesh_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout textures_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout material_layout{VK_NULL_HANDLE};
  VkDescriptorSet textures_set{VK_NULL_HANDLE};
  VkDescriptorSet material_set{VK_NULL_HANDLE};
  omnicpp::render::Allocation material_allocation{};
  SolidTexture white_tex;
  CubeUpload cube;
  omnicpp::render::VulkanOffscreenTarget target;
  omnicpp::render::VulkanPipeline pipeline;
  omnicpp::render::VulkanRenderer renderer;
  std::uint32_t queue_family{0};

  bool initialized() const noexcept { return context.is_initialized(); }

  bool initialize(const char* app_name) {
    if (!context.initialize(app_name, true).is_ok()) return false;
    if (!context.has_descriptor_indexing()) {
      context.cleanup();
      return false;
    }
    if (!allocator.initialize(context.device(), context.physical_device()).is_ok()) {
      context.cleanup();
      return false;
    }
    if (!descriptors.initialize(context.device()).is_ok()) return false;
    queue_family =
        static_cast<std::uint32_t>(context.queue_families().graphics_family);

    // Set 0: per-mesh vertex storage (SSBO), shared with the lit path.
    const std::vector<omnicpp::render::ReflectedBinding> mesh_bindings = {
        {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         VK_SHADER_STAGE_VERTEX_BIT}};
    auto mesh_layout_result = descriptors.create_layout(mesh_bindings, 8U);
    if (!mesh_layout_result.is_ok()) return false;
    mesh_layout = mesh_layout_result.value();

    // Set 1: bindless sampler array (element 0 = opaque-white fallback).
    const std::vector<omnicpp::render::ReflectedBinding> textures_bindings = {
        {1U, 0U, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT}};
    auto textures_layout_result = descriptors.create_layout(
        textures_bindings, 1U, /*bindless=*/true);
    if (!textures_layout_result.is_ok()) return false;
    textures_layout = textures_layout_result.value();
    auto textures_set_result = descriptors.allocate_set(textures_layout);
    if (!textures_set_result.is_ok()) return false;
    textures_set = textures_set_result.value();

    // Set 2: material SSBO, two 64-byte slots.
    const std::vector<omnicpp::render::ReflectedBinding> material_bindings = {
        {2U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         VK_SHADER_STAGE_FRAGMENT_BIT}};
    auto material_layout_result = descriptors.create_layout(material_bindings, 8U);
    if (!material_layout_result.is_ok()) return false;
    material_layout = material_layout_result.value();
    auto material_set_result = descriptors.allocate_set(material_layout);
    if (!material_set_result.is_ok()) return false;
    material_set = material_set_result.value();

    auto material_buffer = allocator.create_buffer(
        2U * sizeof(PbrMaterialData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!material_buffer.is_ok()) return false;
    material_allocation = material_buffer.value();
    if (!descriptors
             .write_buffer(material_set, 0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           material_allocation.buffer, 0U, VK_WHOLE_SIZE)
             .is_ok()) {
      return false;
    }

    // Upload the unit cube into host-visible vertex/index buffers.
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    build_unit_cube(vertices, indices);
    const VkDeviceSize vertex_bytes = vertices.size() * sizeof(float);
    const VkDeviceSize index_bytes = indices.size() * sizeof(std::uint32_t);
    auto vb = allocator.create_buffer(
        vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto ib = allocator.create_buffer(
        index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!vb.is_ok() || !ib.is_ok()) return false;
    cube.vertex_allocation = vb.value();
    cube.index_allocation = ib.value();
    std::memcpy(cube.vertex_allocation.mapped, vertices.data(), vertex_bytes);
    std::memcpy(cube.index_allocation.mapped, indices.data(), index_bytes);

    auto set = descriptors.allocate_set(mesh_layout);
    if (!set.is_ok()) return false;
    if (!descriptors
             .write_buffer(set.value(), 0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                           cube.vertex_allocation.buffer, 0U, VK_WHOLE_SIZE)
             .is_ok()) {
      return false;
    }
    cube.mesh.vertex_buffer = cube.vertex_allocation.buffer;
    cube.mesh.index_buffer = cube.index_allocation.buffer;
    cube.mesh.index_count = static_cast<std::uint32_t>(indices.size());
    cube.mesh.descriptor_set = set.value();

    if (!make_solid_texture(context.device(), context.physical_device(),
                            context.graphics_queue(), queue_family, allocator,
                            std::array<std::uint8_t, 4>{255U, 255U, 255U, 255U},
                            white_tex)) {
      return false;
    }
    if (!descriptors
             .write_image(textures_set, 0U,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          white_tex.sampler, white_tex.view,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
             .is_ok()) {
      return false;
    }

    if (!target.create(context.device(), context.physical_device(),
                       VK_FORMAT_B8G8R8A8_UNORM, 256U, 256U, &allocator)
             .is_ok() ||
        !target.create_depth(context.device(), context.physical_device(),
                             VK_FORMAT_D32_SFLOAT)
             .is_ok() ||
        !target.create_render_pass(context.device()).is_ok() ||
        !target.create_framebuffer(context.device()).is_ok()) {
      return false;
    }

    const std::string shader_dir = OMNICPP_TEST_SHADER_DIR;
    if (!pipeline
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/pbr_scene.vert.spv", "vertex")
             .is_ok() ||
        !pipeline
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/pbr_scene.frag.spv",
                                     "fragment")
             .is_ok()) {
      return false;
    }
    const VkDescriptorSetLayout set_layouts[3] = {mesh_layout, textures_layout,
                                                  material_layout};
    const VkPushConstantRange push_range{
        static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                        VK_SHADER_STAGE_FRAGMENT_BIT),
        0U, 160U};
    if (!pipeline
             .create_pipeline_layout(context.device(), set_layouts, 3U,
                                     &push_range)
             .is_ok() ||
        !pipeline
             .create_graphics_pipeline(context.device(), target.render_pass(),
                                       target.format(), pipeline.pipeline_layout(),
                                       true, true, false)
             .is_ok()) {
      return false;
    }
    return true;
  }

  //! Overwrite the two material SSBO slots from host memory.
  void write_materials(const PbrMaterialData* slots, std::size_t count) {
    const std::size_t bytes = count * sizeof(PbrMaterialData);
    std::memcpy(material_allocation.mapped, slots, bytes);
  }

  //! Bind one texture into the bindless set-1 array at `index` (>= 1).
  bool bind_texture(const SolidTexture& texture, std::uint32_t index) {
    return descriptors
        .write_image(textures_set, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     texture.sampler, texture.view,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, index)
        .is_ok();
  }

  //! Render one PBR scene snapshot and read the 256x256 image back.
  omnicpp_test::ReadbackResult render(const VulkanPbrScene& scene) {
    auto pool_result =
        omnicpp::render::VulkanRenderer::create_command_pool(context.device(),
                                                             queue_family);
    if (!pool_result.is_ok()) return {};
    auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        context.device(), pool_result.value());
    if (!cb_result.is_ok()) {
      vkDestroyCommandPool(context.device(), pool_result.value(), nullptr);
      return {};
    }
    const VkCommandBuffer cb = cb_result.value();
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(context.device(), &fence_info, nullptr, &fence) !=
        VK_SUCCESS) {
      vkDestroyCommandPool(context.device(), pool_result.value(), nullptr);
      return {};
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const bool began = vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS;
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {256U, 256U};
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0U};
    rb.clearValueCount = 2U;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const bool recorded =
        began && renderer.record_pbr_scene(cb, scene, 256U, 256U).is_ok();
    vkCmdEndRenderPass(cb);
    const bool ended = vkEndCommandBuffer(cb) == VK_SUCCESS;
    (void)vkResetFences(context.device(), 1U, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &cb;
    const bool submitted =
        recorded && ended &&
        vkQueueSubmit(context.graphics_queue(), 1U, &submit, fence) ==
            VK_SUCCESS &&
        vkWaitForFences(context.device(), 1U, &fence, VK_TRUE, UINT64_MAX) ==
            VK_SUCCESS;
    omnicpp_test::ReadbackResult result{};
    if (submitted) {
      result = omnicpp_test::readback_swapchain_image(
          context.physical_device(), context.device(), context.graphics_queue(),
          queue_family, target.image(), target.format(), 256U, 256U,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    vkDestroyFence(context.device(), fence, nullptr);
    vkDestroyCommandPool(context.device(), pool_result.value(), nullptr);
    return result;
  }

  void cleanup() {
    pipeline.cleanup(context.device());
    target.cleanup(context.device());
    if (white_tex.sampler != VK_NULL_HANDLE) {
      destroy_solid_texture(context.device(), allocator, white_tex);
    }
    if (cube.vertex_allocation.is_valid()) {
      allocator.destroy_allocation(cube.vertex_allocation);
    }
    if (cube.index_allocation.is_valid()) {
      allocator.destroy_allocation(cube.index_allocation);
    }
    if (material_allocation.is_valid()) {
      allocator.destroy_allocation(material_allocation);
    }
    descriptors.cleanup();
    allocator.cleanup();
    context.cleanup();
  }
};

//! Base scene factory: camera 4.5 units in front of the cube (identity view,
//! origin eye), PBR pipeline and the shared set-1/set-2 descriptor sets.
VulkanPbrScene make_base_scene(PbrSceneHarness& h) {
  VulkanPbrScene scene;
  scene.pipeline = h.pipeline.pipeline();
  scene.pipeline_layout = h.pipeline.pipeline_layout();
  scene.camera.view_projection =
      make_perspective(45.0f, 1.0f, 0.1f, 30.0f);
  // Identity view: eye at the world origin looking down -Z. Push constant
  // camera_position must match (record_pbr_scene pushes scene.camera_position).
  scene.camera_position = {0.0f, 0.0f, 0.0f, 1.0f};
  scene.texture_set = h.textures_set;
  scene.material_set = h.material_set;
  return scene;
}

}  // namespace

// ============================================================================
// GPU tests (defined in the anonymous namespace above)
// ============================================================================

//! Smooth metal vs. rough diffuse: identical gray base colour and pose, same
//! light. The Cook-Torrance specular term concentrates a bright (near-white)
//! highlight on the low-roughness cube and none on the rough one. Two SSBO
//! slots are populated (0 = metallic, 1 = rough) and selected by
//! material_index, which also proves SSBO slot addressing and the 64-byte
//! stride: a stride or offset bug reads the neighbouring slot's factors and
//! breaks the expected shading.
TEST(VulkanHardware, PbrMetallicRoughnessDiscrimination) {
  PbrSceneHarness h;
  if (!h.initialize("pbr_metallic_roughness")) {
    GTEST_SKIP() << "Vulkan context or descriptor indexing unavailable";
  }

  PbrMaterialData materials[2]{};
  // Slot 0: smooth metal. F0 = albedo (0.8 gray); roughness 0.35 keeps the
  // GGX lobe wide enough to cover a usable fraction of the front face while
  // still concentrating a bright specular peak.
  materials[0].base_color_factor = {0.8f, 0.8f, 0.8f, 1.0f};
  materials[0].metallic_factor = 1.0f;
  materials[0].roughness_factor = 0.35f;
  // Slot 1: rough dielectric. Diffuse-only gray, deliberately dim.
  materials[1].base_color_factor = {0.8f, 0.8f, 0.8f, 1.0f};
  materials[1].metallic_factor = 0.0f;
  materials[1].roughness_factor = 0.9f;
  h.write_materials(materials, 2U);

  // Face the +Z front face toward the half-vector of (view, key light) so the
  // mirror direction lands on a visible face. Key light is
  // normalize(0.3, 0.65, 0.7); with the eye at the origin looking down -Z the
  // centre view vector is +Z. Half vector H = normalize(V + L); the cube's
  // +Z face is rotated to align with it.
  const float light[3] = {0.3f, 0.65f, 0.7f};
  float light_len = 0.0f;
  for (float c : light) light_len += c * c;
  light_len = std::sqrt(light_len);
  const std::array<float, 3> l = {light[0] / light_len, light[1] / light_len,
                                  light[2] / light_len};
  const std::array<float, 3> v = {0.0f, 0.0f, 1.0f};
  std::array<float, 3> hvec = {l[0] + v[0], l[1] + v[1], l[2] + v[2]};
  float hlen = std::sqrt(hvec[0] * hvec[0] + hvec[1] * hvec[1] +
                         hvec[2] * hvec[2]);
  hvec = {hvec[0] / hlen, hvec[1] / hlen, hvec[2] / hlen};

  std::array<float, 3> axis = {0.0f, 0.0f, 1.0f};
  float axis_len = 0.0f;
  for (float c : axis) axis_len += c * c;
  // Rotate +Z onto H: axis = cross(Z, H), angle = acos(dot(Z, H)).
  axis = {v[1] * hvec[2] - v[2] * hvec[1], v[2] * hvec[0] - v[0] * hvec[2],
          v[0] * hvec[1] - v[1] * hvec[0]};
  axis_len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  float dot_zh = v[0] * hvec[0] + v[1] * hvec[1] + v[2] * hvec[2];
  dot_zh = std::clamp(dot_zh, -1.0f, 1.0f);
  const SceneMatrix rotation =
      axis_len > 1e-6f
          ? make_rotation({axis[0] / axis_len, axis[1] / axis_len, axis[2] / axis_len},
                          std::acos(dot_zh))
          : scene_identity_matrix();

  VulkanPbrScene scene = make_base_scene(h);
  const SceneMatrix translation = make_translation(0.0f, 0.0f, -4.5f);
  const SceneMatrix model = multiply(translation, rotation);

  ScenePbrObject metallic;
  metallic.mesh = &h.cube.mesh;
  metallic.model = model;
  metallic.material_index = 0U;
  scene.objects.push_back(metallic);
  const omnicpp_test::ReadbackResult metallic_result = h.render(scene);

  ScenePbrObject rough;
  rough.mesh = &h.cube.mesh;
  rough.model = translation;
  rough.material_index = 1U;
  scene.objects.clear();
  scene.objects.push_back(rough);
  const omnicpp_test::ReadbackResult rough_result = h.render(scene);

  h.cleanup();

  ASSERT_TRUE(metallic_result.submitted);
  ASSERT_TRUE(rough_result.submitted);
  // Both cubes must actually be visible (non-clear) — a layout/descriptor
  // failure typically renders nothing.
  EXPECT_GT(rough_result.non_clear_pixels, 5000U)
      << "rough cube did not render";
  EXPECT_GT(metallic_result.non_clear_pixels, 5000U)
      << "metallic cube did not render";
  // Rough dielectric: diffuse gray cannot reach the 200/200/200 threshold.
  EXPECT_EQ(rough_result.bright_pixels, 0U)
      << "rough cube produced an unexpected specular highlight "
         "(peak_luma=" << rough_result.peak_luma << ")";
  // Smooth metal: the concentrated GGX peak saturates a visible patch.
  EXPECT_GT(metallic_result.bright_pixels, 100U)
      << "metallic cube produced no bright specular highlight "
         "(bright=" << metallic_result.bright_pixels
         << ", peak_luma=" << metallic_result.peak_luma << ")";
  EXPECT_GT(metallic_result.bright_pixels, rough_result.bright_pixels + 100U);
  EXPECT_GT(metallic_result.peak_luma, rough_result.peak_luma + 100U);
}

//! Emissive materials add unlit colour: base factor black (so ambient and
//! diffuse vanish) with a pure-red emissive factor must still produce a red
//! cube. Also selects slot 0 of a two-slot SSBO whose slot 1 is poisoned with
//! garbage — correct addressing reads the red emissive data and nothing else.
TEST(VulkanHardware, PbrEmissiveAddsUnlitColor) {
  PbrSceneHarness h;
  if (!h.initialize("pbr_emissive")) {
    GTEST_SKIP() << "Vulkan context or descriptor indexing unavailable";
  }

  alignas(PbrMaterialData) std::array<std::uint8_t, sizeof(PbrMaterialData) * 2U> bytes{};
  bytes.fill(0xAB);  // poison every byte (slot 1 and padding)
  PbrMaterialData& emissive = *reinterpret_cast<PbrMaterialData*>(bytes.data());
  emissive = PbrMaterialData{};
  emissive.base_color_factor = {0.0f, 0.0f, 0.0f, 1.0f};
  emissive.emissive_factor = {1.0f, 0.0f, 0.0f};
  emissive.metallic_factor = 0.0f;
  emissive.roughness_factor = 1.0f;
  h.write_materials(reinterpret_cast<const PbrMaterialData*>(bytes.data()), 2U);

  VulkanPbrScene scene = make_base_scene(h);
  ScenePbrObject object;
  object.mesh = &h.cube.mesh;
  object.model = make_translation(0.0f, 0.0f, -4.5f);
  object.material_index = 0U;
  scene.objects.push_back(object);
  const omnicpp_test::ReadbackResult result = h.render(scene);
  h.cleanup();

  ASSERT_TRUE(result.submitted);
  EXPECT_GT(result.non_clear_pixels, 5000U) << "emissive cube did not render";
  EXPECT_GT(result.red_dominant_pixels, 2000U)
      << "emissive cube is not red-dominant (center_pixel=" << std::hex
      << result.center_pixel << std::dec << ")";
  EXPECT_EQ(result.green_dominant_pixels, 0U);
  EXPECT_EQ(result.blue_dominant_pixels, 0U);
  const std::uint32_t r = result.center_pixel & 0xFFU;
  const std::uint32_t g = (result.center_pixel >> 8U) & 0xFFU;
  const std::uint32_t b = (result.center_pixel >> 16U) & 0xFFU;
  EXPECT_GT(r, 150U) << "emissive red not bright enough at centre";
  EXPECT_LT(g, 30U);
  EXPECT_LT(b, 30U);
}

//! baseColorTexture through the PBR path: a 1x1 red texture bound at set-1
//! element 1, referenced by the material slot's albedo_index, tints the lit
//! cube red (diffuse red, no emissive). Proves set-1 sampling under the PBR
//! ABI and that texture indices travel inside the material SSBO rather than
//! the push block.
TEST(VulkanHardware, PbrAlbedoTextureTintsLitCube) {
  PbrSceneHarness h;
  if (!h.initialize("pbr_albedo_texture")) {
    GTEST_SKIP() << "Vulkan context or descriptor indexing unavailable";
  }

  SolidTexture red_tex;
  if (!make_solid_texture(h.context.device(), h.context.physical_device(),
                          h.context.graphics_queue(), h.queue_family,
                          h.allocator,
                          std::array<std::uint8_t, 4>{255U, 0U, 0U, 255U},
                          red_tex)) {
    h.cleanup();
    GTEST_SKIP() << "texture upload failed";
  }
  if (!h.bind_texture(red_tex, 1U)) {
    destroy_solid_texture(h.context.device(), h.allocator, red_tex);
    h.cleanup();
    GTEST_SKIP() << "bindless write failed";
  }

  PbrMaterialData materials[2]{};
  materials[0].base_color_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  materials[0].metallic_factor = 0.0f;
  materials[0].roughness_factor = 0.9f;
  materials[0].albedo_index = 1U;  // the red 1x1 texture
  materials[1] = materials[0];
  materials[1].albedo_index = 0U;  // white fallback — must stay white-ish
  h.write_materials(materials, 2U);

  VulkanPbrScene scene = make_base_scene(h);
  ScenePbrObject object;
  object.mesh = &h.cube.mesh;
  object.model = make_translation(0.0f, 0.0f, -4.5f);
  object.material_index = 0U;
  scene.objects.push_back(object);
  const omnicpp_test::ReadbackResult result = h.render(scene);
  destroy_solid_texture(h.context.device(), h.allocator, red_tex);
  h.cleanup();

  ASSERT_TRUE(result.submitted);
  EXPECT_GT(result.non_clear_pixels, 5000U) << "textured cube did not render";
  EXPECT_GT(result.red_dominant_pixels, 2000U)
      << "albedo texture did not tint the cube red";
  // Lit red (not emissive): channel balance must show real diffuse, and the
  // white fallback material (slot 1) is not used here, so only red dominates.
  EXPECT_EQ(result.green_dominant_pixels, 0U);
  EXPECT_EQ(result.blue_dominant_pixels, 0U);
  const std::uint32_t r = result.center_pixel & 0xFFU;
  const std::uint32_t g = (result.center_pixel >> 8U) & 0xFFU;
  EXPECT_GT(r, 90U);
  EXPECT_LT(g, 60U);
}

#endif  // OMNICPP_HAS_VULKAN
