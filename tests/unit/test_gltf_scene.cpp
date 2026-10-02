//! @file test_gltf_scene.cpp
//! @brief GPU end-to-end tests for the glTF 2.0 ingestion path feeding the
//!        registry scene renderer: single-mesh and whole-scene import,
//!        per-primitive multi-material baseColorTextures (embedded PNG/JPEG
//!        data URIs, bufferView images and external files through the
//!        ExternalFileLoader), sRGB colour handling at sample time, and the
//!        lit 160-byte material ABI — all verified by pixel readback under
//!        Khronos validation.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "warploom/asset/gltf_importer.hpp"
#include "warploom/core/ecs.hpp"
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_frame_upload.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/render/vulkan_scene.hpp"

#include "vulkan_test_readback.hpp"

namespace {

using omnicpp::asset::ExternalFileLoader;
using omnicpp::asset::GltfMeshImport;
using omnicpp::asset::GltfSceneImport;
using omnicpp::asset::import_gltf_mesh;
using omnicpp::asset::import_gltf_scene;
using omnicpp::render::SceneCameraComponent;
using omnicpp::render::SceneMatrix;
using omnicpp::render::SceneMesh;
using omnicpp::render::SceneTransformComponent;
using omnicpp::render::scene_identity_matrix;

// ============================================================================
// In-memory glTF fixture builders
// ============================================================================

struct CubeFixture {
  std::string json;
  std::vector<std::uint8_t> bin;
  std::string name;
};

//! Build a self-contained single-cube glTF 2.0 document. `color` tints the
//! COLOR_0 attribute; the material factor is fixed by `factor`.
CubeFixture make_cube_gltf(const float color[3], const float factor[4],
                           const char* name) {
  constexpr float kPositions[8][3] = {
      {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
      {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
      {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}};
  constexpr std::uint16_t kIndices[36] = {
      0, 3, 1, 1, 3, 2,  // -z
      4, 5, 7, 5, 6, 7,  // +z
      0, 1, 4, 1, 5, 4,  // -y
      3, 7, 2, 2, 7, 6,  // +y
      0, 4, 3, 3, 4, 7,  // -x
      1, 2, 5, 2, 6, 5}; // +x

  CubeFixture fixture;
  fixture.name = name;
  std::vector<std::uint8_t>& bin = fixture.bin;

  const auto push_le = [&bin](const void* bytes, std::size_t count) {
    const std::size_t offset = bin.size();
    bin.resize(bin.size() + count);
    std::memcpy(bin.data() + offset, bytes, count);
    return offset;
  };
  const auto push_float = [&](float value) {
    return push_le(&value, sizeof(value));
  };

  // 8 positions, 8 colors, 8 radial normals (96 bytes each), 8 uvs (64
  // bytes), then 36 uint16 indices (72 bytes).
  const std::size_t pos_offset = bin.size();
  for (const auto& p : kPositions) {
    for (float component : p) (void)push_float(component);
  }
  const std::size_t color_offset = bin.size();
  for (int i = 0; i < 8; ++i) {
    (void)push_float(color[0]);
    (void)push_float(color[1]);
    (void)push_float(color[2]);
  }
  const std::size_t normal_offset = bin.size();
  for (const auto& p : kPositions) {
    float length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    (void)push_float(p[0] / length);
    (void)push_float(p[1] / length);
    (void)push_float(p[2] / length);
  }
  const std::size_t uv_offset = bin.size();
  for (int i = 0; i < 8; ++i) {
    (void)push_float(0.0f);
    (void)push_float(0.0f);
  }
  const std::size_t index_offset = bin.size();
  for (std::uint16_t index : kIndices) {
    const std::uint8_t little_endian[2] = {
        static_cast<std::uint8_t>(index & 0xFFU),
        static_cast<std::uint8_t>((index >> 8U) & 0xFFU)};
    (void)push_le(little_endian, sizeof(little_endian));
  }

  const auto view = [&](std::size_t offset, std::size_t length) {
    std::string out = R"({"buffer":0,"byteOffset":)";
    out += std::to_string(offset);
    out += R"(,"byteLength":)";
    out += std::to_string(length);
    out += "}";
    return out;
  };
  std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)";
  json += std::to_string(bin.size());
  json += R"(,"uri":"cube.bin"}],"bufferViews":[)";
  json += view(pos_offset, 96U) + "," + view(color_offset, 96U) + "," +
          view(normal_offset, 96U) + "," + view(uv_offset, 64U) + "," +
          view(index_offset, 72U);
  json += R"(],"accessors":[{"bufferView":0,"componentType":5126,"count":8,"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]},{"bufferView":1,"componentType":5126,"count":8,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":8,"type":"VEC3"},{"bufferView":3,"componentType":5126,"count":8,"type":"VEC2"},{"bufferView":4,"componentType":5123,"count":36,"type":"SCALAR"}],"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[)";
  json += std::to_string(factor[0]);
  json += ",";
  json += std::to_string(factor[1]);
  json += ",";
  json += std::to_string(factor[2]);
  json += ",";
  json += std::to_string(factor[3]);
  json += R"(]}}],"meshes":[{"name":")";
  json += name;
  json += R"(","primitives":[{"attributes":{"POSITION":0,"COLOR_0":1,"NORMAL":2,"TEXCOORD_0":3},"indices":4,"material":0}]}]})";
  fixture.json = std::move(json);
  return fixture;
}

//! Single-cube glTF whose material additionally binds baseColorTexture 0 to an
//! embedded PNG data URI. Vertex colors are white and the factor is opaque
//! white, so any output colour must come from the decoded texture.
CubeFixture make_cube_gltf_textured(const char* name,
                                    const std::string& png_uri) {
  constexpr float kPositions[8][3] = {
      {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
      {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
      {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}};
  constexpr std::uint16_t kIndices[36] = {
      0, 3, 1, 1, 3, 2, 4, 5, 7, 5, 6, 7,
      0, 1, 4, 1, 5, 4, 3, 7, 2, 2, 7, 6,
      0, 4, 3, 3, 4, 7, 1, 2, 5, 2, 6, 5};

  CubeFixture fixture;
  fixture.name = name;
  std::vector<std::uint8_t>& bin = fixture.bin;
  const auto push_le = [&bin](const void* bytes, std::size_t count) {
    const std::size_t offset = bin.size();
    bin.resize(bin.size() + count);
    std::memcpy(bin.data() + offset, bytes, count);
    return offset;
  };
  const auto push_float = [&](float value) {
    return push_le(&value, sizeof(value));
  };
  const std::size_t pos_offset = bin.size();
  for (const auto& p : kPositions) {
    for (float component : p) (void)push_float(component);
  }
  const std::size_t color_offset = bin.size();
  for (int i = 0; i < 8; ++i) {
    (void)push_float(1.0f);
    (void)push_float(1.0f);
    (void)push_float(1.0f);
  }
  const std::size_t normal_offset = bin.size();
  for (const auto& p : kPositions) {
    float length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    (void)push_float(p[0] / length);
    (void)push_float(p[1] / length);
    (void)push_float(p[2] / length);
  }
  const std::size_t uv_offset = bin.size();
  for (int i = 0; i < 8; ++i) {
    (void)push_float(0.0f);
    (void)push_float(0.0f);
  }
  const std::size_t index_offset = bin.size();
  for (std::uint16_t index : kIndices) {
    const std::uint8_t little_endian[2] = {
        static_cast<std::uint8_t>(index & 0xFFU),
        static_cast<std::uint8_t>((index >> 8U) & 0xFFU)};
    (void)push_le(little_endian, sizeof(little_endian));
  }

  const auto view = [&](std::size_t offset, std::size_t length) {
    std::string out = R"({"buffer":0,"byteOffset":)";
    out += std::to_string(offset);
    out += R"(,"byteLength":)";
    out += std::to_string(length);
    out += "}";
    return out;
  };
  std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)";
  json += std::to_string(bin.size());
  json += R"(,"uri":"cube.bin"}],"bufferViews":[)";
  json += view(pos_offset, 96U) + "," + view(color_offset, 96U) + "," +
          view(normal_offset, 96U) + "," + view(uv_offset, 64U) + "," +
          view(index_offset, 72U);
  json += R"(],"accessors":[{"bufferView":0,"componentType":5126,"count":8,"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]},{"bufferView":1,"componentType":5126,"count":8,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":8,"type":"VEC3"},{"bufferView":3,"componentType":5126,"count":8,"type":"VEC2"},{"bufferView":4,"componentType":5123,"count":36,"type":"SCALAR"}],)";
  json += R"("samplers":[{"magFilter":9728,"minFilter":9728,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0}],"images":[{"uri":")";
  json += png_uri;
  json += R"("}],"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1.000000,1.000000,1.000000,1.000000],"baseColorTexture":{"index":0}}}],"meshes":[{"name":")";
  json += name;
  json += R"(","primitives":[{"attributes":{"POSITION":0,"COLOR_0":1,"NORMAL":2,"TEXCOORD_0":3},"indices":4,"material":0}]}]})";
  fixture.json = std::move(json);
  return fixture;
}

