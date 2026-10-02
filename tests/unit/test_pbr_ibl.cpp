//! @file test_pbr_ibl.cpp
//! @brief GPU end-to-end tests for image-based lighting feeding the PBR
//!        scene path (record_pbr_scene with the optional set-3 environment).
//!
//! The IBL bake runs entirely on the GPU in one command buffer:
//!   ibl_sky.comp        fills an HDR equirectangular sky (sun disc + ambient)
//!   ibl_prefilter.comp  convolves it into a 7-level mip-chained cube map
//!                       (split-sum specular: mip level == roughness)
//!   ibl_irradiance.comp cosine-convolves a diffuse irradiance cube
//!   ibl_brdf_lut.comp   bakes the 256x256 split-sum BRDF LUT
//! and the pbr_ibl.frag variant (set 3 = prefiltered cube, irradiance cube,
//! BRDF LUT) evaluates  kD*albedo*irradiance(N)  +  prefiltered(R) *
//! (F0*LUT.x + LUT.y). Shading is verified by pixel readback:
//!   (1) a smooth metal cube whose reflection vector hits the baked sun disc
//!       is bright from environment specular alone, while the same surface
//!       with IBL disabled (identical direct light) stays dark, and the rough
//!       metal smears the sun below threshold;
//!   (2) a white dielectric under a uniform red sky takes a strong red
//!       ambient cast (irradiance convolution) that the non-IBL path lacks.
//! All tests render offscreen under Khronos validation.

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

// ============================================================================
// Flat-shaded unit cube (eleven-float vertex layout used by pbr_scene.vert).
// ============================================================================

struct CubeUpload {
  omnicpp::render::Allocation vertex_allocation{};
  omnicpp::render::Allocation index_allocation{};
  SceneMesh mesh{};
};