//! Column-major perspective-projection matrix matching the scene camera
//! convention (vertical fov, z in [-1, 1] NDC after the depth mapping the
//! engine's projection uses). Pure arithmetic — no Vulkan types involved.
void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      SceneMatrix& m) {
  m.fill(0.0f);
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = zfar * zn;
  m[11] = -1.0f;
  m[14] = znear * zfar * zn;
}

//! Column-major translation matrix (translation lives in [12],[13],[14]).
void make_translation(float x, float y, float z, SceneMatrix& m) {
  m = scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

}  // namespace

#if defined(WARPLOOM_HAS_VULKAN)
namespace {  // (helpers)
// ============================================================================
// Texture upload helpers (test-side stand-ins for the app's upload layer).
// ============================================================================

struct SolidTexture {
  VkImage image{VK_NULL_HANDLE};
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  omnicpp::render::Allocation allocation{};
};

//! Create an RGBA8 texture uploaded through a private upload arena
//! (UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY). When `srgb_format` is
//! true the image and view use VK_FORMAT_R8G8B8A8_SRGB, so sampling hardware-
//! decodes the encoded bytes to linear — the canonical representation for
//! sRGB colour textures (glTF baseColorTexture). Bytes on the CPU are never
//! colour-transformed. On failure every handle created so far is released and
//! `out` is left empty.
bool make_texture_rgba(VkDevice device, VkPhysicalDevice physical_device,
                       VkQueue queue, std::uint32_t queue_family,
                       omnicpp::render::VulkanMemoryAllocator& allocator,
                       std::uint32_t width, std::uint32_t height,
                       const std::uint8_t* rgba, std::size_t byte_count,
                       SolidTexture& out, bool srgb_format = false) {
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

  const VkFormat texture_format = srgb_format ? VK_FORMAT_R8G8B8A8_SRGB
                                              : VK_FORMAT_R8G8B8A8_UNORM;
  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = texture_format;
  image_info.extent = {width, height, 1U};
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
  auto span = arena.acquire(byte_count);
  if (!span.is_ok()) return fail();
  std::memcpy(span.value().host_data, rgba, byte_count);
  arena.record_copy_image_rgba8(span.value(), tex.image, width, height);
  if (!arena.submit(queue).is_ok()) return fail();
  arena.wait_idle();

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = tex.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = texture_format;
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

//! 1x1 solid-colour convenience wrapper over make_texture_rgba.
bool make_solid_texture(VkDevice device, VkPhysicalDevice physical_device,
                        VkQueue queue, std::uint32_t queue_family,
                        omnicpp::render::VulkanMemoryAllocator& allocator,
                        const std::array<std::uint8_t, 4>& rgba,
                        SolidTexture& out) {
  return make_texture_rgba(device, physical_device, queue, queue_family,
                           allocator, 1U, 1U, rgba.data(), rgba.size(), out);
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
// One glTF mesh, two primitives (unit cubes offset in X), each binding its
// own material with a baseColorTexture. Used by the multi-material and
// per-material-splitting GPU tests.
// ============================================================================

struct TwoCubeFixture {
  std::string json;
  std::vector<std::uint8_t> bin;
};

//! One glTF mesh whose two primitives are unit cubes offset in X (the first
//! centred at x=0, the second centred at x=+2.5). Each primitive binds its own
//! material with a solid-colour baseColorTexture (red, green). All vertex
//! colours and factors are white, so framebuffer colour must come from the
//! per-primitive textures.
TwoCubeFixture make_two_cube_multimaterial_gltf(const std::string& red_uri,
                                                const std::string& green_uri) {
  constexpr float kCubePositions[8][3] = {
      {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
      {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f},
      {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}};
  constexpr std::uint16_t kIndices[36] = {
      0, 3, 1, 1, 3, 2, 4, 5, 7, 5, 6, 7,
      0, 1, 4, 1, 5, 4, 3, 7, 2, 2, 7, 6,
      0, 4, 3, 3, 4, 7, 1, 2, 5, 2, 6, 5};

  TwoCubeFixture fixture;
  std::vector<std::uint8_t>& bin = fixture.bin;
  const auto push_le = [&bin](const void* bytes, std::size_t count) {
    const std::size_t offset = bin.size();
    bin.resize(bin.size() + count);
    std::memcpy(bin.data() + offset, bytes, count);
    return offset;
  };
  const auto push_float = [&](float value) {
    return push_le(&value, sizeof(value));
  };

  // Two cubes back to back: positions (x-offset for the second), colors,
  // normals, uvs, then uint16 indices — each cube its own view set.
  std::vector<std::size_t> pos_offsets, col_offsets, nrm_offsets, uv_offsets,
      idx_offsets;
  for (int cube = 0; cube < 2; ++cube) {
    const float x_offset = cube == 0 ? 0.0f : 2.5f;
    pos_offsets.push_back(bin.size());
    for (const auto& p : kCubePositions) {
      (void)push_float(p[0] + x_offset);
      (void)push_float(p[1]);
      (void)push_float(p[2]);
    }
    col_offsets.push_back(bin.size());
    for (int i = 0; i < 8; ++i) {
      (void)push_float(1.0f);
      (void)push_float(1.0f);
      (void)push_float(1.0f);
    }
    nrm_offsets.push_back(bin.size());
    for (const auto& p : kCubePositions) {
      const float length =
          std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
      (void)push_float(p[0] / length);
      (void)push_float(p[1] / length);
      (void)push_float(p[2] / length);
    }
    uv_offsets.push_back(bin.size());
    for (int i = 0; i < 8; ++i) {
      (void)push_float(0.0f);
      (void)push_float(0.0f);
    }
    idx_offsets.push_back(bin.size());
    for (std::uint16_t index : kIndices) {
      const std::uint8_t le[2] = {
          static_cast<std::uint8_t>(index & 0xFFU),
          static_cast<std::uint8_t>((index >> 8U) & 0xFFU)};
      (void)push_le(le, sizeof(le));
    }
  }

  const auto view = [](std::size_t offset, std::size_t length) {
    std::string out = R"({"buffer":0,"byteOffset":)";
    out += std::to_string(offset);
    out += R"(,"byteLength":)";
    out += std::to_string(length);
    out += "}";
    return out;
  };

  std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)";
  json += std::to_string(bin.size());
  json += R"(,"uri":"twocube.bin"}],"bufferViews":[)";
  for (std::size_t cube = 0; cube < 2U; ++cube) {
    if (cube != 0) json += ",";
    json += view(pos_offsets[cube], 96U) + "," + view(col_offsets[cube], 96U) +
            "," + view(nrm_offsets[cube], 96U) + "," +
            view(uv_offsets[cube], 64U) + "," + view(idx_offsets[cube], 72U);
  }
  json += R"(],"accessors":[)";
  for (std::size_t cube = 0; cube < 2U; ++cube) {
    if (cube != 0) json += ",";
    const std::size_t b = cube * 5U;
    json += R"({"bufferView":)" + std::to_string(b) +
            R"(,"componentType":5126,"count":8,"type":"VEC3"},{"bufferView":)" +
            std::to_string(b + 1U) +
            R"(,"componentType":5126,"count":8,"type":"VEC3"},{"bufferView":)" +
            std::to_string(b + 2U) +
            R"(,"componentType":5126,"count":8,"type":"VEC3"},{"bufferView":)" +
            std::to_string(b + 3U) +
            R"(,"componentType":5126,"count":8,"type":"VEC2"},{"bufferView":)" +
            std::to_string(b + 4U) +
            R"(,"componentType":5123,"count":36,"type":"SCALAR"})";
  }
  json += R"(],"materials":[{"name":"RedMat","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},{"name":"GreenMat","pbrMetallicRoughness":{"baseColorTexture":{"index":1}}}],)";
  json += R"("textures":[{"source":0},{"source":1}],"images":[{"uri":")" +
          red_uri + R"("},{"uri":")" + green_uri +
          R"("}],"meshes":[{"name":"TwoCubes","primitives":[)";
  json += R"({"attributes":{"POSITION":0,"COLOR_0":1,"NORMAL":2,"TEXCOORD_0":3},"indices":4,"material":0},)";
  json += R"({"attributes":{"POSITION":5,"COLOR_0":6,"NORMAL":7,"TEXCOORD_0":8},"indices":9,"material":1})";
  json += R"(]}]})";
  fixture.json = std::move(json);
  return fixture;
}

// ============================================================================
// Shared GPU scaffolding: context, allocator, descriptor manager, bindless
// albedo set (element 0 = opaque-white fallback), offscreen target and lit
// material pipeline. Individual tests own their world/registry/objects and
// upload their own textures.
// ============================================================================

struct GpuSceneHarness {
  omnicpp::render::VulkanContext context;
  omnicpp::render::VulkanMemoryAllocator allocator;
  omnicpp::render::VulkanDescriptorManager descriptors;
  VkDescriptorSetLayout mesh_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout albedo_layout{VK_NULL_HANDLE};
  VkDescriptorSet albedo_set{VK_NULL_HANDLE};
  SolidTexture white_tex;
  omnicpp::render::VulkanOffscreenTarget target;
  omnicpp::render::VulkanPipeline pipeline;
  omnicpp::render::VulkanRenderer renderer;
  std::uint32_t queue_family{0};

  bool initialized() const noexcept { return context.is_initialized(); }

  //! Context + allocator + descriptor manager + bindless albedo set (with the
  //! opaque-white fallback at element 0) + offscreen target + lit material
  //! pipeline. On failure returns false after releasing everything.
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

    const std::vector<omnicpp::render::ReflectedBinding> bindings = {
        {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         VK_SHADER_STAGE_VERTEX_BIT}};
    auto layout = descriptors.create_layout(bindings, 8U);
    if (!layout.is_ok()) return false;
    mesh_layout = layout.value();

    const std::vector<omnicpp::render::ReflectedBinding> albedo_bindings = {
        {1U, 0U, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         VK_SHADER_STAGE_FRAGMENT_BIT}};
    auto albedo = descriptors.create_layout(albedo_bindings, 1U,
                                            /*bindless=*/true);
    if (!albedo.is_ok()) return false;
    albedo_layout = albedo.value();
    auto albedo_set_result = descriptors.allocate_set(albedo_layout);
    if (!albedo_set_result.is_ok()) return false;
    albedo_set = albedo_set_result.value();

    if (!make_solid_texture(context.device(), context.physical_device(),
                            context.graphics_queue(), queue_family, allocator,
                            std::array<std::uint8_t, 4>{255U, 255U, 255U, 255U},
                            white_tex)) {
      return false;
    }
    if (!descriptors.write_image(
             albedo_set, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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

    const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
    if (!pipeline
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/indexed_scene_material.vert.spv",
                                     "vertex")
             .is_ok() ||
        !pipeline
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/indexed_scene_material.frag.spv",
                                     "fragment")
             .is_ok()) {
      return false;
    }
    const VkDescriptorSetLayout set_layouts[2] = {mesh_layout, albedo_layout};
    const VkPushConstantRange push_range{
        static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                        VK_SHADER_STAGE_FRAGMENT_BIT),
        0U, 160U};
    if (!pipeline
             .create_pipeline_layout(context.device(), set_layouts, 2U,
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

  //! Allocate + bind one storage-buffer descriptor set for a vertex buffer.
  VkDescriptorSet make_mesh_set(VkBuffer vertex_buffer) {
    auto set = descriptors.allocate_set(mesh_layout);
    if (!set.is_ok()) return VK_NULL_HANDLE;
    if (!descriptors.write_buffer(set.value(), 0U,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  vertex_buffer, 0U, VK_WHOLE_SIZE)
             .is_ok()) {
      return VK_NULL_HANDLE;
    }
    return set.value();
  }

  //! Bind one decoded texture into the albedo array at `index`.
  bool bind_albedo(const SolidTexture& texture, std::uint32_t index) {
    return descriptors
        .write_image(albedo_set, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     texture.sampler, texture.view,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, index)
        .is_ok();
  }

  //! Render one snapshot and read the 256x256 offscreen image back.
  omnicpp_test::ReadbackResult render(const omnicpp::render::VulkanScene& snapshot) {
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
        began && renderer.record_scene(cb, snapshot, 256U, 256U).is_ok();
    vkCmdEndRenderPass(cb);
    const bool ended = vkEndCommandBuffer(cb) == VK_SUCCESS;
    (void)vkResetFences(context.device(), 1U, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &cb;
    const bool submitted =
        recorded && ended &&
        vkQueueSubmit(context.graphics_queue(), 1U, &submit, fence) == VK_SUCCESS &&
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
    descriptors.cleanup();
    allocator.cleanup();
    context.cleanup();
  }
};

//! Build a SceneMesh backed by host-visible vertex/index storage buffers and a
//! per-mesh descriptor set from one GPU-format mesh import.
struct GpuMeshUpload {
  omnicpp::render::Allocation vertex_allocation{};
  omnicpp::render::Allocation index_allocation{};
  omnicpp::render::SceneMesh mesh{};

  //! Create the upload from a whole GltfMeshImport (all primitives merged).
  bool create(GpuSceneHarness& h, const GltfMeshImport& import) {
    const VkDeviceSize vertex_bytes = import.vertices.size() * sizeof(float);
    const VkDeviceSize index_bytes = import.indices.size() * sizeof(std::uint32_t);
    auto vb = h.allocator.create_buffer(
        vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto ib = h.allocator.create_buffer(
        index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!vb.is_ok() || !ib.is_ok()) return false;
    vertex_allocation = vb.value();
    index_allocation = ib.value();
    std::memcpy(vertex_allocation.mapped, import.vertices.data(), vertex_bytes);
    std::memcpy(index_allocation.mapped, import.indices.data(), index_bytes);
    VkDescriptorSet set = h.make_mesh_set(vertex_allocation.buffer);
    if (set == VK_NULL_HANDLE) return false;
    mesh.vertex_buffer = vertex_allocation.buffer;
    mesh.index_buffer = index_allocation.buffer;
    mesh.index_count = static_cast<std::uint32_t>(import.indices.size());
    mesh.descriptor_set = set;
    return mesh.is_drawable();
  }

  void destroy(omnicpp::render::VulkanMemoryAllocator& allocator) {
    if (vertex_allocation.is_valid()) allocator.destroy_allocation(vertex_allocation);
    if (index_allocation.is_valid()) allocator.destroy_allocation(index_allocation);
  }
};

}  // namespace (helpers)
#endif  // WARPLOOM_HAS_VULKAN

// ============================================================================
// End-to-end glTF scene tests
// ============================================================================

//! End-to-end baseColorTexture proof for the PNG codec path: the glTF
//! document embeds a 1x1 red PNG (data URI) behind material.baseColorTexture.
//! Import decodes it to RGBA on the CPU; the test uploads those exact bytes,
//! registers them as a texture record, and switches the cube's material so the
//! lit scene path samples it through the bindless albedo array. Readback must
//! show red where the plain white material showed neutral gray.
TEST(VulkanHardware, GltfBaseColorTextureEndToEnd) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  // 1x1 solid red PNG (pure colour: invariant under sRGB decode).
  constexpr const char* kRedPngDataUri =
      "data:image/png;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
      "iZk9HQAAAABJRU5ErkJggg==";
  const CubeFixture cube =
      make_cube_gltf_textured("textured_cube", kRedPngDataUri);
  std::string detail;
  auto imported = import_gltf_mesh(cube.json.data(), cube.json.size(),
                                   cube.bin.data(), cube.bin.size(), 0U, &detail);
  ASSERT_TRUE(imported.is_ok()) << detail;
  const auto& gltf_import = imported.value();
  ASSERT_TRUE(gltf_import.albedo.present);
  EXPECT_TRUE(gltf_import.albedo.encoded_srgb);
  ASSERT_EQ(gltf_import.images.size(), 1U);
  EXPECT_EQ(gltf_import.albedo.image_index, 0U);
  EXPECT_EQ(gltf_import.albedo.mag_filter, 9728U);
  EXPECT_EQ(gltf_import.albedo.min_filter, 9728U);
  EXPECT_EQ(gltf_import.albedo.wrap_s, 33071U);
  EXPECT_EQ(gltf_import.albedo.wrap_t, 33071U);
  EXPECT_EQ(gltf_import.images[0].width, 1U);
  EXPECT_EQ(gltf_import.images[0].height, 1U);
  ASSERT_EQ(gltf_import.images[0].rgba.size(), 4U);
  EXPECT_EQ(gltf_import.images[0].rgba[0], 255U);
  EXPECT_EQ(gltf_import.images[0].rgba[1], 0U);
  EXPECT_EQ(gltf_import.images[0].rgba[2], 0U);
  EXPECT_EQ(gltf_import.images[0].rgba[3], 255U);

  GpuSceneHarness h;
  if (!h.initialize("OmniCppGltfBaseColorTexture")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }
  GpuMeshUpload upload;
  ASSERT_TRUE(upload.create(h, gltf_import));
  SolidTexture gltf_tex;
  ASSERT_TRUE(make_texture_rgba(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator, gltf_import.images[0].width,
      gltf_import.images[0].height, gltf_import.images[0].rgba.data(),
      gltf_import.images[0].rgba.size(), gltf_tex));
  ASSERT_TRUE(h.bind_albedo(gltf_tex, 1U));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_handle = registry.create_mesh(upload.mesh);
  const std::array<float, 4> opaque_white{1.0f, 1.0f, 1.0f, 1.0f};
  const auto plain_material = registry.create_material({opaque_white});
  const auto gltf_texture = registry.create_texture({gltf_tex.view, gltf_tex.sampler, 1U});
  const auto textured_material =
      registry.create_material({opaque_white, gltf_texture});
  ASSERT_TRUE(mesh_handle.valid());
  ASSERT_TRUE(plain_material.valid());
  ASSERT_TRUE(textured_material.valid());
  ASSERT_TRUE(gltf_texture.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto cube_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  omnicpp::render::SceneBounds cube_bounds{};
  cube_bounds.valid = gltf_import.bounds.valid;
  cube_bounds.min = {gltf_import.bounds.min[0], gltf_import.bounds.min[1],
                     gltf_import.bounds.min[2]};
  cube_bounds.max = {gltf_import.bounds.max[0], gltf_import.bounds.max[1],
                     gltf_import.bounds.max[2]};
  ASSERT_TRUE(cube_bounds.valid);
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      cube_entity,
      {nullptr, true, mesh_handle, plain_material, nullptr, cube_bounds});
  omnicpp::render::SceneTransformComponent cube_transform;
  make_translation(0.0f, 0.0f, -4.0f, cube_transform.model);
  world.add_component<omnicpp::render::SceneTransformComponent>(cube_entity,
                                                                cube_transform);

  const auto render = [&](const omnicpp::render::VulkanScene& scene) {
    return h.render(scene);
  };
  auto snapshot = [&]() {
    auto scene = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene.texture_set = h.albedo_set;
    return scene;
  };

  // Frame A: plain white material (no texture). The lit cube is neutral gray.
  const auto untextured = render(snapshot());
  ASSERT_TRUE(untextured.submitted);
  // Frame B: switch to the material whose albedo came from the glTF PNG.
  world.get_component<omnicpp::render::SceneRenderableComponent>(cube_entity)
      .material_handle = textured_material;
  const auto textured = render(snapshot());
  ASSERT_TRUE(textured.submitted);

  EXPECT_LT(untextured.red_dominant_pixels, 500U);
  EXPECT_GT(textured.red_dominant_pixels, 4000U);
  EXPECT_GT(textured.red_dominant_pixels,
            untextured.red_dominant_pixels + 1000U);
  EXPECT_NE(untextured.hash, textured.hash);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_handle, 1U);
  (void)registry.destroy(plain_material, 1U);
  (void)registry.destroy(textured_material, 1U);
  (void)registry.destroy(gltf_texture, 1U);
  registry.collect(1U);
  destroy_solid_texture(h.context.device(), h.allocator, gltf_tex);
  upload.destroy(h.allocator);
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

//! End-to-end baseColorTexture proof for the JPEG codec path: the glTF
//! document embeds an 8x8 solid red JPEG (220,30,30) behind
//! material.baseColorTexture. Import dispatches the payload to the vendored
//! JPEG decoder on the CPU (the constant colour decodes bit-exactly); the test
//! uploads those exact bytes, registers them as a texture record, and switches
//! the cube's material so the lit scene path samples it through the bindless
//! albedo array — the same proof the PNG sibling runs, but for JPEG payloads.
TEST(VulkanHardware, GltfJpegBaseColorTextureEndToEnd) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  // 8x8 solid red JPEG (220,30,30) — constant content encodes to DC-only
  // coefficients, so the decoder reproduces it bit-exactly.
  constexpr const char* kRedJpegDataUri =
      "data:image/jpeg;base64,"
"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYF"
      "BgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoK"
      "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAIAAgDASIA"
      "AhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQA"
      "AAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
      "ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWm"
      "p6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEA"
      "AwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSEx"
      "BhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElK"
      "U1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3"
      "uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5booo"
      "r+fz/Xw//9k=";
  const CubeFixture cube =
      make_cube_gltf_textured("jpeg_textured_cube", kRedJpegDataUri);
  std::string detail;
  auto imported = import_gltf_mesh(cube.json.data(), cube.json.size(),
                                   cube.bin.data(), cube.bin.size(), 0U, &detail);
  ASSERT_TRUE(imported.is_ok()) << detail;
  const auto& gltf_import = imported.value();
  ASSERT_TRUE(gltf_import.albedo.present);
  EXPECT_TRUE(gltf_import.albedo.encoded_srgb);
  ASSERT_EQ(gltf_import.images.size(), 1U);
  EXPECT_EQ(gltf_import.images[0].width, 8U);
  EXPECT_EQ(gltf_import.images[0].height, 8U);
  ASSERT_EQ(gltf_import.images[0].rgba.size(), 8U * 8U * 4U);
  for (std::size_t i = 0; i < 64U; ++i) {
    const std::uint8_t* px = gltf_import.images[0].rgba.data() + i * 4U;
    EXPECT_EQ(px[0], 220U) << "pixel " << i << " r";
    EXPECT_EQ(px[1], 30U) << "pixel " << i << " g";
    EXPECT_EQ(px[2], 30U) << "pixel " << i << " b";
    EXPECT_EQ(px[3], 255U) << "pixel " << i << " a";
  }

  GpuSceneHarness h;
  if (!h.initialize("OmniCppGltfJpegBaseColorTexture")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }
  GpuMeshUpload upload;
  ASSERT_TRUE(upload.create(h, gltf_import));
  SolidTexture gltf_tex;
  ASSERT_TRUE(make_texture_rgba(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator, gltf_import.images[0].width,
      gltf_import.images[0].height, gltf_import.images[0].rgba.data(),
      gltf_import.images[0].rgba.size(), gltf_tex));
  ASSERT_TRUE(h.bind_albedo(gltf_tex, 1U));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_handle = registry.create_mesh(upload.mesh);
  const std::array<float, 4> opaque_white{1.0f, 1.0f, 1.0f, 1.0f};
  const auto plain_material = registry.create_material({opaque_white});
  const auto gltf_texture = registry.create_texture({gltf_tex.view, gltf_tex.sampler, 1U});
  const auto textured_material = registry.create_material({opaque_white, gltf_texture});
  ASSERT_TRUE(mesh_handle.valid());
  ASSERT_TRUE(plain_material.valid());
  ASSERT_TRUE(textured_material.valid());
  ASSERT_TRUE(gltf_texture.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto cube_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  omnicpp::render::SceneBounds cube_bounds{};
  cube_bounds.valid = gltf_import.bounds.valid;
  cube_bounds.min = {gltf_import.bounds.min[0], gltf_import.bounds.min[1],
                      gltf_import.bounds.min[2]};
  cube_bounds.max = {gltf_import.bounds.max[0], gltf_import.bounds.max[1],
                      gltf_import.bounds.max[2]};
  ASSERT_TRUE(cube_bounds.valid);
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      cube_entity,
      {nullptr, true, mesh_handle, plain_material, nullptr, cube_bounds});
  omnicpp::render::SceneTransformComponent cube_transform;
  make_translation(0.0f, 0.0f, -4.0f, cube_transform.model);
  world.add_component<omnicpp::render::SceneTransformComponent>(cube_entity,
                                                                cube_transform);

  const auto snapshot = [&]() {
    auto scene = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene.texture_set = h.albedo_set;
    return scene;
  };

  const auto untextured = h.render(snapshot());
  ASSERT_TRUE(untextured.submitted);
  world.get_component<omnicpp::render::SceneRenderableComponent>(cube_entity)
      .material_handle = textured_material;
  const auto textured = h.render(snapshot());
  ASSERT_TRUE(textured.submitted);

  EXPECT_LT(untextured.red_dominant_pixels, 500U);
  EXPECT_GT(textured.red_dominant_pixels, 4000U);
  EXPECT_GT(textured.red_dominant_pixels,
            untextured.red_dominant_pixels + 1000U);
  EXPECT_NE(untextured.hash, textured.hash);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_handle, 1U);
  (void)registry.destroy(plain_material, 1U);
  (void)registry.destroy(textured_material, 1U);
  (void)registry.destroy(gltf_texture, 1U);
  registry.collect(1U);
  destroy_solid_texture(h.context.device(), h.allocator, gltf_tex);
  upload.destroy(h.allocator);
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

//! Canonical colour-space proof: glTF baseColorTexture bytes are sRGB-encoded
//! and must be sampled through an sRGB image format so the hardware linearises
//! before lighting. An encoded grey of 186 decodes to linear 0.491021
//! ((186/255 + 0.055)/1.055)^2.4. Under the lit-scene ABI the brightest cube
//! face is lit by factor 0.45 + 0.55*dot(normalize(0.3,0.65,0.7), +Z) =
//! 0.834520, so the frame-centre pixel must read ~0.491021*0.834520*255 ~= 104.
//! If the texture were sampled raw (bytes treated as linear), the centre would
//! read ~155 — a 50-count separation. The test renders the same cube two ways:
//! with the grey as a material factor (already linear by convention) and as an
//! sRGB-format texture; byte-close centre pixels prove the representations of
//! the same linear value meet, and the analytic window proves the value.
TEST(VulkanHardware, SrgbBaseColorTextureLinearisesAtSample) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  // 1x1 grayscale PNG, encoded value 186 (Pillow 'L' mode).
  constexpr const char* kGrey186PngDataUri =
      "data:image/png;base64,"
"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAAAAAA6fptVAAAACklEQVR4nGPYBQAAvAC774y9jQAA"
      "AABJRU5ErkJggg==";
  // sRGB decode of 186 (piecewise transfer, double then float).
  constexpr float kLinearGrey = 0.491021f;

  const CubeFixture cube =
      make_cube_gltf_textured("srgb_grey_cube", kGrey186PngDataUri);
  std::string detail;
  auto imported = import_gltf_mesh(cube.json.data(), cube.json.size(),
                                   cube.bin.data(), cube.bin.size(), 0U, &detail);
  ASSERT_TRUE(imported.is_ok()) << detail;
  const auto& gltf_import = imported.value();
  ASSERT_TRUE(gltf_import.albedo.present);
  EXPECT_TRUE(gltf_import.albedo.encoded_srgb);
  ASSERT_EQ(gltf_import.images.size(), 1U);
  ASSERT_EQ(gltf_import.images[0].rgba.size(), 4U);
  // Bytes stay *encoded* on the CPU: 186 on every channel, straight alpha.
  EXPECT_EQ(gltf_import.images[0].rgba[0], 186U);
  EXPECT_EQ(gltf_import.images[0].rgba[1], 186U);
  EXPECT_EQ(gltf_import.images[0].rgba[2], 186U);
  EXPECT_EQ(gltf_import.images[0].rgba[3], 255U);

  GpuSceneHarness h;
  if (!h.initialize("OmniCppSrgbBaseColor")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }
  GpuMeshUpload upload;
  ASSERT_TRUE(upload.create(h, gltf_import));
  // The canonical upload for an encoded_srgb binding: an SRGB image so the
  // hardware decodes 186 -> 0.491021 linear at sample time.
  SolidTexture grey_tex;
  ASSERT_TRUE(make_texture_rgba(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator, gltf_import.images[0].width,
      gltf_import.images[0].height, gltf_import.images[0].rgba.data(),
      gltf_import.images[0].rgba.size(), grey_tex, /*srgb_format=*/true));
  ASSERT_TRUE(h.bind_albedo(grey_tex, 1U));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_handle = registry.create_mesh(upload.mesh);
  const auto factor_material = registry.create_material(
      {kLinearGrey, kLinearGrey, kLinearGrey, 1.0f});
  const auto grey_texture = registry.create_texture({grey_tex.view, grey_tex.sampler, 1U});
  const auto textured_material =
      registry.create_material({1.0f, 1.0f, 1.0f, 1.0f, grey_texture});
  ASSERT_TRUE(mesh_handle.valid());
  ASSERT_TRUE(factor_material.valid());
  ASSERT_TRUE(textured_material.valid());
  ASSERT_TRUE(grey_texture.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto cube_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  omnicpp::render::SceneBounds cube_bounds{};
  cube_bounds.valid = gltf_import.bounds.valid;
  cube_bounds.min = {gltf_import.bounds.min[0], gltf_import.bounds.min[1],
                      gltf_import.bounds.min[2]};
  cube_bounds.max = {gltf_import.bounds.max[0], gltf_import.bounds.max[1],
                      gltf_import.bounds.max[2]};
  ASSERT_TRUE(cube_bounds.valid);
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      cube_entity,
      {nullptr, true, mesh_handle, factor_material, nullptr, cube_bounds});
  omnicpp::render::SceneTransformComponent cube_transform;
  make_translation(0.0f, 0.0f, -4.0f, cube_transform.model);
  world.add_component<omnicpp::render::SceneTransformComponent>(cube_entity,
                                                                cube_transform);

  const auto snapshot = [&]() {
    auto scene = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene.texture_set = h.albedo_set;
    return scene;
  };
  const auto grey_of = [](std::uint32_t pixel) { return pixel & 0xFFU; };

  // Frame A: the same linear grey as a material factor (vertex-colour path).
  const auto factor_frame = h.render(snapshot());
  ASSERT_TRUE(factor_frame.submitted);
  const auto factor_centre = grey_of(factor_frame.center_pixel);
  // Frame B: the grey as an sRGB-format texture (encoded 186 on disk).
  world.get_component<omnicpp::render::SceneRenderableComponent>(cube_entity)
      .material_handle = textured_material;
  const auto texture_frame = h.render(snapshot());
  ASSERT_TRUE(texture_frame.submitted);
  const auto texture_centre = grey_of(texture_frame.center_pixel);

  // Both representations of linear 0.491021*0.834520*255 ~= 104.5 must land on
  // the same ~104 byte; sampling 186 raw-as-linear would give ~155 instead.
  EXPECT_NEAR(static_cast<int>(factor_centre), 104, 3)
      << "factor-frame centre (grey decoded 104.5 expected)";
  EXPECT_NEAR(static_cast<int>(texture_centre), 104, 3)
      << "sRGB-texture-frame centre (must equal the linear value, not ~155)";
  EXPECT_LE(texture_centre > factor_centre ? texture_centre - factor_centre
                                           : factor_centre - texture_centre,
            1U)
      << "texture and factor paths of the same linear grey must match";
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_handle, 1U);
  (void)registry.destroy(factor_material, 1U);
  (void)registry.destroy(textured_material, 1U);
  (void)registry.destroy(grey_texture, 1U);
  registry.collect(1U);
  destroy_solid_texture(h.context.device(), h.allocator, grey_tex);
  upload.destroy(h.allocator);
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
//! Scene-path integration for imported meshes with the lit material ABI: a red
//! vertex-coloured cube (near) fully occludes a white-vertex cube tinted green
//! purely by its material baseColorFactor (far). Re-importing the same meshes
//! with the red cube moved behind the green one flips the occlusion, proving
//! depth ordering and per-object materials — and the transform change between
//! frames is the engine's animation hook.
TEST(VulkanHardware, GltfImportedSceneWithRegistryMaterials) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  const float red_color[3] = {1.0f, 0.0f, 0.0f};
  const float white_color[3] = {1.0f, 1.0f, 1.0f};
  const float opaque[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float green_factor[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  const CubeFixture red_cube = make_cube_gltf(red_color, opaque, "red_cube");
  const CubeFixture green_cube = make_cube_gltf(white_color, green_factor, "green_cube");

  std::string detail;
  auto red_imported = import_gltf_mesh(red_cube.json.data(), red_cube.json.size(),
                                       red_cube.bin.data(), red_cube.bin.size(),
                                       0U, &detail);
  auto green_imported =
      import_gltf_mesh(green_cube.json.data(), green_cube.json.size(),
                       green_cube.bin.data(), green_cube.bin.size(), 0U, &detail);
  ASSERT_TRUE(red_imported.is_ok()) << detail;
  ASSERT_TRUE(green_imported.is_ok()) << detail;
  const auto& red_import = red_imported.value();
  const auto& green_import = green_imported.value();
  ASSERT_TRUE(red_import.bounds.valid);
  ASSERT_TRUE(green_import.bounds.valid);

  GpuSceneHarness h;
  if (!h.initialize("OmniCppRegistryMaterials")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }
  GpuMeshUpload red_upload;
  GpuMeshUpload green_upload;
  ASSERT_TRUE(red_upload.create(h, red_import));
  ASSERT_TRUE(green_upload.create(h, green_import));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_red = registry.create_mesh(red_upload.mesh);
  const auto mesh_green = registry.create_mesh(green_upload.mesh);
  const std::array<float, 4> opaque_white{1.0f, 1.0f, 1.0f, 1.0f};
  const std::array<float, 4> green{0.0f, 1.0f, 0.0f, 1.0f};
  const auto material_red = registry.create_material({opaque_white});
  const auto material_green = registry.create_material({green});
  ASSERT_TRUE(mesh_red.valid());
  ASSERT_TRUE(mesh_green.valid());
  ASSERT_TRUE(material_red.valid());
  ASSERT_TRUE(material_green.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto red_entity = world.create_entity();
  const auto green_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  omnicpp::render::SceneBounds bounds{};
  bounds.valid = true;
  bounds.min = {-1.0f, -1.0f, -1.0f};
  bounds.max = {1.0f, 1.0f, 1.0f};
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      red_entity, {nullptr, true, mesh_red, material_red, nullptr, bounds});
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      green_entity, {nullptr, true, mesh_green, material_green, nullptr, bounds});
  omnicpp::render::SceneTransformComponent red_transform;
  omnicpp::render::SceneTransformComponent green_transform;
  make_translation(0.0f, 0.0f, -3.0f, red_transform.model);    // near: occludes
  make_translation(0.0f, 0.0f, -5.0f, green_transform.model);  // far
  world.add_component<omnicpp::render::SceneTransformComponent>(red_entity,
                                                                red_transform);
  world.add_component<omnicpp::render::SceneTransformComponent>(green_entity,
                                                                green_transform);

  const auto snapshot = [&]() {
    auto scene = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene.texture_set = h.albedo_set;
    return scene;
  };

  // Frame 1: the near red cube hides the far green cube.
  const auto first = h.render(snapshot());
  ASSERT_TRUE(first.submitted);
  EXPECT_GT(first.red_dominant_pixels, 5000U);
  EXPECT_LT(first.green_dominant_pixels, 800U);

  // Frame 2 (animation): the red cube moves behind the green one; occlusion
  // flips, so no red remains visible and green dominates.
  omnicpp::render::SceneTransformComponent behind;
  make_translation(0.0f, 0.0f, -7.0f, behind.model);
  world.get_component<omnicpp::render::SceneTransformComponent>(red_entity)
      .model = behind.model;
  const auto second = h.render(snapshot());
  ASSERT_TRUE(second.submitted);
  EXPECT_EQ(second.red_dominant_pixels, 0U);
  EXPECT_GT(second.green_dominant_pixels, 5000U);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_red, 1U);
  (void)registry.destroy(mesh_green, 1U);
  (void)registry.destroy(material_red, 1U);
  (void)registry.destroy(material_green, 1U);
  registry.collect(1U);
  red_upload.destroy(h.allocator);
  green_upload.destroy(h.allocator);
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