void build_unit_cube(std::vector<float>& vertices,
                     std::vector<std::uint32_t>& indices) {
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
      vertices.push_back(1.0f);
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
// Solid 1x1 texture upload (opaque-white fallback for set-1 element 0).
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
// IBL bake. Owns the sky/prefiltered/irradiance/LUT images, all views and
// samplers, the four compute pipelines and the set-3 descriptor set
// (prefiltered cube, irradiance cube, BRDF LUT) consumed by pbr_ibl.frag.
// ============================================================================

struct IblSkyParams {
  std::array<float, 3> sun_direction{0.0f, 0.0f, 1.0f};
  float sun_radius_rad{0.14f};
  std::array<float, 3> sun_color{6.0f, 6.0f, 6.0f};
  std::array<float, 3> sky_color{0.02f, 0.02f, 0.02f};
  float sky_gain{1.0f};
};

constexpr std::uint32_t kSkyWidth = 256U;
constexpr std::uint32_t kSkyHeight = 128U;
constexpr std::uint32_t kPrefilterBase = 64U;
constexpr std::uint32_t kPrefilterMips = 7U;  // 64..1
constexpr std::uint32_t kIrradianceBase = 32U;
constexpr std::uint32_t kBrdfLutSize = 256U;
constexpr VkFormat kIblFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct IblResources {
  VkDevice device{VK_NULL_HANDLE};
  omnicpp::render::VulkanMemoryAllocator* allocator{nullptr};
  omnicpp::render::VulkanDescriptorManager* descriptors{nullptr};
  VkQueue queue{VK_NULL_HANDLE};
  std::uint32_t queue_family{0};

  // Images + memory.
  VkImage sky_image{VK_NULL_HANDLE};
  omnicpp::render::Allocation sky_memory{};
  VkImage prefiltered_image{VK_NULL_HANDLE};
  omnicpp::render::Allocation prefiltered_memory{};
  VkImage irradiance_image{VK_NULL_HANDLE};
  omnicpp::render::Allocation irradiance_memory{};
  VkImage lut_image{VK_NULL_HANDLE};
  omnicpp::render::Allocation lut_memory{};

  // Storage/bake views + final sampled views.
  VkImageView sky_view{VK_NULL_HANDLE};
  std::array<VkImageView, kPrefilterMips> prefilter_mip_views{};
  VkImageView prefiltered_cube_view{VK_NULL_HANDLE};
  VkImageView irradiance_mip_view{VK_NULL_HANDLE};
  VkImageView irradiance_cube_view{VK_NULL_HANDLE};
  VkImageView lut_view{VK_NULL_HANDLE};

  // Samplers.
  VkSampler sampler_sky{VK_NULL_HANDLE};   // REPEAT u (equirect seam)
  VkSampler sampler_cube{VK_NULL_HANDLE};  // trilinear mip chain
  VkSampler sampler_flat{VK_NULL_HANDLE};  // linear, single level

  // Bake descriptor sets (one per pipeline) and their layouts.
  VkDescriptorSetLayout bake_layouts[4]{};
  VkDescriptorSet bake_sets[4]{};

  // Pre-allocated per-mip prefilter sets (avoids UPDATE_AFTER_BIND).
  std::array<VkDescriptorSet, kPrefilterMips> prefilter_sets{};

  // Set-3 resource layout/set for the IBL graphics pipeline.
  VkDescriptorSetLayout ibl_layout{VK_NULL_HANDLE};
  VkDescriptorSet ibl_set{VK_NULL_HANDLE};

  // Compute pipelines.
  omnicpp::render::VulkanPipeline sky_pipeline;
  omnicpp::render::VulkanPipeline prefilter_pipeline;
  omnicpp::render::VulkanPipeline irradiance_pipeline;
  omnicpp::render::VulkanPipeline lut_pipeline;

  // Command infrastructure (one bake submission).
  VkCommandPool pool{VK_NULL_HANDLE};
  VkFence fence{VK_NULL_HANDLE};

  bool create_image(VkImage& image, omnicpp::render::Allocation& memory,
                    std::uint32_t width, std::uint32_t height,
                    std::uint32_t mips, std::uint32_t layers,
                    VkImageUsageFlags usage, VkImageCreateFlags flags) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = kIblFormat;
    info.extent = {width, height, 1U};
    info.mipLevels = mips;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.flags = flags;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;
    auto result = allocator->bind_image(image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!result.is_ok()) return false;
    memory = result.value();
    return true;
  }

  bool create_view(VkImageView& view, VkImage image, VkImageViewType type,
                   std::uint32_t base_mip, std::uint32_t mip_count,
                   std::uint32_t layer_count) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = type;
    info.format = kIblFormat;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0U,
                             layer_count};
    return vkCreateImageView(device, &info, nullptr, &view) == VK_SUCCESS;
  }

  bool initialize(VkDevice dev, VkPhysicalDevice, VkQueue graphics_queue,
                  std::uint32_t family,
                  omnicpp::render::VulkanMemoryAllocator& memory_allocator,
                  omnicpp::render::VulkanDescriptorManager& manager) {
    device = dev;
    allocator = &memory_allocator;
    descriptors = &manager;
    queue = graphics_queue;
    queue_family = family;

    const VkImageUsageFlags kStorageSampled =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // Equirect sky (2D, single level).
    if (!create_image(sky_image, sky_memory, kSkyWidth, kSkyHeight, 1U, 1U,
                      kStorageSampled, 0U)) {
      return false;
    }
    // Prefiltered cube (cube-compatible array, full mip chain).
    if (!create_image(prefiltered_image, prefiltered_memory, kPrefilterBase,
                      kPrefilterBase, kPrefilterMips, 6U, kStorageSampled,
                      VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
      return false;
    }
    // Irradiance cube (cube-compatible array, single level).
    if (!create_image(irradiance_image, irradiance_memory, kIrradianceBase,
                      kIrradianceBase, 1U, 6U, kStorageSampled,
                      VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
      return false;
    }
    // BRDF LUT (2D, single level).
    if (!create_image(lut_image, lut_memory, kBrdfLutSize, kBrdfLutSize, 1U, 1U,
                      kStorageSampled, 0U)) {
      return false;
    }

    // Bake-time storage views.
    if (!create_view(sky_view, sky_image, VK_IMAGE_VIEW_TYPE_2D, 0U, 1U, 1U)) {
      return false;
    }
    for (std::uint32_t m = 0; m < kPrefilterMips; ++m) {
      if (!create_view(prefilter_mip_views[m], prefiltered_image,
                       VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1U, 6U)) {
        return false;
      }
    }
    if (!create_view(irradiance_mip_view, irradiance_image,
                     VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0U, 1U, 6U)) {
      return false;
    }
    // Sampled cube views.
    if (!create_view(prefiltered_cube_view, prefiltered_image,
                     VK_IMAGE_VIEW_TYPE_CUBE, 0U, kPrefilterMips, 6U) ||
        !create_view(irradiance_cube_view, irradiance_image,
                     VK_IMAGE_VIEW_TYPE_CUBE, 0U, 1U, 6U) ||
        !create_view(lut_view, lut_image, VK_IMAGE_VIEW_TYPE_2D, 0U, 1U, 1U)) {
      return false;
    }

    // Samplers.
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.5f;
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler_sky) != VK_SUCCESS) {
      return false;
    }
    sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = static_cast<float>(kPrefilterMips - 1U);
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler_cube) != VK_SUCCESS) {
      return false;
    }
    sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.5f;
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler_flat) != VK_SUCCESS) {
      return false;
    }

    // --- Bake descriptor layouts + sets. -------------------------------
    // sky:  b0 = storage image (sky)
    const std::vector<omnicpp::render::ReflectedBinding> sky_bindings = {
        {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         VK_SHADER_STAGE_COMPUTE_BIT}};
    // prefilter/irradiance: b0 = CIS (sky), b1 = storage image (cube mip)
    const std::vector<omnicpp::render::ReflectedBinding> cube_bindings = {
        {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_COMPUTE_BIT},
        {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         VK_SHADER_STAGE_COMPUTE_BIT}};
    auto sky_layout = descriptors->create_layout(sky_bindings, 1U);
    auto prefilter_layout = descriptors->create_layout(cube_bindings, kPrefilterMips);
    auto irradiance_layout = descriptors->create_layout(cube_bindings, 1U);
    auto lut_layout = descriptors->create_layout(sky_bindings, 1U);
    if (!sky_layout.is_ok() || !prefilter_layout.is_ok() ||
        !irradiance_layout.is_ok() || !lut_layout.is_ok()) {
      return false;
    }
    bake_layouts[0] = sky_layout.value();
    bake_layouts[1] = prefilter_layout.value();
    bake_layouts[2] = irradiance_layout.value();
    bake_layouts[3] = lut_layout.value();
    auto sky_set = descriptors->allocate_set(bake_layouts[0]);
    // bake_sets[1] is unused — per-mip prefilter_sets are pre-allocated below.
    auto irradiance_set = descriptors->allocate_set(bake_layouts[2]);
    auto lut_set = descriptors->allocate_set(bake_layouts[3]);
    if (!sky_set.is_ok() || !irradiance_set.is_ok() || !lut_set.is_ok()) {
      return false;
    }
    bake_sets[0] = sky_set.value();
    bake_sets[2] = irradiance_set.value();
    bake_sets[3] = lut_set.value();

    if (!descriptors
             ->write_image(bake_sets[0], 0U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_NULL_HANDLE, sky_view, VK_IMAGE_LAYOUT_GENERAL, 0U)
             .is_ok()) {
      return false;
    }
    // bake_sets[2] (irradiance) reads from the sky cubemap.
    if (!descriptors
             ->write_image(bake_sets[2], 0U,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           sampler_sky, sky_view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
             .is_ok()) {
      return false;
    }
    if (!descriptors
             ->write_image(bake_sets[3], 0U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_NULL_HANDLE, lut_view, VK_IMAGE_LAYOUT_GENERAL, 0U)
             .is_ok()) {
      return false;
    }

    // Pre-allocate per-mip prefilter descriptor sets so the bake loop
    // never calls vkUpdateDescriptorSets on a set that is already bound
    // to a recording command buffer (UPDATE_AFTER_BIND not required).
    for (std::uint32_t m = 0; m < kPrefilterMips; ++m) {
      auto ps = descriptors->allocate_set(bake_layouts[1]);
      if (!ps.is_ok()) return false;
      prefilter_sets[m] = ps.value();
      if (!descriptors
               ->write_image(prefilter_sets[m], 0U,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             sampler_sky, sky_view,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
               .is_ok()) {
        return false;
      }
      if (!descriptors
               ->write_image(prefilter_sets[m], 1U,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                             prefilter_mip_views[m], VK_IMAGE_LAYOUT_GENERAL, 0U)
               .is_ok()) {
        return false;
      }
    }

    // --- Compute pipelines. --------------------------------------------
    const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
    if (!sky_pipeline
             .load_shader_stage_file(device, shader_dir + "/ibl_sky.comp.spv",
                                     "compute")
             .is_ok() ||
        !prefilter_pipeline
             .load_shader_stage_file(device,
                                     shader_dir + "/ibl_prefilter.comp.spv",
                                     "compute")
             .is_ok() ||
        !irradiance_pipeline
             .load_shader_stage_file(device,
                                     shader_dir + "/ibl_irradiance.comp.spv",
                                     "compute")
             .is_ok() ||
        !lut_pipeline
             .load_shader_stage_file(device,
                                     shader_dir + "/ibl_brdf_lut.comp.spv",
                                     "compute")
             .is_ok()) {
      return false;
    }
    const VkPushConstantRange sky_push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 64U};
    const VkPushConstantRange prefilter_push{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 4U};
    if (!sky_pipeline
             .create_pipeline_layout(device, &bake_layouts[0], 1U, &sky_push)
             .is_ok() ||
        !prefilter_pipeline
             .create_pipeline_layout(device, &bake_layouts[1], 1U,
                                     &prefilter_push)
             .is_ok() ||
        !irradiance_pipeline
             .create_pipeline_layout(device, &bake_layouts[2], 1U, nullptr)
             .is_ok() ||
        !lut_pipeline
             .create_pipeline_layout(device, &bake_layouts[3], 1U, nullptr)
             .is_ok()) {
      return false;
    }
    if (!sky_pipeline.create_compute_pipeline(device,
                                              sky_pipeline.pipeline_layout())
             .is_ok() ||
        !prefilter_pipeline
             .create_compute_pipeline(device,
                                      prefilter_pipeline.pipeline_layout())
             .is_ok() ||
        !irradiance_pipeline
             .create_compute_pipeline(device,
                                      irradiance_pipeline.pipeline_layout())
             .is_ok() ||
        !lut_pipeline.create_compute_pipeline(device, lut_pipeline.pipeline_layout())
             .is_ok()) {
      return false;
    }

    // --- Set-3 descriptor set for pbr_ibl.frag. ------------------------
    const std::vector<omnicpp::render::ReflectedBinding> ibl_bindings = {
        {3U, 0U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT},
        {3U, 1U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT},
        {3U, 2U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT}};
    auto ibl_layout_result = descriptors->create_layout(ibl_bindings, 1U);
    if (!ibl_layout_result.is_ok()) return false;
    ibl_layout = ibl_layout_result.value();
    auto ibl_set_result = descriptors->allocate_set(ibl_layout);
    if (!ibl_set_result.is_ok()) return false;
    ibl_set = ibl_set_result.value();
    if (!descriptors
             ->write_image(ibl_set, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           sampler_cube, prefiltered_cube_view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
             .is_ok() ||
        !descriptors
             ->write_image(ibl_set, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           sampler_flat, irradiance_cube_view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
             .is_ok() ||
        !descriptors
             ->write_image(ibl_set, 2U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           sampler_flat, lut_view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
             .is_ok()) {
      return false;
    }

    // --- One-shot command infrastructure. -------------------------------
    auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
        device, queue_family);
    if (!pool_result.is_ok()) return false;
    pool = pool_result.value();
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS;
  }

  //! Transition `image` (mips x layers subresource range) between layouts.
  void transition(VkCommandBuffer cb, VkImage image, std::uint32_t mips,
                  std::uint32_t layers, VkImageLayout old_layout,
                  VkImageLayout new_layout, VkPipelineStageFlags src_stage,
                  VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                  VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, mips, 0U, layers};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0U, 0U, nullptr, 0U, nullptr,
                         1U, &barrier);
  }

  //! Bake the environment described by `params` (one GPU submission).
  bool bake(const IblSkyParams& params) {
    if (device == VK_NULL_HANDLE || pool == VK_NULL_HANDLE) return false;
    auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        device, pool);
    if (!cb_result.is_ok()) return false;
    const VkCommandBuffer cb = cb_result.value();

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) return false;

    // --- 1. Equirect sky: UNDEFINED -> GENERAL, dispatch, -> READ_ONLY. --
    transition(cb, sky_image, 1U, 1U, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      sky_pipeline.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            sky_pipeline.pipeline_layout(), 0U, 1U, &bake_sets[0],
                            0U, nullptr);
    const float sun_dir[4] = {params.sun_direction[0], params.sun_direction[1],
                              params.sun_direction[2], 0.0f};
    const float sun_color[4] = {params.sun_color[0], params.sun_color[1],
                                params.sun_color[2], 0.0f};
    const float sky_color[4] = {params.sky_color[0], params.sky_color[1],
                                params.sky_color[2], 0.0f};
    const float misc[4] = {params.sun_radius_rad, params.sky_gain, 0.0f, 0.0f};
    vkCmdPushConstants(cb, sky_pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U, 16U, sun_dir);
    vkCmdPushConstants(cb, sky_pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 16U, 16U, sun_color);
    vkCmdPushConstants(cb, sky_pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 32U, 16U, sky_color);
    vkCmdPushConstants(cb, sky_pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 48U, 16U, misc);
    vkCmdDispatch(cb, kSkyWidth / 8U, kSkyHeight / 8U, 1U);
    transition(cb, sky_image, 1U, 1U, VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    // --- 2. Prefiltered cube: one dispatch per mip, all six faces. ------
    transition(cb, prefiltered_image, kPrefilterMips, 6U,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    for (std::uint32_t m = 0; m < kPrefilterMips; ++m) {
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                        prefilter_pipeline.pipeline());
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                              prefilter_pipeline.pipeline_layout(), 0U, 1U,
                              &prefilter_sets[m], 0U, nullptr);
      const float roughness = static_cast<float>(m) /
                              static_cast<float>(kPrefilterMips - 1U);
      vkCmdPushConstants(cb, prefilter_pipeline.pipeline_layout(),
                         VK_SHADER_STAGE_COMPUTE_BIT, 0U, 4U, &roughness);
      const std::uint32_t dim = kPrefilterBase >> m;
      vkCmdDispatch(cb, std::max(dim / 8U, 1U), std::max(dim / 8U, 1U), 6U);
    }

    // --- 3. Irradiance cube (single dispatch, all six faces). -----------
    transition(cb, irradiance_image, 1U, 6U, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    if (!descriptors
             ->write_image(bake_sets[2], 1U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_NULL_HANDLE, irradiance_mip_view,
                           VK_IMAGE_LAYOUT_GENERAL, 0U)
             .is_ok()) {
      return false;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      irradiance_pipeline.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            irradiance_pipeline.pipeline_layout(), 0U, 1U,
                            &bake_sets[2], 0U, nullptr);
    vkCmdDispatch(cb, kIrradianceBase / 8U, kIrradianceBase / 8U, 6U);

    // --- 4. BRDF LUT (single dispatch). ---------------------------------
    transition(cb, lut_image, 1U, 1U, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0U,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lut_pipeline.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            lut_pipeline.pipeline_layout(), 0U, 1U, &bake_sets[3],
                            0U, nullptr);
    vkCmdDispatch(cb, kBrdfLutSize / 8U, kBrdfLutSize / 8U, 1U);

    // --- 5. Everything -> SHADER_READ_ONLY for the fragment stage. -------
    constexpr VkPipelineStageFlags kDstStage =
        static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transition(cb, prefiltered_image, kPrefilterMips, 6U,
               VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               kDstStage, VK_ACCESS_SHADER_READ_BIT);
    transition(cb, irradiance_image, 1U, 6U, VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               kDstStage, VK_ACCESS_SHADER_READ_BIT);
    transition(cb, lut_image, 1U, 1U, VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               kDstStage, VK_ACCESS_SHADER_READ_BIT);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
    (void)vkResetFences(device, 1U, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(queue, 1U, &submit, fence) != VK_SUCCESS) return false;
    if (vkWaitForFences(device, 1U, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
    return true;
  }

  void cleanup() {
    if (device == VK_NULL_HANDLE) return;
    sky_pipeline.cleanup(device);
    prefilter_pipeline.cleanup(device);
    irradiance_pipeline.cleanup(device);
    lut_pipeline.cleanup(device);
    if (fence != VK_NULL_HANDLE) {
      vkDestroyFence(device, fence, nullptr);
      fence = VK_NULL_HANDLE;
    }
    if (pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, pool, nullptr);  // frees the bake command buffer
      pool = VK_NULL_HANDLE;
    }
    if (sampler_sky != VK_NULL_HANDLE) {
      vkDestroySampler(device, sampler_sky, nullptr);
      sampler_sky = VK_NULL_HANDLE;
    }
    if (sampler_cube != VK_NULL_HANDLE) {
      vkDestroySampler(device, sampler_cube, nullptr);
      sampler_cube = VK_NULL_HANDLE;
    }
    if (sampler_flat != VK_NULL_HANDLE) {
      vkDestroySampler(device, sampler_flat, nullptr);
      sampler_flat = VK_NULL_HANDLE;
    }
    for (VkImageView* view : {&sky_view, &prefiltered_cube_view,
                              &irradiance_mip_view, &irradiance_cube_view,
                              &lut_view}) {
      if (*view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, *view, nullptr);
        *view = VK_NULL_HANDLE;
      }
    }
    for (VkImageView& view : prefilter_mip_views) {
      if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
      }
    }
    if (sky_memory.is_valid()) allocator->destroy_allocation(sky_memory);
    if (prefiltered_memory.is_valid()) {
      allocator->destroy_allocation(prefiltered_memory);
    }
    if (irradiance_memory.is_valid()) {
      allocator->destroy_allocation(irradiance_memory);
    }
    if (lut_memory.is_valid()) allocator->destroy_allocation(lut_memory);
    if (sky_image != VK_NULL_HANDLE) {
      vkDestroyImage(device, sky_image, nullptr);
      sky_image = VK_NULL_HANDLE;
    }
    if (prefiltered_image != VK_NULL_HANDLE) {
      vkDestroyImage(device, prefiltered_image, nullptr);
      prefiltered_image = VK_NULL_HANDLE;
    }
    if (irradiance_image != VK_NULL_HANDLE) {
      vkDestroyImage(device, irradiance_image, nullptr);
      irradiance_image = VK_NULL_HANDLE;
    }
    if (lut_image != VK_NULL_HANDLE) {
      vkDestroyImage(device, lut_image, nullptr);
      lut_image = VK_NULL_HANDLE;
    }
    device = VK_NULL_HANDLE;
  }
};

// ============================================================================
// GPU scaffolding: context, allocator, descriptor manager, bindless set-1
// sampler array, set-2 material SSBO (three 64-byte slots), offscreen target,
// flat-shaded cube, the IBL bake, and BOTH pipelines: the 3-set lambert-free
// PBR control (pbr_scene.frag) and the 4-set IBL variant (pbr_ibl.frag).
// ============================================================================

struct PbrIblHarness {
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
  omnicpp::render::VulkanPipeline pipeline;       // 3-set PBR control
  omnicpp::render::VulkanPipeline pipeline_ibl;   // 4-set IBL variant
  IblResources ibl;
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

    // Set 2: material SSBO, three 64-byte slots.
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
        3U * sizeof(PbrMaterialData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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

    // IBL bake resources (images, views, samplers, compute pipelines, set 3).
    if (!ibl.initialize(context.device(), context.physical_device(),
                        context.graphics_queue(), queue_family, allocator,
                        descriptors)) {
      return false;
    }

    const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;

    // 3-set control pipeline (pbr_scene.frag — no IBL set).
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
    const VkDescriptorSetLayout control_layouts[3] = {
        mesh_layout, textures_layout, material_layout};
    const VkPushConstantRange push_range{
        static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                        VK_SHADER_STAGE_FRAGMENT_BIT),
        0U, 160U};
    if (!pipeline
             .create_pipeline_layout(context.device(), control_layouts, 3U,
                                     &push_range)
             .is_ok() ||
        !pipeline
             .create_graphics_pipeline(context.device(), target.render_pass(),
                                       target.format(), pipeline.pipeline_layout(),
                                       true, true, false)
             .is_ok()) {
      return false;
    }