//! The bindless albedo texture path in the lit scene: two cubes share one
//! white-vertex mesh; switching one cube's material to a registry texture
//! record (solid red 1x1, bindless element 1) turns that cube red under
//! lambert while its untextured neighbour stays neutral gray.
TEST(VulkanHardware, AlbedoTextureTintsLitMaterialScene) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  const float white[3] = {1.0f, 1.0f, 1.0f};
  const float opaque[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const CubeFixture cube = make_cube_gltf(white, opaque, "shared_cube");
  std::string detail;
  auto imported = import_gltf_mesh(cube.json.data(), cube.json.size(),
                                   cube.bin.data(), cube.bin.size(), 0U, &detail);
  ASSERT_TRUE(imported.is_ok()) << detail;
  const auto& gltf_import = imported.value();

  GpuSceneHarness h;
  if (!h.initialize("OmniCppAlbedoTint")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }
  GpuMeshUpload upload;
  ASSERT_TRUE(upload.create(h, gltf_import));
  SolidTexture red_tex;
  ASSERT_TRUE(make_solid_texture(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator,
      std::array<std::uint8_t, 4>{255U, 0U, 0U, 255U}, red_tex));
  ASSERT_TRUE(h.bind_albedo(red_tex, 1U));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_handle = registry.create_mesh(upload.mesh);
  const std::array<float, 4> opaque_white{1.0f, 1.0f, 1.0f, 1.0f};
  const auto plain_material = registry.create_material({opaque_white});
  const auto red_texture = registry.create_texture({red_tex.view, red_tex.sampler, 1U});
  const auto textured_material = registry.create_material({opaque_white, red_texture});
  ASSERT_TRUE(mesh_handle.valid());
  ASSERT_TRUE(plain_material.valid());
  ASSERT_TRUE(textured_material.valid());
  ASSERT_TRUE(red_texture.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto left_entity = world.create_entity();
  const auto right_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      left_entity, {nullptr, true, mesh_handle, plain_material, nullptr});
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      right_entity, {nullptr, true, mesh_handle, plain_material, nullptr});
  omnicpp::render::SceneTransformComponent left_transform;
  omnicpp::render::SceneTransformComponent right_transform;
  make_translation(-1.7f, 0.0f, -7.0f, left_transform.model);
  make_translation(1.7f, 0.0f, -7.0f, right_transform.model);
  world.add_component<omnicpp::render::SceneTransformComponent>(left_entity,
                                                                left_transform);
  world.add_component<omnicpp::render::SceneTransformComponent>(right_entity,
                                                                right_transform);

  const auto snapshot = [&]() {
    auto scene = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene.texture_set = h.albedo_set;
    return scene;
  };

  // Frame A: both cubes on the untextured white material: neutral gray, so no
  // channel dominates (the stray-rounding ceiling is generous).
  const auto untextured = h.render(snapshot());
  ASSERT_TRUE(untextured.submitted);
  // Frame B: switch only the right cube to the red-albedo material.
  world.get_component<omnicpp::render::SceneRenderableComponent>(right_entity)
      .material_handle = textured_material;
  const auto textured = h.render(snapshot());
  ASSERT_TRUE(textured.submitted);

  EXPECT_LT(untextured.red_dominant_pixels, 500U);
  EXPECT_GT(textured.red_dominant_pixels, 1500U);
  EXPECT_GT(textured.red_dominant_pixels,
            untextured.red_dominant_pixels + 1000U);
  EXPECT_NE(untextured.hash, textured.hash);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_handle, 1U);
  (void)registry.destroy(plain_material, 1U);
  (void)registry.destroy(textured_material, 1U);
  (void)registry.destroy(red_texture, 1U);
  registry.collect(1U);
  destroy_solid_texture(h.context.device(), h.allocator, red_tex);
  upload.destroy(h.allocator);
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

//! Whole-glTF-scene integration through import_gltf_scene: the document
//! carries one white cube mesh used by a two-node hierarchy (root translated
//! left + back, child inheriting a further +X from the root). Scene import
//! flattens two world-space instances of the ONE mesh; each instance gets a
//! different registry material (green left, red right) and both render under
//! lambert. Readback must show both colours, proving the scene-graph import
//! feeds the registry scene path.
TEST(VulkanHardware, GltfSceneGraphRendersInstances) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  const float white[3] = {1.0f, 1.0f, 1.0f};
  const float opaque[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const CubeFixture cube = make_cube_gltf(white, opaque, "shared_mesh");
  // Re-wrap the single-mesh document as a scene: node 0 = root (left, back),
  // node 1 = its child instancing the mesh another +X away (world x = +1.7).
  const std::string doc = cube.json;
  const std::string prefix =
      R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[)"
      R"({"name":"root","mesh":0,"children":[1],"translation":[-1.7,0.0,-7.0]},)"
      R"({"name":"right","mesh":0,"translation":[3.4,0.0,0.0]}],)";
  const std::string json = prefix +
      doc.substr(doc.find(R"("buffers")"));

  std::string detail;
  auto imported = omnicpp::asset::import_gltf_scene(
      json.data(), json.size(), cube.bin.data(), cube.bin.size(), 0U, &detail);
  ASSERT_TRUE(imported.is_ok()) << detail;
  const auto& scene = imported.value();
  ASSERT_EQ(scene.meshes.size(), 1U);  // shared mesh imported exactly once
  ASSERT_EQ(scene.nodes.size(), 2U);
  EXPECT_TRUE(scene.nodes[0].has_mesh);
  EXPECT_TRUE(scene.nodes[1].has_mesh);
  EXPECT_EQ(scene.nodes[0].mesh_index, 0U);
  EXPECT_EQ(scene.nodes[1].mesh_index, 0U);
  EXPECT_NEAR(scene.nodes[0].model[12], -1.7f, 0.001f);
  EXPECT_NEAR(scene.nodes[1].model[12], 1.7f, 0.001f);
  EXPECT_NEAR(scene.nodes[0].model[14], -7.0f, 0.001f);
  EXPECT_NEAR(scene.nodes[1].model[14], -7.0f, 0.001f);

  GpuSceneHarness h;
  if (!h.initialize("OmniCppSceneGraph")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }
  GpuMeshUpload upload;
  ASSERT_TRUE(upload.create(h, scene.meshes[0]));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_handle = registry.create_mesh(upload.mesh);
  const std::array<float, 4> green{0.0f, 1.0f, 0.0f, 1.0f};
  const std::array<float, 4> red{1.0f, 0.0f, 0.0f, 1.0f};
  const auto material_green = registry.create_material({green});
  const auto material_red = registry.create_material({red});
  ASSERT_TRUE(mesh_handle.valid());
  ASSERT_TRUE(material_green.valid());
  ASSERT_TRUE(material_red.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto left_entity = world.create_entity();
  const auto right_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      left_entity, {nullptr, true, mesh_handle, material_green, nullptr});
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      right_entity, {nullptr, true, mesh_handle, material_red, nullptr});
  omnicpp::render::SceneTransformComponent left_transform;
  omnicpp::render::SceneTransformComponent right_transform;
  std::copy(scene.nodes[0].model.begin(), scene.nodes[0].model.end(),
            left_transform.model.begin());
  std::copy(scene.nodes[1].model.begin(), scene.nodes[1].model.end(),
            right_transform.model.begin());
  world.add_component<omnicpp::render::SceneTransformComponent>(left_entity,
                                                                left_transform);
  world.add_component<omnicpp::render::SceneTransformComponent>(right_entity,
                                                                right_transform);

  auto snapshot = [&]() {
    auto scene_snapshot = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene_snapshot.texture_set = h.albedo_set;
    return scene_snapshot;
  };
  const auto frame = h.render(snapshot());
  ASSERT_TRUE(frame.submitted);
  EXPECT_GT(frame.green_dominant_pixels, 1200U);
  EXPECT_GT(frame.red_dominant_pixels, 1200U);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_handle, 1U);
  (void)registry.destroy(material_green, 1U);
  (void)registry.destroy(material_red, 1U);
  registry.collect(1U);
  upload.destroy(h.allocator);
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