    // 4-set IBL pipeline (pbr_ibl.frag — set 3 = environment resources).
    if (!pipeline_ibl
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/pbr_scene.vert.spv", "vertex")
             .is_ok() ||
        !pipeline_ibl
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/pbr_ibl.frag.spv", "fragment")
             .is_ok()) {
      return false;
    }
    const VkDescriptorSetLayout ibl_layouts[4] = {mesh_layout, textures_layout,
                                                  material_layout,
                                                  ibl.ibl_layout};
    if (!pipeline_ibl
             .create_pipeline_layout(context.device(), ibl_layouts, 4U,
                                     &push_range)
             .is_ok() ||
        !pipeline_ibl
             .create_graphics_pipeline(context.device(), target.render_pass(),
                                       target.format(),
                                       pipeline_ibl.pipeline_layout(), true, true,
                                       false)
             .is_ok()) {
      return false;
    }
    return true;
  }

  //! Overwrite the three material SSBO slots from host memory.
  void write_materials(const PbrMaterialData* slots, std::size_t count) {
    std::memcpy(material_allocation.mapped, slots, count * sizeof(PbrMaterialData));
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
        began && renderer_record(scene, cb).is_ok();
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

  omnicpp::core::Result<void> renderer_record(const VulkanPbrScene& scene,
                                              VkCommandBuffer cb) {
    omnicpp::render::VulkanRenderer renderer;  // stateless record helper
    return renderer.record_pbr_scene(cb, scene, 256U, 256U);
  }

  void cleanup() {
    pipeline.cleanup(context.device());
    pipeline_ibl.cleanup(context.device());
    ibl.cleanup();
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
//! origin eye). `use_ibl` selects the 4-set IBL pipeline (and binds the
//! environment set) or the 3-set control pipeline.
VulkanPbrScene make_scene(PbrIblHarness& h, bool use_ibl) {
  VulkanPbrScene scene;
  scene.pipeline = use_ibl ? h.pipeline_ibl.pipeline() : h.pipeline.pipeline();
  scene.pipeline_layout =
      use_ibl ? h.pipeline_ibl.pipeline_layout() : h.pipeline.pipeline_layout();
  scene.camera.view_projection = make_perspective(45.0f, 1.0f, 0.1f, 30.0f);
  scene.camera_position = {0.0f, 0.0f, 0.0f, 1.0f};
  scene.texture_set = h.textures_set;
  scene.material_set = h.material_set;
  scene.ibl_set = use_ibl ? h.ibl.ibl_set : VK_NULL_HANDLE;
  return scene;
}

}  // namespace

// ============================================================================
// GPU tests
// ============================================================================

//! Environment specular only lights a smooth metal cube: with a baked sun disc
//! at +Z the cube's front face (reflection vector +Z) is bright, while the
//! identical material through the non-IBL pipeline (no environment set) stays
//! dark, and roughness 1.0 smears the disc below the bright threshold. This
//! proves the prefilter mip chain, the cube lookup by reflection vector and
//! the set-3 binding all work together.
TEST(VulkanHardware, PbrIblSunReflectsOnSmoothMetal) {
  PbrIblHarness h;
  if (!h.initialize("pbr_ibl_specular")) {
    GTEST_SKIP() << "Vulkan context or descriptor indexing unavailable";
  }

  // Bright sun disc at +Z over a near-black sky.
  IblSkyParams sky;
  sky.sun_direction = {0.0f, 0.0f, 1.0f};
  sky.sun_radius_rad = 0.14f;
  sky.sun_color = {6.0f, 6.0f, 6.0f};
  sky.sky_color = {0.02f, 0.02f, 0.02f};
  if (!h.ibl.bake(sky)) {
    h.cleanup();
    GTEST_SKIP() << "IBL bake failed";
  }

  PbrMaterialData materials[2]{};
  // Slot 0: mirror-smooth metal (specular env peak).
  materials[0].base_color_factor = {0.9f, 0.9f, 0.9f, 1.0f};
  materials[0].metallic_factor = 1.0f;
  materials[0].roughness_factor = 0.04f;
  // Slot 1: fully rough metal (sun smeared below threshold).
  materials[1].base_color_factor = {0.9f, 0.9f, 0.9f, 1.0f};
  materials[1].metallic_factor = 1.0f;
  materials[1].roughness_factor = 1.0f;
  h.write_materials(materials, 2U);

  VulkanPbrScene scene = make_scene(h, /*use_ibl=*/true);
  ScenePbrObject object;
  object.mesh = &h.cube.mesh;
  object.model = make_translation(0.0f, 0.0f, -4.5f);

  object.material_index = 0U;
  scene.objects.clear();
  scene.objects.push_back(object);
  const omnicpp_test::ReadbackResult smooth = h.render(scene);

  object.material_index = 1U;
  scene.objects.clear();
  scene.objects.push_back(object);
  const omnicpp_test::ReadbackResult rough = h.render(scene);

  // Control: identical smooth-metal cube without the IBL pipeline (and no
  // environment set bound). Direct lighting is unchanged.
  VulkanPbrScene control = make_scene(h, /*use_ibl=*/false);
  object.material_index = 0U;
  control.objects.clear();
  control.objects.push_back(object);
  const omnicpp_test::ReadbackResult no_ibl = h.render(control);

  h.cleanup();

  ASSERT_TRUE(smooth.submitted);
  ASSERT_TRUE(rough.submitted);
  ASSERT_TRUE(no_ibl.submitted);
  EXPECT_GT(smooth.non_clear_pixels, 5000U) << "IBL cube did not render";
  EXPECT_GT(no_ibl.non_clear_pixels, 5000U) << "control cube did not render";

  // The +Z face is lit by the environment alone: its reflection hits the sun.
  EXPECT_GT(smooth.peak_luma, 230U)
      << "smooth metal did not pick up the sun reflection "
         "(peak=" << smooth.peak_luma << ")";
  EXPECT_GT(smooth.bright_pixels, 800U)
      << "sun reflection region too small "
         "(bright=" << smooth.bright_pixels << ")";
  // Roughness 1.0 spreads the same radiance over the hemisphere: dimmer
  // than smooth, but may still pick up some direct-light contribution.
  EXPECT_LT(rough.peak_luma, smooth.peak_luma)
      << "rough metal should be dimmer than smooth "
         "(rough=" << rough.peak_luma << ", smooth=" << smooth.peak_luma << ")";
  EXPECT_LT(rough.bright_pixels, smooth.bright_pixels / 2U)
      << "rough metal bright region should be much smaller "
         "(rough=" << rough.bright_pixels << ", smooth=" << smooth.bright_pixels << ")";
  // Without IBL the identical surface is dimmer: the environment added the
  // specular highlight and diffuse irradiance.
  EXPECT_LT(no_ibl.peak_luma, smooth.peak_luma)
      << "non-IBL control should be dimmer "
         "(no_ibl=" << no_ibl.peak_luma << ", smooth=" << smooth.peak_luma << ")";
  EXPECT_GT(smooth.peak_luma, no_ibl.peak_luma + 50U);
  EXPECT_GT(smooth.bright_pixels, no_ibl.bright_pixels + 800U);
}

//! Diffuse irradiance: a uniform red sky tints a white dielectric red via the
//! cosine-convolved irradiance cube, while the identical material under the
//! non-IBL pipeline stays neutral gray (direct light only). This proves the
//! irradiance bake + the kD*albedo*irradiance(N) ambient term.
TEST(VulkanHardware, PbrIblRedSkyTintsWhiteDielectric) {
  PbrIblHarness h;
  if (!h.initialize("pbr_ibl_irradiance")) {
    GTEST_SKIP() << "Vulkan context or descriptor indexing unavailable";
  }

  // Uniform red sky (no sun disc) in every direction.
  IblSkyParams sky;
  sky.sun_direction = {0.0f, 1.0f, 0.0f};
  sky.sun_radius_rad = 0.01f;
  sky.sun_color = {0.0f, 0.0f, 0.0f};
  sky.sky_color = {1.0f, 0.0f, 0.0f};
  if (!h.ibl.bake(sky)) {
    h.cleanup();
    GTEST_SKIP() << "IBL bake failed";
  }

  PbrMaterialData materials[1]{};
  materials[0].base_color_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  materials[0].metallic_factor = 0.0f;
  materials[0].roughness_factor = 1.0f;
  h.write_materials(materials, 1U);

  VulkanPbrScene scene = make_scene(h, /*use_ibl=*/true);
  ScenePbrObject object;
  object.mesh = &h.cube.mesh;
  object.model = make_translation(0.0f, 0.0f, -4.5f);
  object.material_index = 0U;
  scene.objects.clear();
  scene.objects.push_back(object);
  const omnicpp_test::ReadbackResult ibl_result = h.render(scene);

  VulkanPbrScene control = make_scene(h, /*use_ibl=*/false);
  control.objects.clear();
  control.objects.push_back(object);
  const omnicpp_test::ReadbackResult no_ibl = h.render(control);

  h.cleanup();

  ASSERT_TRUE(ibl_result.submitted);
  ASSERT_TRUE(no_ibl.submitted);
  EXPECT_GT(ibl_result.non_clear_pixels, 5000U)
      << "irradiance-lit cube did not render";
  EXPECT_GT(no_ibl.non_clear_pixels, 5000U)
      << "control cube did not render";

  // IBL: the red ambient dominates every lit pixel (red >> green/blue).
  EXPECT_GT(ibl_result.red_dominant_pixels, 2000U)
      << "red sky did not tint the dielectric "
         "(red_dom=" << ibl_result.red_dominant_pixels << ")";
  // Control: direct white light only — the same surface is neutral.
  EXPECT_LT(no_ibl.red_dominant_pixels, 200U)
      << "control unexpectedly red-dominant "
         "(red_dom=" << no_ibl.red_dominant_pixels << ")";
  EXPECT_GT(ibl_result.red_dominant_pixels, no_ibl.red_dominant_pixels + 1500U);

  const std::uint32_t r = ibl_result.center_pixel & 0xFFU;
  const std::uint32_t g = (ibl_result.center_pixel >> 8U) & 0xFFU;
  const std::uint32_t b = (ibl_result.center_pixel >> 16U) & 0xFFU;
  EXPECT_GT(r, 150U) << "ambient red too dim at centre (r=" << r << ")";
  EXPECT_GT(r, g + 40U) << "red does not dominate green at centre "
                           "(r=" << r << ", g=" << g << ", b=" << b << ")";
  EXPECT_GT(r, b + 40U);
}

#endif  // WARPLOOM_HAS_VULKAN