//! One glTF mesh, two primitives (unit cubes offset in X), each with its own
//! baseColorTexture (data-URI PNGs). The importer records per-primitive
//! vertex/index slices and materials; this test uploads each primitive as its
//! own renderable (CPU-side slice copy) so both decoded textures reach the
//! framebuffer in one frame.
TEST(VulkanHardware, MultiMaterialGltfMeshRendersBothPrimitiveTextures) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  constexpr const char* kRedUri =
      "data:image/png;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
      "iZk9HQAAAABJRU5ErkJggg==";
  constexpr const char* kGreenUri =
      "data:image/png;base64,"
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNg+M/wHwAEAQH/"
      "cetH5QAAAABJRU5ErkJggg==";
  const TwoCubeFixture doc = make_two_cube_multimaterial_gltf(kRedUri, kGreenUri);
  std::string detail;
  auto imported = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                   doc.bin.data(), doc.bin.size(), 0U, &detail);
  ASSERT_TRUE(imported.is_ok()) << detail;
  const auto& mesh = imported.value();

  // Asset layer proof: every primitive recorded with its own texture binding.
  ASSERT_EQ(mesh.primitives.size(), 2U);
  ASSERT_EQ(mesh.images.size(), 2U);
  const auto& red_prim = mesh.primitives[0];
  const auto& green_prim = mesh.primitives[1];
  EXPECT_EQ(red_prim.vertex_offset, 0U);
  EXPECT_EQ(red_prim.vertex_count, 8U);
  EXPECT_EQ(red_prim.index_offset, 0U);
  EXPECT_EQ(red_prim.index_count, 36U);
  ASSERT_TRUE(red_prim.albedo.present);
  EXPECT_TRUE(red_prim.albedo.encoded_srgb);
  EXPECT_EQ(red_prim.albedo.image_index, 0U);
  EXPECT_EQ(green_prim.vertex_offset, 8U);
  EXPECT_EQ(green_prim.vertex_count, 8U);
  EXPECT_EQ(green_prim.index_offset, 36U);
  EXPECT_EQ(green_prim.index_count, 36U);
  ASSERT_TRUE(green_prim.albedo.present);
  EXPECT_EQ(green_prim.albedo.image_index, 1U);
  EXPECT_EQ(mesh.images[0].rgba[0], 255U);
  EXPECT_EQ(mesh.images[0].rgba[1], 0U);
  EXPECT_EQ(mesh.images[1].rgba[1], 255U);

  GpuSceneHarness h;
  if (!h.initialize("OmniCppMultiMaterial")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }

  // Upload each primitive's vertex/index slice as its own mesh (CPU slice).
  struct GpuSlice {
    omnicpp::render::Allocation vertex_allocation{};
    omnicpp::render::Allocation index_allocation{};
    omnicpp::render::SceneMesh mesh{};
  };
  GpuSlice slices[2];
  for (std::size_t p = 0; p < 2U; ++p) {
    const auto& prim = mesh.primitives[p];
    const std::size_t vertex_floats =
        prim.vertex_count * omnicpp::asset::kSceneVertexFloats;
    std::vector<float> slice_vertices(vertex_floats);
    std::memcpy(slice_vertices.data(),
                mesh.vertices.data() +
                    prim.vertex_offset * omnicpp::asset::kSceneVertexFloats,
                vertex_floats * sizeof(float));
    std::vector<std::uint32_t> slice_indices(prim.index_count);
    for (std::size_t i = 0; i < prim.index_count; ++i) {
      slice_indices[i] = mesh.indices[prim.index_offset + i] -
                         static_cast<std::uint32_t>(prim.vertex_offset);
    }
    auto vb = h.allocator.create_buffer(
        slice_vertices.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto ib = h.allocator.create_buffer(
        slice_indices.size() * sizeof(std::uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(vb.is_ok());
    ASSERT_TRUE(ib.is_ok());
    slices[p].vertex_allocation = vb.value();
    slices[p].index_allocation = ib.value();
    std::memcpy(slices[p].vertex_allocation.mapped, slice_vertices.data(),
                slice_vertices.size() * sizeof(float));
    std::memcpy(slices[p].index_allocation.mapped, slice_indices.data(),
                slice_indices.size() * sizeof(std::uint32_t));
    slices[p].mesh.vertex_buffer = slices[p].vertex_allocation.buffer;
    slices[p].mesh.index_buffer = slices[p].index_allocation.buffer;
    slices[p].mesh.index_count = static_cast<std::uint32_t>(prim.index_count);
    slices[p].mesh.descriptor_set =
        h.make_mesh_set(slices[p].vertex_allocation.buffer);
    ASSERT_NE(slices[p].mesh.descriptor_set, VK_NULL_HANDLE);
  }

  // Bindless albedo set: element 0 white fallback (harness), 1 red, 2 green.
  SolidTexture red_tex;
  SolidTexture green_tex;
  ASSERT_TRUE(make_texture_rgba(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator, mesh.images[0].width, mesh.images[0].height,
      mesh.images[0].rgba.data(), mesh.images[0].rgba.size(), red_tex));
  ASSERT_TRUE(make_texture_rgba(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator, mesh.images[1].width, mesh.images[1].height,
      mesh.images[1].rgba.data(), mesh.images[1].rgba.size(), green_tex));
  ASSERT_TRUE(h.bind_albedo(red_tex, 1U));
  ASSERT_TRUE(h.bind_albedo(green_tex, 2U));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_a = registry.create_mesh(slices[0].mesh);
  const auto mesh_b = registry.create_mesh(slices[1].mesh);
  const auto texture_red = registry.create_texture({red_tex.view, red_tex.sampler, 1U});
  const auto texture_green = registry.create_texture({green_tex.view, green_tex.sampler, 2U});
  const std::array<float, 4> opaque_white{1.0f, 1.0f, 1.0f, 1.0f};
  const auto material_red = registry.create_material({opaque_white, texture_red});
  const auto material_green = registry.create_material({opaque_white, texture_green});
  ASSERT_TRUE(mesh_a.valid());
  ASSERT_TRUE(mesh_b.valid());
  ASSERT_TRUE(material_red.valid());
  ASSERT_TRUE(material_green.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto left_entity = world.create_entity();
  const auto right_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  // Geometry places the red cube at x=0 and the green cube at x=+2.5; both are
  // pushed 7 units in front of the camera.
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      left_entity, {nullptr, true, mesh_a, material_red, nullptr});
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      right_entity, {nullptr, true, mesh_b, material_green, nullptr});
  omnicpp::render::SceneTransformComponent left_transform;
  omnicpp::render::SceneTransformComponent right_transform;
  make_translation(0.0f, 0.0f, -7.0f, left_transform.model);
  make_translation(0.0f, 0.0f, -7.0f, right_transform.model);
  world.add_component<omnicpp::render::SceneTransformComponent>(left_entity,
                                                                left_transform);
  world.add_component<omnicpp::render::SceneTransformComponent>(right_entity,
                                                                right_transform);

  auto snapshot = [&]() {
    auto scene_snapshot = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene_snapshot.texture_set = h.albedo_set;
    return scene_snapshot;
  };
  const auto frame = h.render(snapshot());
  ASSERT_TRUE(frame.submitted);

  // Both per-primitive baseColorTextures reached the screen in one frame.
  EXPECT_GT(frame.red_dominant_pixels, 1200U);
  EXPECT_GT(frame.green_dominant_pixels, 1200U);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_a, 1U);
  (void)registry.destroy(mesh_b, 1U);
  (void)registry.destroy(material_red, 1U);
  (void)registry.destroy(material_green, 1U);
  (void)registry.destroy(texture_red, 1U);
  (void)registry.destroy(texture_green, 1U);
  registry.collect(1U);
  destroy_solid_texture(h.context.device(), h.allocator, red_tex);
  destroy_solid_texture(h.context.device(), h.allocator, green_tex);
  for (std::size_t p = 0; p < 2U; ++p) {
    if (slices[p].vertex_allocation.is_valid()) {
      h.allocator.destroy_allocation(slices[p].vertex_allocation);
    }
    if (slices[p].index_allocation.is_valid()) {
      h.allocator.destroy_allocation(slices[p].index_allocation);
    }
  }
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}

//! 1x1 pure red PNG payload (external file content).
constexpr std::uint8_t kRed1x1Png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

//! 1x1 pure green PNG payload (external file content).
constexpr std::uint8_t kGreen1x1Png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0xf8, 0xcf, 0xf0,
    0x1f, 0x00, 0x04, 0x01, 0x01, 0xff, 0x71, 0xeb, 0x47, 0xe5, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

//! Render-layer per-material splitting: the same two-primitive glTF mesh, but
//! with its baseColorTextures delivered as *external image files* through the
//! importer's ExternalFileLoader. The upload registers per-primitive
//! SceneMesh records that SHARE the one merged vertex/index buffer (submesh
//! ranges via index_offset) and per-primitive materials — so one glTF mesh
//! renders as N scene draws without CPU-side geometry copies.
TEST(VulkanHardware, MultiMaterialMeshSplitsIntoSharedBufferDraws) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  const TwoCubeFixture doc = make_two_cube_multimaterial_gltf(
      "textures/red.png", "textures/green.png");
  std::size_t loader_calls = 0;
  omnicpp::asset::ExternalFileLoader loader =
      [&](const std::string& uri, std::string& error,
          std::vector<std::uint8_t>& bytes) -> bool {
        ++loader_calls;
        if (uri == "textures/red.png") {
          bytes.assign(std::begin(kRed1x1Png), std::end(kRed1x1Png));
          return true;
        }
        if (uri == "textures/green.png") {
          bytes.assign(std::begin(kGreen1x1Png), std::end(kGreen1x1Png));
          return true;
        }
        error = "unexpected uri '" + uri + "'";
        return false;
      };
  std::string detail;
  auto imported = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                   doc.bin.data(), doc.bin.size(), 0U, &detail,
                                   &loader);
  ASSERT_TRUE(imported.is_ok()) << detail;
  const auto& mesh = imported.value();
  ASSERT_EQ(mesh.primitives.size(), 2U);
  ASSERT_EQ(mesh.images.size(), 2U);
  EXPECT_EQ(mesh.primitives[0].index_offset, 0U);
  EXPECT_EQ(mesh.primitives[0].index_count, 36U);
  EXPECT_EQ(mesh.primitives[1].index_offset, 36U);
  EXPECT_EQ(mesh.primitives[1].index_count, 36U);
  EXPECT_TRUE(mesh.primitives[0].albedo.encoded_srgb);
  EXPECT_TRUE(mesh.primitives[1].albedo.encoded_srgb);
  EXPECT_EQ(mesh.images[0].rgba[0], 255U);
  EXPECT_EQ(mesh.images[1].rgba[1], 255U);
  EXPECT_EQ(loader_calls, 2U);

  GpuSceneHarness h;
  if (!h.initialize("OmniCppSplitDraws")) {
    GTEST_SKIP() << "Device does not support descriptor indexing (bindless)";
  }

  // ONE merged vertex/index buffer pair + ONE descriptor set, shared by both
  // per-primitive SceneMesh submesh records (index_offset in bytes).
  const VkDeviceSize vertex_bytes = mesh.vertices.size() * sizeof(float);
  const VkDeviceSize index_bytes = mesh.indices.size() * sizeof(std::uint32_t);
  auto vertex_buffer = h.allocator.create_buffer(
      vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto index_buffer = h.allocator.create_buffer(
      index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(vertex_buffer.is_ok());
  ASSERT_TRUE(index_buffer.is_ok());
  omnicpp::render::Allocation vertex_allocation = vertex_buffer.value();
  omnicpp::render::Allocation index_allocation = index_buffer.value();
  std::memcpy(vertex_allocation.mapped, mesh.vertices.data(), vertex_bytes);
  std::memcpy(index_allocation.mapped, mesh.indices.data(), index_bytes);
  const VkDescriptorSet shared_set = h.make_mesh_set(vertex_allocation.buffer);
  ASSERT_NE(shared_set, VK_NULL_HANDLE);

  omnicpp::render::SceneMesh red_submesh;
  red_submesh.vertex_buffer = vertex_allocation.buffer;
  red_submesh.index_buffer = index_allocation.buffer;
  red_submesh.descriptor_set = shared_set;
  red_submesh.index_offset =
      mesh.primitives[0].index_offset * sizeof(std::uint32_t);
  red_submesh.index_count =
      static_cast<std::uint32_t>(mesh.primitives[0].index_count);
  omnicpp::render::SceneMesh green_submesh = red_submesh;
  green_submesh.index_offset =
      mesh.primitives[1].index_offset * sizeof(std::uint32_t);
  green_submesh.index_count =
      static_cast<std::uint32_t>(mesh.primitives[1].index_count);
  ASSERT_TRUE(red_submesh.is_drawable());
  ASSERT_TRUE(green_submesh.is_drawable());

  // Both glTF colour textures upload through the sRGB colour path.
  SolidTexture red_tex;
  SolidTexture green_tex;
  ASSERT_TRUE(make_texture_rgba(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator, mesh.images[0].width, mesh.images[0].height,
      mesh.images[0].rgba.data(), mesh.images[0].rgba.size(), red_tex,
      /*srgb_format=*/true));
  ASSERT_TRUE(make_texture_rgba(
      h.context.device(), h.context.physical_device(), h.context.graphics_queue(),
      h.queue_family, h.allocator, mesh.images[1].width, mesh.images[1].height,
      mesh.images[1].rgba.data(), mesh.images[1].rgba.size(), green_tex,
      /*srgb_format=*/true));
  ASSERT_TRUE(h.bind_albedo(red_tex, 1U));
  ASSERT_TRUE(h.bind_albedo(green_tex, 2U));

  omnicpp::render::VulkanSceneResourceRegistry registry;
  const auto mesh_a = registry.create_mesh(red_submesh);
  const auto mesh_b = registry.create_mesh(green_submesh);
  const auto texture_red = registry.create_texture({red_tex.view, red_tex.sampler, 1U});
  const auto texture_green = registry.create_texture({green_tex.view, green_tex.sampler, 2U});
  const std::array<float, 4> opaque_white{1.0f, 1.0f, 1.0f, 1.0f};
  const auto material_red = registry.create_material({opaque_white, texture_red});
  const auto material_green = registry.create_material({opaque_white, texture_green});
  ASSERT_TRUE(mesh_a.valid());
  ASSERT_TRUE(mesh_b.valid());
  ASSERT_TRUE(material_red.valid());
  ASSERT_TRUE(material_green.valid());

  omnicpp::core::World world;
  const auto camera_entity = world.create_entity();
  const auto left_entity = world.create_entity();
  const auto right_entity = world.create_entity();
  omnicpp::render::SceneCameraComponent camera_component;
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, camera_component.view_projection);
  world.add_component<omnicpp::render::SceneCameraComponent>(camera_entity,
                                                             camera_component);
  // One mesh instance split into two draws: same transform for both (the
  // geometry already carries the per-cube X offsets).
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      left_entity, {nullptr, true, mesh_a, material_red, nullptr});
  world.add_component<omnicpp::render::SceneRenderableComponent>(
      right_entity, {nullptr, true, mesh_b, material_green, nullptr});
  omnicpp::render::SceneTransformComponent left_transform;
  omnicpp::render::SceneTransformComponent right_transform;
  make_translation(0.0f, 0.0f, -7.0f, left_transform.model);
  make_translation(0.0f, 0.0f, -7.0f, right_transform.model);
  world.add_component<omnicpp::render::SceneTransformComponent>(left_entity,
                                                                left_transform);
  world.add_component<omnicpp::render::SceneTransformComponent>(right_entity,
                                                                right_transform);

  auto snapshot = [&]() {
    auto scene_snapshot = omnicpp::render::extract_vulkan_scene(
        world, registry, h.pipeline.pipeline(), h.pipeline.pipeline_layout());
    scene_snapshot.texture_set = h.albedo_set;
    return scene_snapshot;
  };
  const auto frame = h.render(snapshot());
  ASSERT_TRUE(frame.submitted);

  // Both per-primitive submesh draws (sharing one merged buffer) reached the
  // screen in one frame: left cube red-dominant, right cube green-dominant.
  EXPECT_GT(frame.red_dominant_pixels, 1200U);
  EXPECT_GT(frame.green_dominant_pixels, 1200U);
  EXPECT_EQ(h.context.validation_error_count(), 0U);
  EXPECT_EQ(h.context.validation_warning_count(), 0U);

  (void)registry.destroy(mesh_a, 1U);
  (void)registry.destroy(mesh_b, 1U);
  (void)registry.destroy(material_red, 1U);
  (void)registry.destroy(material_green, 1U);
  (void)registry.destroy(texture_red, 1U);
  (void)registry.destroy(texture_green, 1U);
  registry.collect(1U);
  destroy_solid_texture(h.context.device(), h.allocator, red_tex);
  destroy_solid_texture(h.context.device(), h.allocator, green_tex);
  h.allocator.destroy_allocation(vertex_allocation);
  h.allocator.destroy_allocation(index_allocation);
  h.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
