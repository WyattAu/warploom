//! @file test_gltf_importer.cpp
//! @brief Deterministic CPU tests for the dependency-free glTF 2.0 importer:
//!        position/color/normal/uv decode, index conversion, stride handling,
//!        primitive merging, embedded buffers, and strict rejection of
//!        malformed data.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "warploom/asset/gltf_importer.hpp"

namespace {

using omnicpp::asset::GltfMeshImport;
using omnicpp::asset::import_gltf_mesh;
using omnicpp::asset::kSceneVertexFloats;
using omnicpp::core::RuntimeError;

// Vertex layout offsets within the eleven-float canonical stream.
constexpr std::size_t kPos = 0U;   // position.xyz
constexpr std::size_t kCol = 3U;   // color.rgb
constexpr std::size_t kNrm = 6U;   // normal.xyz
constexpr std::size_t kUv = 9U;    // uv.xy

//! Little-endian float pusher into a binary buffer under construction.
struct BinBuilder {
  std::vector<std::uint8_t> data;

  std::size_t push_floats(const std::vector<float>& values) {
    const std::size_t offset = data.size();
    data.resize(data.size() + values.size() * sizeof(float));
    std::memcpy(data.data() + offset, values.data(),
                values.size() * sizeof(float));
    return offset;
  }

  std::size_t push_u16(const std::vector<std::uint16_t>& values) {
    const std::size_t offset = data.size();
    for (const std::uint16_t value : values) {
      data.push_back(static_cast<std::uint8_t>(value & 0xFFU));
      data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    }
    return offset;
  }

  std::size_t push_u32(const std::vector<std::uint32_t>& values) {
    const std::size_t offset = data.size();
    for (const std::uint32_t value : values) {
      for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        data.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
      }
    }
    return offset;
  }
};

std::string make_document(std::string buffers_json, std::string views_json,
                          std::string accessors_json,
                          std::string meshes_json,
                          std::string materials_json = "[]") {
  std::string doc = R"({"asset":{"version":"2.0"},"buffers":)";
  doc += buffers_json;
  doc += R"(,"bufferViews":)";
  doc += views_json;
  doc += R"(,"accessors":)";
  doc += accessors_json;
  doc += R"(,"materials":)";
  doc += materials_json;
  doc += R"(,"meshes":)";
  doc += meshes_json;
  doc += "}";
  return doc;
}

std::string buffer_view(std::size_t byte_offset, std::size_t byte_length,
                        const char* extra = "") {
  std::string view = R"({"buffer":0,"byteOffset":)";
  view += std::to_string(byte_offset);
  view += R"(,"byteLength":)";
  view += std::to_string(byte_length);
  if (extra[0] != '\0') {
    view += ",";
    view += extra;
  }
  view += "}";
  return view;
}

std::string accessor(std::size_t view_index, std::size_t byte_offset,
                     std::int64_t component_type, std::size_t count,
                     const char* type, const char* extra = "") {
  std::string acc = R"({"bufferView":)";
  acc += std::to_string(view_index);
  acc += R"(,"byteOffset":)";
  acc += std::to_string(byte_offset);
  acc += R"(,"componentType":)";
  acc += std::to_string(component_type);
  acc += R"(,"count":)";
  acc += std::to_string(count);
  acc += R"(,"type":")";
  acc += type;
  acc += "\"";
  if (extra[0] != '\0') {
    acc += ",";
    acc += extra;
  }
  acc += "}";
  return acc;
}

//! Document-level buffer view length that is reused across fixtures.
std::string external_buffer(std::size_t byte_length) {
  return R"([{"byteLength":)" + std::to_string(byte_length) +
         R"(,"uri":"scene.bin"}])";
}

//! glTF-escaped string view of a primitive JSON built from raw members.
std::string primitive_from(std::string attributes_json,
                           const char* extra = "") {
  std::string prim = R"({"attributes":{)";
  prim += attributes_json;
  prim += "}";
  if (extra[0] != '\0') {
    prim += ",";
    prim += extra;
  }
  prim += "}";
  return prim;
}

}  // namespace

// ============================================================================
// Valid imports
// ============================================================================

//! Full-feature quad: POSITION + COLOR_0 + NORMAL + TEXCOORD_0 + uint16
//! indices + a tinted material. Every vertex slot must decode correctly.
TEST(GltfImporter, FullQuadWithAllAttributes) {
  BinBuilder bin;
  const std::vector<float> positions = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f,
                                        1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f};
  const std::vector<float> colors = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                     0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f};
  const std::vector<float> normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  const std::vector<float> uvs = {0.0f, 0.0f, 1.0f, 0.0f,
                                  1.0f, 1.0f, 0.0f, 1.0f};
  const std::vector<std::uint16_t> indices = {0, 1, 2, 0, 2, 3};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::size_t col_offset = bin.push_floats(colors);
  const std::size_t nrm_offset = bin.push_floats(normals);
  const std::size_t uv_offset = bin.push_floats(uvs);
  const std::size_t idx_offset = bin.push_u16(indices);
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(pos_offset, 4U * 3U * 4U) + "," +
          buffer_view(col_offset, 4U * 3U * 4U) + "," +
          buffer_view(nrm_offset, 4U * 3U * 4U) + "," +
          buffer_view(uv_offset, 4U * 2U * 4U) + "," +
          buffer_view(idx_offset, 6U * 2U) + "]",
      "[" + accessor(0, 0, 5126, 4, "VEC3") + "," +
          accessor(1, 0, 5126, 4, "VEC3") + "," +
          accessor(2, 0, 5126, 4, "VEC3") + "," +
          accessor(3, 0, 5126, 4, "VEC2") + "," +
          accessor(4, 0, 5123, 6, "SCALAR") + "]",
      std::string("[{\"name\":\"quad\",\"primitives\":[") +
          primitive_from(R"("POSITION":0,"COLOR_0":1,"NORMAL":2,"TEXCOORD_0":3)",
                         R"("indices":4,"material":0)") +
          "]}]",
      R"([{"pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,0.125,1.0]}}])");

  std::string detail;
  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size(), 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();
  EXPECT_EQ(mesh.name, "quad");
  EXPECT_EQ(mesh.vertex_count(), 4U);
  ASSERT_EQ(mesh.vertices.size(), 4U * kSceneVertexFloats);
  ASSERT_EQ(mesh.indices.size(), 6U);
  for (std::size_t i = 0; i < 4U; ++i) {
    const std::size_t base = i * kSceneVertexFloats;
    EXPECT_FLOAT_EQ(mesh.vertices[base + kPos + 0U], positions[i * 3U + 0U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kPos + 1U], positions[i * 3U + 1U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kPos + 2U], positions[i * 3U + 2U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 0U], colors[i * 3U + 0U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 1U], colors[i * 3U + 1U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 2U], colors[i * 3U + 2U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 0U], normals[i * 3U + 0U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 1U], normals[i * 3U + 1U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 2U], normals[i * 3U + 2U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kUv + 0U], uvs[i * 2U + 0U]);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kUv + 1U], uvs[i * 2U + 1U]);
  }
  for (std::size_t i = 0; i < 6U; ++i) {
    EXPECT_EQ(mesh.indices[i], static_cast<std::uint32_t>(indices[i]));
  }
  EXPECT_FLOAT_EQ(mesh.base_color[0], 0.5f);
  EXPECT_FLOAT_EQ(mesh.base_color[1], 0.25f);
  EXPECT_FLOAT_EQ(mesh.base_color[2], 0.125f);
  EXPECT_FLOAT_EQ(mesh.base_color[3], 1.0f);
}

//! Missing optional attributes default: white color, +Z normal, zero UV.
TEST(GltfImporter, MissingAttributesGetDefaults) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::vector<std::uint16_t> indices = {0, 1, 2};
  const std::size_t idx_offset = bin.push_u16(indices);
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(pos_offset, 3U * 3U * 4U) + "," +
          buffer_view(idx_offset, 3U * 2U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5123, 3, "SCALAR") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("indices":1)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  const GltfMeshImport& mesh = result.value();
  ASSERT_EQ(mesh.vertex_count(), 3U);
  for (std::size_t i = 0; i < 3U; ++i) {
    const std::size_t base = i * kSceneVertexFloats;
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 0U], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 1U], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 2U], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 0U], 0.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 1U], 0.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 2U], 1.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kUv + 0U], 0.0f);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kUv + 1U], 0.0f);
  }
  EXPECT_FLOAT_EQ(mesh.base_color[0], 1.0f);
}

//! Non-indexed primitives become sequential indices.
TEST(GltfImporter, NonIndexedPrimitiveBecomesSequential) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f, 2.0f, 0.0f, 0.0f,
                                        0.0f, 2.0f, 0.0f, 1.0f, 2.0f, 0.0f};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(pos_offset, 6U * 3U * 4U) + "]",
      "[" + accessor(0, 0, 5126, 6, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  const GltfMeshImport& mesh = result.value();
  ASSERT_EQ(mesh.indices.size(), 6U);
  for (std::size_t i = 0; i < 6U; ++i) {
    EXPECT_EQ(mesh.indices[i], static_cast<std::uint32_t>(i));
  }
  EXPECT_EQ(mesh.vertex_count(), 6U);
}

//! Multiple primitives merge into one vertex stream with re-based indices.
TEST(GltfImporter, MultiPrimitiveMergeRebasesIndices) {
  BinBuilder bin;
  const std::vector<float> first = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                    0.0f, 1.0f, 0.0f};
  const std::vector<float> first_colors = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                           1.0f, 0.0f, 0.0f};
  const std::vector<float> second = {5.0f, 5.0f, 5.0f, 6.0f, 5.0f, 5.0f,
                                     5.0f, 6.0f, 5.0f};
  const std::vector<std::uint16_t> first_indices = {0, 1, 2};
  const std::vector<std::uint16_t> second_indices = {0, 1, 2};
  const std::size_t f_pos = bin.push_floats(first);
  const std::size_t f_col = bin.push_floats(first_colors);
  const std::size_t s_pos = bin.push_floats(second);
  const std::size_t i0 = bin.push_u16(first_indices);
  const std::size_t i1 = bin.push_u16(second_indices);
  // Primitive 0 binds POSITION + COLOR_0; primitive 1 binds POSITION only and
  // no material, so its vertices default to white.
  const std::string first_primitive =
      primitive_from(R"("POSITION":0,"COLOR_0":1)", R"("indices":3)");
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(f_pos, 3U * 3U * 4U) + "," +
          buffer_view(f_col, 3U * 3U * 4U) + "," +
          buffer_view(s_pos, 3U * 3U * 4U) + "," +
          buffer_view(i0, 3U * 2U) + "," + buffer_view(i1, 3U * 2U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5126, 3, "VEC3") + "," +
          accessor(2, 0, 5126, 3, "VEC3") + "," +
          accessor(3, 0, 5123, 3, "SCALAR") + "," +
          accessor(4, 0, 5123, 3, "SCALAR") + "]",
      std::string(R"([{"primitives":[)") + first_primitive + "," +
          primitive_from(R"("POSITION":2)", R"("indices":4)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  const GltfMeshImport& mesh = result.value();
  EXPECT_EQ(mesh.vertex_count(), 6U);
  ASSERT_EQ(mesh.indices.size(), 6U);
  EXPECT_EQ(mesh.indices[0], 0U);
  EXPECT_EQ(mesh.indices[1], 1U);
  EXPECT_EQ(mesh.indices[2], 2U);
  // Second primitive indices re-based by the first primitive's vertex count.
  EXPECT_EQ(mesh.indices[3], 3U);
  EXPECT_EQ(mesh.indices[4], 4U);
  EXPECT_EQ(mesh.indices[5], 5U);
  // First primitive red (r=1,g=0), second primitive default white (1,1,1).
  EXPECT_FLOAT_EQ(mesh.vertices[0U * kSceneVertexFloats + kCol], 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[0U * kSceneVertexFloats + kCol + 1U], 0.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[3U * kSceneVertexFloats + kCol], 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[3U * kSceneVertexFloats + kCol + 1U], 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[3U * kSceneVertexFloats + kPos], 5.0f);
}

//! uint32 index stream is supported.
TEST(GltfImporter, UInt32Indices) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::vector<std::uint32_t> indices = {0U, 1U, 2U};
  const std::size_t idx_offset = bin.push_u32(indices);
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(pos_offset, 3U * 3U * 4U) + "," +
          buffer_view(idx_offset, 3U * 4U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5125, 3, "SCALAR") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("indices":1)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.value().indices.size(), 3U);
  EXPECT_EQ(result.value().indices[2], 2U);
}

//! Interleaved vertex data via bufferView.byteStride across all four
//! attributes (stride 11 floats).
TEST(GltfImporter, InterleavedByteStride) {
  struct Vertex {
    float px, py, pz, cr, cg, cb, nx, ny, nz, u, v;
  };
  const std::vector<Vertex> vertices = {
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f}};
  BinBuilder bin;
  for (const Vertex& v : vertices) {
    bin.push_floats({v.px, v.py, v.pz, v.cr, v.cg, v.cb, v.nx, v.ny, v.nz,
                     v.u, v.v});
  }
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(0, bin.data.size(),
                        R"("byteStride":44)") + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(0, 12, 5126, 3, "VEC3") + "," +
          accessor(0, 24, 5126, 3, "VEC3") + "," +
          accessor(0, 36, 5126, 3, "VEC2") + "]",
      R"([{"primitives":[)" +
          primitive_from(
              R"("POSITION":0,"COLOR_0":1,"NORMAL":2,"TEXCOORD_0":3)") +
          "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  const GltfMeshImport& mesh = result.value();
  ASSERT_EQ(mesh.vertex_count(), 3U);
  for (std::size_t i = 0; i < 3U; ++i) {
    const std::size_t base = i * kSceneVertexFloats;
    EXPECT_FLOAT_EQ(mesh.vertices[base + kPos + 0U], vertices[i].px);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kPos + 1U], vertices[i].py);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kPos + 2U], vertices[i].pz);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 0U], vertices[i].cr);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 1U], vertices[i].cg);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kCol + 2U], vertices[i].cb);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 0U], vertices[i].nx);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 1U], vertices[i].ny);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kNrm + 2U], vertices[i].nz);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kUv + 0U], vertices[i].u);
    EXPECT_FLOAT_EQ(mesh.vertices[base + kUv + 1U], vertices[i].v);
  }
}

//! Fully embedded document: buffer data comes from a data: base64 URI.
TEST(GltfImporter, EmbeddedBase64Buffer) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f};
  bin.push_floats(positions);
  const std::string json = make_document(
      R"([{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}])",
      "[" + buffer_view(0, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), nullptr, 0U);
  ASSERT_TRUE(result.is_ok());
  const GltfMeshImport& mesh = result.value();
  EXPECT_EQ(mesh.vertex_count(), 3U);
  EXPECT_FLOAT_EQ(mesh.vertices[0U * kSceneVertexFloats + kPos + 0U], 0.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[1U * kSceneVertexFloats + kPos + 0U], 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[2U * kSceneVertexFloats + kPos + 1U], 1.0f);
}

//! Determinism: identical inputs yield identical outputs.
TEST(GltfImporter, DeterministicOutput) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(pos_offset, 3U * 3U * 4U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");

  const auto first = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                      bin.data.size());
  const auto second = import_gltf_mesh(json.data(), json.size(),
                                       bin.data.data(), bin.data.size());
  ASSERT_TRUE(first.is_ok());
  ASSERT_TRUE(second.is_ok());
  EXPECT_EQ(first.value().vertices, second.value().vertices);
  EXPECT_EQ(first.value().indices, second.value().indices);
  EXPECT_EQ(first.value().base_color, second.value().base_color);
}

//! Mesh selection by index.
TEST(GltfImporter, SelectsRequestedMesh) {
  BinBuilder bin;
  const std::vector<float> first = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                    0.0f, 1.0f, 0.0f};
  const std::vector<float> second = {3.0f, 3.0f, 3.0f, 4.0f, 3.0f, 3.0f,
                                     3.0f, 4.0f, 3.0f};
  const std::size_t f_pos = bin.push_floats(first);
  const std::size_t s_pos = bin.push_floats(second);
  const std::string meshes =
      "[{\"name\":\"zero\",\"primitives\":[" + primitive_from(R"("POSITION":0)") +
      "]},{\"name\":\"one\",\"primitives\":[" + primitive_from(R"("POSITION":1)") + "]}]";
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(f_pos, 3U * 3U * 4U) + "," +
          buffer_view(s_pos, 3U * 3U * 4U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5126, 3, "VEC3") + "]",
      meshes);

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size(), 1U);
  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.value().name, "one");
  EXPECT_FLOAT_EQ(result.value().vertices[0U], 3.0f);
}

// ============================================================================
// Rejection of malformed input
// ============================================================================

namespace {

void expect_malformed(const std::string& json, const void* bin,
                      std::size_t bin_len,
                      const std::vector<std::string>& fragments,
                      std::size_t mesh_index = 0) {
  std::string detail;
  auto result = import_gltf_mesh(json.data(), json.size(),
                                 static_cast<const std::uint8_t*>(bin), bin_len,
                                 mesh_index, &detail);
  ASSERT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
  ASSERT_FALSE(detail.empty()) << "expected a diagnostic message";
  bool matched = false;
  for (const std::string& fragment : fragments) {
    if (detail.find(fragment) != std::string::npos) matched = true;
  }
  EXPECT_TRUE(matched) << "detail '" << detail << "' matched none of:";
  for (const std::string& fragment : fragments) {
    std::cerr << "  - " << fragment << "\n";
  }
}

}  // namespace

TEST(GltfImporter, RejectsBrokenJson) {
  expect_malformed("{not json", nullptr, 0U, {"JSON parse error"});
  expect_malformed(R"({"asset":{"version":"2.0"}} trailing)", nullptr, 0U,
                   {"trailing content"});
  expect_malformed("", nullptr, 0U, {"empty glTF document"});
  expect_malformed(R"({"asset":{"version":"2.0"}})", nullptr, 0U,
                   {"missing buffers"});
}

TEST(GltfImporter, RejectsWrongAssetVersion) {
  const std::string json = R"({"asset":{"version":"1.0"},"buffers":[],"bufferViews":[],"accessors":[],"meshes":[{"primitives":[{"attributes":{}}]}]})";
  expect_malformed(json, nullptr, 0U, {"asset.version"});
}

TEST(GltfImporter, RejectsMeshIndexOutOfRange) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::string json = make_document(
      R"([{"byteLength":36,"uri":"scene.bin"}])",
      "[" + buffer_view(0, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(), {"out of range"}, 5U);
}

TEST(GltfImporter, RejectsUnsupportedComponentType) {
  BinBuilder bin;
  bin.data.resize(3U);  // 3 signed bytes
  const std::string json = make_document(
      R"([{"byteLength":3,"uri":"scene.bin"}])",
      "[" + buffer_view(0, 3U) + "]",
      "[" + accessor(0, 0, 5120, 3, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  // Signed-byte componentType is rejected while parsing the accessor.
  expect_malformed(json, bin.data.data(), bin.data.size(), {"not supported"});
}

TEST(GltfImporter, RejectsWrongNormalType) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::size_t nrm_offset = bin.push_floats(
      {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f});
  const std::string json = make_document(
      R"([{"byteLength":72,"uri":"scene.bin"}])",
      "[" + buffer_view(pos_offset, 36U) + "," +
          buffer_view(nrm_offset, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5123, 3, "SCALAR") + "]",
      R"([{"primitives":[)" +
          primitive_from(R"("POSITION":0,"NORMAL":1)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"NORMAL must be a VEC3 float32"});
}

TEST(GltfImporter, RejectsWrongUvType) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  // Four-component texcoord accessor over float data.
  const std::size_t uv_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
       0.0f});
  const std::string json = make_document(
      R"([{"byteLength":84,"uri":"scene.bin"}])",
      "[" + buffer_view(pos_offset, 36U) + "," +
          buffer_view(uv_offset, 48U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5126, 3, "VEC4") + "]",
      R"([{"primitives":[)" +
          primitive_from(R"("POSITION":0,"TEXCOORD_0":1)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"TEXCOORD_0 must be a VEC2 float32"});
}

TEST(GltfImporter, RejectsAttributeCountMismatch) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  // Only two normals for three positions.
  const std::size_t nrm_offset =
      bin.push_floats({0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f});
  const std::string json = make_document(
      R"([{"byteLength":60,"uri":"scene.bin"}])",
      "[" + buffer_view(pos_offset, 36U) + "," +
          buffer_view(nrm_offset, 24U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5126, 2, "VEC3") + "]",
      R"([{"primitives":[)" +
          primitive_from(R"("POSITION":0,"NORMAL":1)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"does not match POSITION count"});
}

TEST(GltfImporter, RejectsUnsupportedPrimitiveMode) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::string json = make_document(
      R"([{"byteLength":36,"uri":"scene.bin"}])",
      "[" + buffer_view(0, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("mode":1)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(), {"only TRIANGLES"});
}

TEST(GltfImporter, RejectsIndexRegionBeyondView) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::size_t idx_offset = bin.push_u16({0, 1, 2});
  // Index view claims only 4 bytes even though 6 live there.
  const std::string json = make_document(
      R"([{"byteLength":42,"uri":"scene.bin"}])",
      "[" + buffer_view(pos_offset, 36U) + "," +
          buffer_view(idx_offset, 4U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5123, 3, "SCALAR") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("indices":1)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"extends past bufferView"});
}

TEST(GltfImporter, RejectsSparseAccessors) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::string json = make_document(
      R"([{"byteLength":36,"uri":"scene.bin"}])",
      "[" + buffer_view(0, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3",
                     R"("sparse":{"count":1,"indices":{"bufferView":0},"values":{"bufferView":0}})") +
          "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(), {"sparse accessors"});
}

TEST(GltfImporter, RejectsOutOfRangeIndexValue) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::size_t idx_offset = bin.push_u16({0, 1, 99});
  const std::string json = make_document(
      R"([{"byteLength":42,"uri":"scene.bin"}])",
      "[" + buffer_view(pos_offset, 36U) + "," +
          buffer_view(idx_offset, 6U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5123, 3, "SCALAR") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("indices":1)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(), {"out of range for"});
}

TEST(GltfImporter, RejectsBufferLengthMismatch) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f});
  const std::string json = make_document(
      R"([{"byteLength":99,"uri":"scene.bin"}])",  // declared larger
      "[" + buffer_view(0, 12U) + "]",
      "[" + accessor(0, 0, 5126, 1, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"does not match declared byteLength"});
}

TEST(GltfImporter, RejectsNonTriangleVertexCount) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
  const std::string json = make_document(
      R"([{"byteLength":24,"uri":"scene.bin"}])",
      "[" + buffer_view(0, 24U) + "]",
      "[" + accessor(0, 0, 5126, 2, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(), {"not a multiple of 3"});
}

TEST(GltfImporter, RejectsMaterialIndexOutOfRange) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::string json = make_document(
      R"([{"byteLength":36,"uri":"scene.bin"}])",
      "[" + buffer_view(pos_offset, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("material":7)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(), {"material"});
}

TEST(GltfImporter, RejectsExternalBufferBeyondFirst) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f});
  const std::string json =
      R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":12,"uri":"a.bin"},{"byteLength":4,"uri":"b.bin"}],"bufferViews":[{"buffer":1,"byteLength":4}],"accessors":[{"bufferView":0,"componentType":5126,"count":1,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]})";
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"only the first external buffer"});
}

TEST(GltfImporter, RejectsStrideSmallerThanElement) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
  const std::string json = make_document(
      R"([{"byteLength":24,"uri":"scene.bin"}])",
      "[" + buffer_view(0, 24U, R"("byteStride":8)") + "]",
      "[" + accessor(0, 0, 5126, 2, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(), {"byteStride"});
}

TEST(GltfImporter, RejectsStridedIndexView) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  // Indices stride 4 -> the 3-element region needs 10 bytes so it fits the
  // view; only the strided-index prohibition should fire.
  const std::size_t idx_offset = bin.push_u16({0, 1, 2, 0, 0});
  const std::string json = make_document(
      R"([{"byteLength":46,"uri":"scene.bin"}])",
      "[" + buffer_view(pos_offset, 36U) + "," +
          buffer_view(idx_offset, 10U, R"("byteStride":4)") + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5123, 3, "SCALAR") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("indices":1)") + "]}]");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"must not define byteStride"});
}

TEST(GltfImporter, RejectsBadBase64) {
  const std::string json = make_document(
      R"([{"byteLength":4,"uri":"data:application/octet-stream;base64,!!!!"}])",
      "[" + buffer_view(0, 4U) + "]",
      "[" + accessor(0, 0, 5126, 1, "VEC3") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(json, nullptr, 0U, {"base64"});
}

//! POSITION min/max parse into the imported local-space bounds.
TEST(GltfImporter, ImportsPositionBounds) {
  BinBuilder bin;
  const std::size_t pos_offset = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(pos_offset, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3",
                     R"("min":[0,0,0],"max":[1,1,0])") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  const auto& bounds = result.value().bounds;
  EXPECT_TRUE(bounds.valid);
  EXPECT_FLOAT_EQ(bounds.min[0], 0.0f);
  EXPECT_FLOAT_EQ(bounds.min[1], 0.0f);
  EXPECT_FLOAT_EQ(bounds.min[2], 0.0f);
  EXPECT_FLOAT_EQ(bounds.max[0], 1.0f);
  EXPECT_FLOAT_EQ(bounds.max[1], 1.0f);
  EXPECT_FLOAT_EQ(bounds.max[2], 0.0f);
}

//! Bounds union across primitives and union of the two boxes.
TEST(GltfImporter, BoundsUnionAcrossPrimitives) {
  BinBuilder bin;
  const std::size_t f_pos = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::size_t s_pos = bin.push_floats(
      {2.0f, 2.0f, 2.0f, 4.0f, 4.0f, 4.0f, 3.0f, 2.0f, 2.0f});
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(f_pos, 36U) + "," + buffer_view(s_pos, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3",
                     R"("min":[0,0,0],"max":[1,1,0])") + "," +
          accessor(1, 0, 5126, 3, "VEC3",
                   R"("min":[2,2,2],"max":[4,4,4])") + "]",
      R"([{"primitives":[)" +
          primitive_from(R"("POSITION":0)") + "," +
          primitive_from(R"("POSITION":1)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  const auto& bounds = result.value().bounds;
  EXPECT_TRUE(bounds.valid);
  EXPECT_FLOAT_EQ(bounds.min[0], 0.0f);
  EXPECT_FLOAT_EQ(bounds.max[0], 4.0f);
  EXPECT_FLOAT_EQ(bounds.max[2], 4.0f);
}

//! When any primitive lacks POSITION min/max the mesh box is invalid so the
//! culling layer will not drop the object.
TEST(GltfImporter, BoundsInvalidWhenAnyPrimitiveLacksMinMax) {
  BinBuilder bin;
  const std::size_t f_pos = bin.push_floats(
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f});
  const std::size_t s_pos = bin.push_floats(
      {2.0f, 2.0f, 2.0f, 4.0f, 4.0f, 4.0f, 3.0f, 2.0f, 2.0f});
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(f_pos, 36U) + "," + buffer_view(s_pos, 36U) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3",
                     R"("min":[0,0,0],"max":[1,1,0])") + "," +
          accessor(1, 0, 5126, 3, "VEC3") + "]",
      R"([{"primitives":[)" +
          primitive_from(R"("POSITION":0)") + "," +
          primitive_from(R"("POSITION":1)") + "]}]");

  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size());
  ASSERT_TRUE(result.is_ok());
  EXPECT_FALSE(result.value().bounds.valid);
}

//! A min without a matching max (or the wrong length) is malformed, not
//! silently treated as unbounded.
TEST(GltfImporter, RejectsOneSidedOrWrongLengthBounds) {
  BinBuilder bin;
  bin.push_floats({0.0f, 0.0f, 0.0f});
  const std::string min_only = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(0, 12U) + "]",
      "[" + accessor(0, 0, 5126, 1, "VEC3", R"("min":[0,0,0])") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(min_only, bin.data.data(), bin.data.size(),
                   {"must declare both min and max"});

  const std::string short_max = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(0, 12U) + "]",
      "[" + accessor(0, 0, 5126, 1, "VEC3",
                     R"("min":[0,0,0],"max":[1,1])") + "]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]");
  expect_malformed(short_max, bin.data.data(), bin.data.size(),
                   {"must be an array of 3 numbers"});
}

// ============================================================================
// BaseColorTexture wiring
// ============================================================================

namespace {

//! CRC-32 (PNG chunk layer).
std::uint32_t tex_crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
    }
  }
  return crc ^ 0xffffffffU;
}

void tex_chunk(std::vector<std::uint8_t>& out, const char* type,
               const std::vector<std::uint8_t>& data) {
  const std::uint32_t length = static_cast<std::uint32_t>(data.size());
  for (unsigned shift = 24U;; shift -= 8U) {
    out.push_back(static_cast<std::uint8_t>((length >> shift) & 0xFFU));
    if (shift == 0U) break;
  }
  const std::size_t type_offset = out.size();
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(type[i]));
  out.insert(out.end(), data.begin(), data.end());
  const std::uint32_t crc =
      tex_crc32(out.data() + type_offset, out.size() - type_offset);
  for (unsigned shift = 24U;; shift -= 8U) {
    out.push_back(static_cast<std::uint8_t>((crc >> shift) & 0xFFU));
    if (shift == 0U) break;
  }
}

//! 1x1 truecolour+alpha PNG (stored DEFLATE, filter 0).
std::vector<std::uint8_t> png_1x1_rgba(std::uint8_t r, std::uint8_t g,
                                       std::uint8_t b, std::uint8_t a) {
  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  const std::vector<std::uint8_t> ihdr = {
      0, 0, 0, 1,  // width 1
      0, 0, 0, 1,  // height 1
      8,           // bit depth
      6,           // colour type RGBA
      0, 0, 0};    // compression, filter, interlace
  tex_chunk(png, "IHDR", ihdr);
  const std::vector<std::uint8_t> raw = {0, r, g, b, a};  // filter 0 + texel
  std::vector<std::uint8_t> idat = {0x78, 0x01, 0x01, 0x05, 0x00, 0xfa, 0xff};
  idat.insert(idat.end(), raw.begin(), raw.end());
  tex_chunk(png, "IDAT", idat);
  tex_chunk(png, "IEND", {});
  return png;
}

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
  constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t i = 0; i < bytes.size(); i += 3U) {
    const std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16U |
                                (i + 1U < bytes.size()
                                     ? static_cast<std::uint32_t>(bytes[i + 1U]) << 8U
                                     : 0U) |
                                (i + 2U < bytes.size()
                                     ? static_cast<std::uint32_t>(bytes[i + 2U])
                                     : 0U);
    out.push_back(kAlphabet[(chunk >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(chunk >> 12U) & 0x3FU]);
    out.push_back(i + 1U < bytes.size() ? kAlphabet[(chunk >> 6U) & 0x3FU] : '=');
    out.push_back(i + 2U < bytes.size() ? kAlphabet[chunk & 0x3FU] : '=');
  }
  return out;
}

std::string png_data_uri(const std::vector<std::uint8_t>& png) {
  return "data:image/png;base64," + base64_encode(png);
}

struct TriangleFixture {
  std::string json;
  std::vector<std::uint8_t> bin;
};

//! Single-triangle glTF whose first primitive's material pbr object carries
//! `material_pbr_extra`; `extra_arrays` is appended after the materials array
//! (samplers/textures/images JSON). Geometry lives in the external .bin.
TriangleFixture textured_triangle_doc(std::string material_pbr_extra,
                                      std::string extra_arrays) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f};
  const std::vector<std::uint16_t> indices = {0, 1, 2};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::size_t idx_offset = bin.push_u16(indices);
  const std::string views = "[" + buffer_view(pos_offset, 3U * 3U * 4U) + "," +
                            buffer_view(idx_offset, 3U * 2U) + "]";
  const std::string accessors = "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
                                accessor(1, 0, 5123, 3, "SCALAR") + "]";
  const std::string material = "[{\"pbrMetallicRoughness\":{\"baseColorFactor\""
                               ":[1,1,1,1]" +
                               material_pbr_extra + "}}]";
  const std::string json = make_document(
      external_buffer(bin.data.size()), views, accessors,
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)",
                                             R"("indices":1,"material":0)") +
          "]}]",
      material + extra_arrays);
  return {json, std::move(bin.data)};
}

}  // namespace

//! Material baseColorTexture -> data-URI PNG: decoded to a 1x1 RGBA image
//! (bytes stay in the payload's encoded form), the binding declares its sRGB
//! colour role, sampler fields round-trip, base color untouched.
TEST(GltfImporter, BaseColorTextureDataUriDecodesToImage) {
  const std::vector<std::uint8_t> png = png_1x1_rgba(200, 30, 30, 255);
  const std::string extra =
      R"(,"samplers":[{"magFilter":9729,"minFilter":9987,"wrapS":33071}],"textures":[{"source":0,"sampler":0}],"images":[{"uri":")" +
      png_data_uri(png) + R"("}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", extra);

  std::string detail;
  auto result = import_gltf_mesh(fixture.json.data(), fixture.json.size(),
                                 fixture.bin.data(), fixture.bin.size(),
                                 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();
  ASSERT_TRUE(mesh.albedo.present);
  // baseColorTexture is an sRGB colour binding by glTF definition: bytes stay
  // encoded and the binding tells uploaders to sample through an sRGB format.
  EXPECT_TRUE(mesh.albedo.encoded_srgb);
  ASSERT_EQ(mesh.images.size(), 1U);
  EXPECT_EQ(mesh.albedo.image_index, 0U);
  EXPECT_EQ(mesh.images[0].width, 1U);
  EXPECT_EQ(mesh.images[0].height, 1U);
  ASSERT_EQ(mesh.images[0].rgba.size(), 4U);
  EXPECT_EQ(mesh.images[0].rgba[0], 200U);
  EXPECT_EQ(mesh.images[0].rgba[1], 30U);
  EXPECT_EQ(mesh.images[0].rgba[2], 30U);
  EXPECT_EQ(mesh.images[0].rgba[3], 255U);
  EXPECT_EQ(mesh.albedo.mag_filter, 9729U);
  EXPECT_EQ(mesh.albedo.min_filter, 9987U);
  EXPECT_EQ(mesh.albedo.wrap_s, 33071U);
  EXPECT_EQ(mesh.albedo.wrap_t, 10497U);  // glTF REPEAT default
  EXPECT_FLOAT_EQ(mesh.base_color[0], 1.0f);  // factor still applies
  EXPECT_EQ(mesh.vertex_count(), 3U);
}

//! baseColorTexture payload stored in a tightly packed bufferView of the
//! external .bin (PNG appended after the mesh data).
TEST(GltfImporter, BaseColorTextureFromBufferViewDecodes) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f};
  const std::vector<std::uint16_t> indices = {0, 1, 2};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::size_t idx_offset = bin.push_u16(indices);
  const std::vector<std::uint8_t> png = png_1x1_rgba(0, 0, 250, 255);
  const std::size_t png_offset = bin.data.size();
  bin.data.insert(bin.data.end(), png.begin(), png.end());

  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(pos_offset, 3U * 3U * 4U) + "," +
          buffer_view(idx_offset, 3U * 2U) + "," +
          buffer_view(png_offset, png.size()) + "]",
      "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
          accessor(1, 0, 5123, 3, "SCALAR") + "]",
      R"([{"primitives":[)" +
          primitive_from(R"("POSITION":0)",
                         R"("indices":1,"material":0)") +
          "]}]",
      std::string(R"([{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"baseColorTexture":{"index":0}}}],)") +
          R"("images":[{"bufferView":2}],"textures":[{"source":0}])");

  std::string detail;
  auto result = import_gltf_mesh(json.data(), json.size(), bin.data.data(),
                                 bin.data.size(), 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();
  ASSERT_TRUE(mesh.albedo.present);
  ASSERT_EQ(mesh.images.size(), 1U);
  EXPECT_EQ(mesh.images[0].width, 1U);
  EXPECT_EQ(mesh.images[0].rgba[2], 250U);
  // No sampler declared: filters unspecified (0), wraps default to REPEAT.
  EXPECT_EQ(mesh.albedo.mag_filter, 0U);
  EXPECT_EQ(mesh.albedo.min_filter, 0U);
  EXPECT_EQ(mesh.albedo.wrap_s, 10497U);
}

//! A material without baseColorTexture leaves albedo absent and images empty.
TEST(GltfImporter, MaterialWithoutTextureHasNoAlbedo) {
  const std::string extra =
      R"(,"images":[{"uri":")" + png_data_uri(png_1x1_rgba(1, 2, 3, 255)) +
      R"("}],"textures":[{"source":0}])";
  const TriangleFixture fixture = textured_triangle_doc("", extra);
  std::string detail;
  auto result = import_gltf_mesh(fixture.json.data(), fixture.json.size(),
                                 fixture.bin.data(), fixture.bin.size(),
                                 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  EXPECT_FALSE(result.value().albedo.present);
  EXPECT_TRUE(result.value().images.empty());
}

// ============================================================================
// Texture rejections
// ============================================================================

TEST(GltfImporter, RejectsBaseColorTextureOutOfRange) {
  const std::string extra =
      R"(,"textures":[{"source":0}],"images":[{"uri":")" +
      png_data_uri(png_1x1_rgba(1, 2, 3, 255)) + R"("}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":3})", extra);
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"baseColorTexture.index"});
}

TEST(GltfImporter, RejectsUnsupportedTexCoordSet) {
  const std::string extra =
      R"(,"textures":[{"source":0}],"images":[{"uri":")" +
      png_data_uri(png_1x1_rgba(1, 2, 3, 255)) + R"("}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0,"texCoord":1})", extra);
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"TEXCOORD_0"});
}

TEST(GltfImporter, RejectsInvalidSamplerEnums) {
  const std::string bad_filter =
      R"(,"samplers":[{"magFilter":1}],"textures":[{"source":0,"sampler":0}],"images":[{"uri":")" +
      png_data_uri(png_1x1_rgba(1, 2, 3, 255)) + R"("}])";
  const TriangleFixture bad_filter_fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", bad_filter);
  expect_malformed(bad_filter_fixture.json, bad_filter_fixture.bin.data(),
                   bad_filter_fixture.bin.size(), {"magFilter"});

  const std::string bad_wrap =
      R"(,"samplers":[{"wrapS":7}],"textures":[{"source":0,"sampler":0}],"images":[{"uri":")" +
      png_data_uri(png_1x1_rgba(1, 2, 3, 255)) + R"("}])";
  const TriangleFixture bad_wrap_fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", bad_wrap);
  expect_malformed(bad_wrap_fixture.json, bad_wrap_fixture.bin.data(),
                   bad_wrap_fixture.bin.size(), {"wrapS"});
}

TEST(GltfImporter, RejectsTextureMissingSource) {
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", R"(,"textures":[{}])");
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"missing source"});
}

TEST(GltfImporter, RejectsTextureSourceOutOfRange) {
  const TriangleFixture fixture = textured_triangle_doc(
      R"(,"baseColorTexture":{"index":0})", R"(,"textures":[{"source":5}])");
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"source 5"});
}

TEST(GltfImporter, RejectsSamplerOutOfRange) {
  const std::string extra =
      R"(,"textures":[{"source":0,"sampler":2}],"images":[{"uri":")" +
      png_data_uri(png_1x1_rgba(1, 2, 3, 255)) + R"("}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", extra);
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"sampler 2"});
}

TEST(GltfImporter, RejectsImageWithBothUriAndBufferView) {
  const std::vector<std::uint8_t> png = png_1x1_rgba(1, 2, 3, 255);
  BinBuilder bin;
  bin.data = png;
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(0, png.size()) + "]",
      "[]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]",
      std::string(R"([{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)") +
          R"("images":[{"uri":")" + png_data_uri(png) +
          R"(","bufferView":0}],"textures":[{"source":0}])");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"must not define both uri and bufferView"});
}

TEST(GltfImporter, RejectsExternalImageUriWhenReferenced) {
  const std::string extra =
      R"(,"images":[{"uri":"albedo.png"}],"textures":[{"source":0}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", extra);
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"external file"});
}

TEST(GltfImporter, RejectsTruncatedJpegImagePayload) {
  // A real JPEG SOI + APP0 header but no scan: the JPEG decoder must reject
  // it with its own diagnostic, not claim PNG decode failure.
  const std::string jpeg = base64_encode(
      {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00,
       0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00});
  const std::string extra =
      R"(,"images":[{"uri":"data:image/jpeg;base64,)" + jpeg + R"("}],"textures":[{"source":0}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", extra);
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"not a decodable PNG or JPEG", "truncated"});
}

TEST(GltfImporter, RejectsUnknownImagePayload) {
  // GIF magic bytes match no supported codec: rejected by the dispatcher.
  const std::string gif = base64_encode({'G', 'I', 'F', '8', '9', 'a'});
  const std::string extra =
      R"(,"images":[{"uri":"data:image/gif;base64,)" + gif + R"("}],"textures":[{"source":0}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", extra);
  expect_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                   {"not a decodable PNG or JPEG"});
}

//! A real (cjpeg-encoded, optimised Huffman) 1x1 solid-colour JPEG behind
//! baseColorTexture decodes through the dispatcher: the importer no longer
//! rejects JPEG payloads. The constant colour decodes exactly.
TEST(GltfImporter, JpegBaseColorTextureDataUriDecodesToImage) {
  const std::string jpeg =
      R"(/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAABAAEDAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDl6/ow/nc//9k=)";
  const std::string extra =
      R"(,"images":[{"uri":"data:image/jpeg;base64,)" + jpeg + R"("}],"textures":[{"source":0}])";
  const TriangleFixture fixture =
      textured_triangle_doc(R"(,"baseColorTexture":{"index":0})", extra);

  std::string detail;
  auto result = import_gltf_mesh(fixture.json.data(), fixture.json.size(),
                                 fixture.bin.data(), fixture.bin.size(),
                                 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();
  ASSERT_TRUE(mesh.albedo.present);
  ASSERT_EQ(mesh.images.size(), 1U);
  EXPECT_EQ(mesh.albedo.image_index, 0U);
  EXPECT_EQ(mesh.images[0].width, 1U);
  EXPECT_EQ(mesh.images[0].height, 1U);
  ASSERT_EQ(mesh.images[0].rgba.size(), 4U);
  EXPECT_EQ(mesh.images[0].rgba[0], 70U);
  EXPECT_EQ(mesh.images[0].rgba[1], 130U);
  EXPECT_EQ(mesh.images[0].rgba[2], 180U);
  EXPECT_EQ(mesh.images[0].rgba[3], 255U);
  EXPECT_EQ(mesh.vertex_count(), 3U);
}

TEST(GltfImporter, RejectsStridedImageBufferView) {
  const std::vector<std::uint8_t> png = png_1x1_rgba(1, 2, 3, 255);
  BinBuilder bin;
  bin.data = png;
  const std::string json = make_document(
      external_buffer(bin.data.size()),
      "[" + buffer_view(0, png.size(), R"("byteStride":4)") + "]",
      "[]",
      R"([{"primitives":[)" + primitive_from(R"("POSITION":0)") + "]}]",
      std::string(R"([{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)") +
          R"("images":[{"bufferView":0}],"textures":[{"source":0}])");
  expect_malformed(json, bin.data.data(), bin.data.size(),
                   {"image bufferViews", "byteStride"});
}

// ============================================================================
// Scene graph import (nodes/scenes -> flattened world instances)
// ============================================================================

namespace {

using omnicpp::asset::GltfSceneImport;
using omnicpp::asset::import_gltf_scene;

struct SceneFixture {
  std::string json;
  std::vector<std::uint8_t> bin;
};

//! Full document with the given views/accessors/meshes/nodes/scenes JSON.
std::string make_scene_document(std::size_t bin_size,
                                const std::string& views,
                                const std::string& accessors,
                                const std::string& meshes,
                                const std::string& nodes,
                                const std::string& scenes) {
  std::string doc = R"({"asset":{"version":"2.0"},"buffers":)";
  doc += external_buffer(bin_size);
  doc += R"(,"bufferViews":)";
  doc += views;
  doc += R"(,"accessors":)";
  doc += accessors;
  doc += R"(,"materials":[],"meshes":)";
  doc += meshes;
  doc += R"(,"nodes":)";
  doc += nodes;
  doc += R"(,"scenes":)";
  doc += scenes;
  doc += "}";
  return doc;
}

//! Single-triangle mesh JSON (POSITION + indices) with the shared builders.
std::string triangle_mesh() {
  return R"([{"primitives":[)" +
         primitive_from(R"("POSITION":0)", R"("indices":1)") + "]}]";
}

std::string triangle_views(std::size_t pos_offset, std::size_t idx_offset) {
  return "[" + buffer_view(pos_offset, 3U * 3U * 4U) + "," +
         buffer_view(idx_offset, 3U * 2U) + "]";
}

std::string triangle_accessors() {
  return "[" + accessor(0, 0, 5126, 3, "VEC3") + "," +
         accessor(1, 0, 5123, 3, "SCALAR") + "]";
}

//! Fixture with one triangle mesh whose nodes/scenes come from JSON strings.
SceneFixture scene_triangle(std::string nodes, std::string scenes) {
  BinBuilder bin;
  const std::vector<float> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f};
  const std::vector<std::uint16_t> indices = {0, 1, 2};
  const std::size_t pos_offset = bin.push_floats(positions);
  const std::size_t idx_offset = bin.push_u16(indices);
  SceneFixture fixture;
  fixture.bin = std::move(bin.data);
  fixture.json = make_scene_document(
      fixture.bin.size(), triangle_views(pos_offset, idx_offset),
      triangle_accessors(), triangle_mesh(), nodes, scenes);
  return fixture;
}

//! Column-major 4x4 applied to a point (homogeneous w = 1).
std::array<float, 3> apply_matrix(const std::array<float, 16>& m,
                                  const std::array<float, 3>& p) {
  std::array<float, 3> out{};
  for (int r = 0; r < 3; ++r) {
    out[static_cast<std::size_t>(r)] =
        m[static_cast<std::size_t>(r)] * p[0] +
        m[static_cast<std::size_t>(r) + 4U] * p[1] +
        m[static_cast<std::size_t>(r) + 8U] * p[2] +
        m[static_cast<std::size_t>(r) + 12U];
  }
  return out;
}

//! Scene-importer variant of expect_malformed.
void expect_scene_malformed(const std::string& json, const void* bin,
                            std::size_t bin_len,
                            const std::vector<std::string>& fragments,
                            std::size_t scene_index = 0) {
  std::string detail;
  auto result = import_gltf_scene(json.data(), json.size(),
                                  static_cast<const std::uint8_t*>(bin), bin_len,
                                  scene_index, &detail);
  ASSERT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
  ASSERT_FALSE(detail.empty()) << "expected a diagnostic message";
  bool matched = false;
  for (const std::string& fragment : fragments) {
    if (detail.find(fragment) != std::string::npos) matched = true;
  }
  EXPECT_TRUE(matched) << "detail '" << detail << "' matched none of:";
  for (const std::string& fragment : fragments) {
    std::cerr << "  - " << fragment << "\n";
  }
}

}  // namespace

//! Parent translation composes into the child's absolute model; the pure
//! group node itself is not emitted.
TEST(GltfSceneImport, ParentTranslationComposesIntoChild) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"root","translation":[3,0,0],"children":[1]},
          {"name":"child","mesh":0,"translation":[0,2,0]}])",
      R"([{"nodes":[0]}])");
  std::string detail;
  auto result = import_gltf_scene(fixture.json.data(), fixture.json.size(),
                                  fixture.bin.data(), fixture.bin.size(),
                                  0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfSceneImport& scene = result.value();
  ASSERT_EQ(scene.meshes.size(), 1U);
  ASSERT_EQ(scene.nodes.size(), 1U);
  EXPECT_EQ(scene.nodes[0].name, "child");
  EXPECT_TRUE(scene.nodes[0].has_mesh);
  EXPECT_EQ(scene.nodes[0].mesh_index, 0U);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[12], 3.0f);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[13], 2.0f);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[14], 0.0f);
}

//! A node carrying an explicit column-major matrix keeps it as its model.
TEST(GltfSceneImport, NodeMatrixUsedDirectly) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"m","mesh":0,
           "matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 2,1,0,1]}])",
      R"([{"nodes":[0]}])");
  std::string detail;
  auto result = import_gltf_scene(fixture.json.data(), fixture.json.size(),
                                  fixture.bin.data(), fixture.bin.size(),
                                  0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfSceneImport& scene = result.value();
  ASSERT_EQ(scene.nodes.size(), 1U);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[12], 2.0f);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[13], 1.0f);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[0], 1.0f);
}

//! TRS = translation * rotation * scale; a child at +X rotated +90 deg about
//! Y with 2x scale lands at world (1,0,-2) relative to its parent origin.
TEST(GltfSceneImport, TrsComposesWithRotationAndScale) {
  const float q = 0.70710678f;  // sin/cos of 45 deg
  const std::string rotation = std::to_string(0.0f) + "," +
                               std::to_string(q) + "," +
                               std::to_string(0.0f) + "," + std::to_string(q);
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"parent","translation":[0,0,-4],"children":[1]},
          {"name":"child","mesh":0,"translation":[1,0,0],
           "rotation":[)" + rotation +
          R"(],"scale":[2,2,2]}])",
      R"([{"nodes":[0]}])");
  std::string detail;
  auto result = import_gltf_scene(fixture.json.data(), fixture.json.size(),
                                  fixture.bin.data(), fixture.bin.size(),
                                  0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfSceneImport& scene = result.value();
  ASSERT_EQ(scene.nodes.size(), 1U);
  // Child origin maps to parent translation + local translation (1,0,0).
  EXPECT_FLOAT_EQ(scene.nodes[0].model[12], 1.0f);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[13], 0.0f);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[14], -4.0f);
  // Local point (1,0,0) scaled by 2, rotated +90 deg about Y -> (0,0,-2),
  // translated by local (1,0,0) -> world (1,0,-6).
  const auto world_point =
      apply_matrix(scene.nodes[0].model, {1.0f, 0.0f, 0.0f});
  EXPECT_NEAR(world_point[0], 1.0f, 1e-5f);
  EXPECT_NEAR(world_point[1], 0.0f, 1e-5f);
  EXPECT_NEAR(world_point[2], -6.0f, 1e-5f);
}

//! A mesh referenced by two nodes is imported once but instantiated twice.
TEST(GltfSceneImport, SharedMeshImportedOnce) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"a","mesh":0,"translation":[1,0,0]},
          {"name":"b","mesh":0,"translation":[2,0,0]}])",
      R"([{"nodes":[0,1]}])");
  std::string detail;
  auto result = import_gltf_scene(fixture.json.data(), fixture.json.size(),
                                  fixture.bin.data(), fixture.bin.size(),
                                  0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfSceneImport& scene = result.value();
  ASSERT_EQ(scene.meshes.size(), 1U);
  ASSERT_EQ(scene.nodes.size(), 2U);
  EXPECT_EQ(scene.nodes[0].mesh_index, 0U);
  EXPECT_EQ(scene.nodes[1].mesh_index, 0U);
  EXPECT_FLOAT_EQ(scene.nodes[1].model[12], 2.0f);
}

//! A subtree shared by two parents instantiates once per path.
TEST(GltfSceneImport, DiamondSharedSubtreeInstantiatesPerPath) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"root","children":[1,2]},
          {"name":"left","translation":[3,0,0],"children":[3]},
          {"name":"right","translation":[9,0,0],"children":[3]},
          {"name":"leaf","mesh":0,"translation":[0,1,0]}])",
      R"([{"nodes":[0]}])");
  std::string detail;
  auto result = import_gltf_scene(fixture.json.data(), fixture.json.size(),
                                  fixture.bin.data(), fixture.bin.size(),
                                  0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfSceneImport& scene = result.value();
  ASSERT_EQ(scene.meshes.size(), 1U);
  ASSERT_EQ(scene.nodes.size(), 2U);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[12], 3.0f);
  EXPECT_FLOAT_EQ(scene.nodes[0].model[13], 1.0f);
  EXPECT_FLOAT_EQ(scene.nodes[1].model[12], 9.0f);
  EXPECT_FLOAT_EQ(scene.nodes[1].model[13], 1.0f);
}

//! Scene index selects among the document's scenes.
TEST(GltfSceneImport, SelectsRequestedScene) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"zero","mesh":0,"translation":[1,0,0]},
          {"name":"one","mesh":0,"translation":[7,0,0]}])",
      R"([{"nodes":[0]},{"nodes":[1]}])");
  std::string detail;
  auto result = import_gltf_scene(fixture.json.data(), fixture.json.size(),
                                  fixture.bin.data(), fixture.bin.size(),
                                  1U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfSceneImport& scene = result.value();
  ASSERT_EQ(scene.nodes.size(), 1U);
  EXPECT_EQ(scene.nodes[0].name, "one");
  EXPECT_FLOAT_EQ(scene.nodes[0].model[12], 7.0f);
}

// ============================================================================
// Scene graph rejections
// ============================================================================

TEST(GltfSceneImport, RejectsNodeCycle) {
  const SceneFixture fixture = scene_triangle(
      R"([{"children":[1]},{"children":[0]}])",
      R"([{"nodes":[0]}])");
  expect_scene_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                         {"cycle"});
}

TEST(GltfSceneImport, RejectsMeshOutOfRange) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"m","mesh":5}])", R"([{"nodes":[0]}])");
  expect_scene_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                         {"out of range"});
}

TEST(GltfSceneImport, RejectsMatrixAndTrsTogether) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"m","mesh":0,"translation":[1,0,0],
           "matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]}])",
      R"([{"nodes":[0]}])");
  expect_scene_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                         {"must not define both"});
}

TEST(GltfSceneImport, RejectsSkin) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"m","mesh":0,"skin":0}])", R"([{"nodes":[0]}])");
  expect_scene_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                         {"skin"});
}

TEST(GltfSceneImport, RejectsBadRotationArity) {
  const SceneFixture fixture = scene_triangle(
      R"([{"name":"m","mesh":0,"rotation":[0,1,0]}])",
      R"([{"nodes":[0]}])");
  expect_scene_malformed(
      fixture.json, fixture.bin.data(), fixture.bin.size(),
      {"rotation must be an array of 4 numbers"});
}

TEST(GltfSceneImport, RejectsChildOutOfRange) {
  const SceneFixture fixture = scene_triangle(
      R"([{"children":[7]}])", R"([{"nodes":[0]}])");
  expect_scene_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                         {"children[0]"});
}

TEST(GltfSceneImport, RejectsSceneIndexOutOfRange) {
  const SceneFixture fixture = scene_triangle(R"([])", R"([{"nodes":[]}])");
  expect_scene_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                         {"scene index"}, 3U);
}

TEST(GltfSceneImport, RejectsSceneNodeOutOfRange) {
  const SceneFixture fixture = scene_triangle(R"([])", R"([{"nodes":[4]}])");
  expect_scene_malformed(fixture.json, fixture.bin.data(), fixture.bin.size(),
                         {"out of range"});
}

// =============================================================================
// Multi-material meshes (per-primitive material assignment)
// =============================================================================

namespace {

struct MultiTriDoc {
  std::string json;
  std::vector<std::uint8_t> bin;
};

//! Build a glTF mesh with one indexed triangle per entry in `position_sets`.
//! Every triangle gets its own POSITION accessor (in order) and they share one
//! uint16 indices accessor holding 0,1,2 (valid for every triangle because the
//! importer offsets raw indices by each primitive's base vertex). Each entry of
//! `primitive_extras` is spliced into the matching primitive object (e.g.
//! "\"indices\":2,\"material\":0" — the indices accessor index equals
//! position_sets.size()).
MultiTriDoc multi_triangles_doc(
    const std::vector<std::vector<float>>& position_sets,
    const std::vector<std::string>& primitive_extras,
    std::string materials_json, std::string extra_arrays,
    const std::vector<std::size_t>& position_accessors = {}) {
  BinBuilder bin;
  std::vector<std::size_t> pos_views;
  for (const auto& positions : position_sets) {
    pos_views.push_back(bin.push_floats(positions));
  }
  const std::size_t idx_offset = bin.push_u16({0, 1, 2});

  std::string views = "[";
  for (std::size_t i = 0; i < pos_views.size(); ++i) {
    if (i != 0) views += ",";
    views += buffer_view(pos_views[i],
                         position_sets[i].size() * sizeof(float));
  }
  views += "," + buffer_view(idx_offset, 3U * 2U) + "]";

  std::string accessors = "[";
  for (std::size_t i = 0; i < pos_views.size(); ++i) {
    if (i != 0) accessors += ",";
    accessors += accessor(i, 0, 5126, 3, "VEC3");
  }
  accessors += "," + accessor(pos_views.size(), 0, 5123, 3, "SCALAR") + "]";

  std::string prims = "[";
  for (std::size_t i = 0; i < primitive_extras.size(); ++i) {
    if (i != 0) prims += ",";
    // Which POSITION accessor this primitive references: the shared default
    // is its own triangle's accessor; a caller may reuse one accessor across
    // primitives by passing matching entries.
    const std::size_t pos_accessor =
        i < position_accessors.size() ? position_accessors[i] : i;
    prims += primitive_from(R"("POSITION":)" + std::to_string(pos_accessor),
                            primitive_extras[i].c_str());
  }
  prims += "]";
  const std::string json = make_document(
      external_buffer(bin.data.size()), views, accessors,
      R"([{"primitives":)" + prims + "}]",
      materials_json + extra_arrays);
  return {json, std::move(bin.data)};
}

}  // namespace

//! A mesh whose two primitives bind different materials (one textured, one
//! not) must capture every binding: contiguous per-primitive index/vertex
//! ranges, per-primitive base colors and albedo references. The mesh-level
//! convenience fields alias the FIRST primitive that binds a material.
TEST(GltfImporter, MultiMaterialMeshCapturesEveryPrimitive) {
  MultiTriDoc doc = multi_triangles_doc(
      {{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
       {5.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f, 5.0f, 1.0f, 0.0f}},
      {R"("indices":2,"material":0)", R"("indices":2,"material":1)"},
      std::string(R"([{"name":"Red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}},{"name":"Green","pbrMetallicRoughness":{"baseColorFactor":[0,1,0,1],"baseColorTexture":{"index":0}}}])"),
      R"(,"samplers":[{"wrapS":33071}],"textures":[{"source":0,"sampler":0}],"images":[{"uri":")" +
          png_data_uri(png_1x1_rgba(0, 255, 0, 255)) + R"("}])");

  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();

  ASSERT_EQ(mesh.primitives.size(), 2U);

  const auto& red = mesh.primitives[0];
  EXPECT_EQ(red.material_name, "Red");
  EXPECT_EQ(red.vertex_offset, 0U);
  EXPECT_EQ(red.vertex_count, 3U);
  EXPECT_EQ(red.index_offset, 0U);
  EXPECT_EQ(red.index_count, 3U);
  EXPECT_FLOAT_EQ(red.base_color[0], 1.0f);
  EXPECT_FLOAT_EQ(red.base_color[1], 0.0f);
  EXPECT_FALSE(red.albedo.present);

  const auto& green = mesh.primitives[1];
  EXPECT_EQ(green.material_name, "Green");
  EXPECT_EQ(green.vertex_offset, 3U);
  EXPECT_EQ(green.vertex_count, 3U);
  EXPECT_EQ(green.index_offset, 3U);
  EXPECT_EQ(green.index_count, 3U);
  EXPECT_FLOAT_EQ(green.base_color[0], 0.0f);
  EXPECT_FLOAT_EQ(green.base_color[1], 1.0f);
  ASSERT_TRUE(green.albedo.present);
  EXPECT_EQ(green.albedo.image_index, 0U);

  // The green primitive's PNG was decoded; the red one adds no image.
  ASSERT_EQ(mesh.images.size(), 1U);
  ASSERT_EQ(mesh.images[0].rgba.size(), 4U);
  EXPECT_EQ(mesh.images[0].rgba[1], 255U);

  // Geometry slices: second primitive's vertices start at float 3*11 and hold
  // its own positions; its indices were offset by base vertex 3.
  EXPECT_FLOAT_EQ(mesh.vertices[green.vertex_offset * kSceneVertexFloats + 0U],
                  5.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[red.vertex_offset * kSceneVertexFloats + 0U],
                  0.0f);
  ASSERT_EQ(mesh.indices.size(), 6U);
  EXPECT_EQ(mesh.indices[3], 3U);
  EXPECT_EQ(mesh.indices[4], 4U);
  EXPECT_EQ(mesh.indices[5], 5U);

  // Top-level convenience fields alias the first binder (the red primitive).
  EXPECT_FLOAT_EQ(mesh.base_color[0], 1.0f);
  EXPECT_FALSE(mesh.albedo.present);
}

//! Several primitives sharing one texture must decode it exactly once while
//! primitives keep per-binding image references and sampler state.
TEST(GltfImporter, SharedTextureAcrossPrimitivesDecodesOnce) {
  MultiTriDoc doc = multi_triangles_doc(
      {{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
       {5.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f, 5.0f, 1.0f, 0.0f},
       {10.0f, 0.0f, 0.0f, 11.0f, 0.0f, 0.0f, 10.0f, 1.0f, 0.0f}},
      {R"("indices":3,"material":0)", R"("indices":3,"material":1)",
       R"("indices":3,"material":2)"},
      std::string(R"([{"name":"A","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},{"name":"B","pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},{"name":"C","pbrMetallicRoughness":{"baseColorTexture":{"index":1}}}])"),
      R"(,"samplers":[{"wrapS":10497},{"wrapS":33071}],"textures":[{"source":0,"sampler":0},{"source":1,"sampler":1}],"images":[{"uri":")" +
          png_data_uri(png_1x1_rgba(200, 0, 0, 255)) + R"("},{"uri":")" +
          png_data_uri(png_1x1_rgba(0, 200, 0, 255)) + R"("}])");

  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();

  ASSERT_EQ(mesh.primitives.size(), 3U);
  ASSERT_EQ(mesh.images.size(), 2U);  // decoded once per unique texture
  EXPECT_EQ(mesh.images[0].rgba[0], 200U);
  EXPECT_EQ(mesh.images[1].rgba[1], 200U);

  EXPECT_TRUE(mesh.primitives[0].albedo.present);
  EXPECT_TRUE(mesh.primitives[1].albedo.present);
  EXPECT_TRUE(mesh.primitives[2].albedo.present);
  EXPECT_EQ(mesh.primitives[0].albedo.image_index, 0U);
  EXPECT_EQ(mesh.primitives[1].albedo.image_index, 0U);
  EXPECT_EQ(mesh.primitives[2].albedo.image_index, 1U);
  // Shared texture 0 reports the same sampler state on both bindings.
  EXPECT_EQ(mesh.primitives[0].albedo.wrap_s, 10497U);
  EXPECT_EQ(mesh.primitives[1].albedo.wrap_s, 10497U);
  EXPECT_EQ(mesh.primitives[2].albedo.wrap_s, 33071U);
}

//! A primitive without a material member binds glTF's default material
//! (opaque white, no albedo); the mesh-level alias tracks the first primitive
//! that DOES bind a material.
TEST(GltfImporter, PrimitiveWithoutMaterialGetsDefaultWhite) {
  MultiTriDoc doc = multi_triangles_doc(
      {{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
       {5.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f, 5.0f, 1.0f, 0.0f}},
      {"", R"("indices":2,"material":0)"},
      std::string(R"([{"pbrMetallicRoughness":{"baseColorFactor":[0,0,1,1]}}])"),
      "");

  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();
  ASSERT_EQ(mesh.primitives.size(), 2U);

  const auto& plain = mesh.primitives[0];
  EXPECT_TRUE(plain.material_name.empty());
  EXPECT_FLOAT_EQ(plain.base_color[0], 1.0f);
  EXPECT_FLOAT_EQ(plain.base_color[3], 1.0f);
  EXPECT_FALSE(plain.albedo.present);

  const auto& blue = mesh.primitives[1];
  EXPECT_FLOAT_EQ(blue.base_color[2], 1.0f);

  // The alias is the first BINDING primitive (the second one here).
  EXPECT_FLOAT_EQ(mesh.base_color[2], 1.0f);
  EXPECT_EQ(mesh.base_color[0], 0.0f);
}

//! Two primitives sharing one POSITION accessor each get a self-contained
//! contiguous vertex slice (the importer copies per primitive), so render
//! layers can split without aliasing surprises.
TEST(GltfImporter, SharedAccessorSlicesAreSelfContained) {
  MultiTriDoc doc = multi_triangles_doc(
      {{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}},
      {R"("indices":1,"material":0)", R"("indices":1,"material":1)"},
      std::string(R"([{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}},{"pbrMetallicRoughness":{"baseColorFactor":[0,0,1,1]}}])"),
      "", {0U, 0U});

  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();
  ASSERT_EQ(mesh.primitives.size(), 2U);

  EXPECT_EQ(mesh.primitives[0].vertex_offset, 0U);
  EXPECT_EQ(mesh.primitives[0].vertex_count, 3U);
  EXPECT_EQ(mesh.primitives[1].vertex_offset, 3U);
  EXPECT_EQ(mesh.primitives[1].vertex_count, 3U);
  EXPECT_EQ(mesh.vertex_count(), 6U);
  // The duplicated slice decodes to the same positions.
  for (std::size_t c = 0; c < 3U; ++c) {
    EXPECT_FLOAT_EQ(mesh.vertices[c * kSceneVertexFloats],
                    mesh.vertices[(3U + c) * kSceneVertexFloats]);
  }
  EXPECT_EQ(mesh.indices[3], 3U);
  EXPECT_EQ(mesh.indices[4], 4U);
}

//! An out-of-range material reference on ANY primitive is rejected (not just
//! the first binding one, which the historical single-material path checked).
TEST(GltfImporter, RejectsOutOfRangeMaterialOnLaterPrimitive) {
  MultiTriDoc doc = multi_triangles_doc(
      {{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f},
       {5.0f, 0.0f, 0.0f, 6.0f, 0.0f, 0.0f, 5.0f, 1.0f, 0.0f}},
      {R"("indices":2,"material":0)", R"("indices":2,"material":9)"},
      std::string(R"([{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]}}])"),
      "");
  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail);
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
  EXPECT_NE(detail.find("material 9 is out of range"), std::string::npos);
}

// ============================================================================
// External image files (optional ExternalFileLoader)
// ============================================================================

//! A material baseColorTexture whose image is an external file path is
//! resolved lazily through the supplied loader; the fetched bytes go through
//! the same magic-byte decode dispatch as embedded payloads.
TEST(GltfImporter, ExternalImageResolvedThroughLoader) {
  const std::vector<std::uint8_t> png = png_1x1_rgba(11, 22, 33, 255);
  const TriangleFixture doc = textured_triangle_doc(
      R"(,"baseColorTexture":{"index":0})",
      R"(,"images":[{"uri":"textures/albedo.png"}],"textures":[{"source":0}])");

  std::size_t calls = 0;
  omnicpp::asset::ExternalFileLoader loader =
      [&](const std::string& uri, std::string& error,
          std::vector<std::uint8_t>& bytes) -> bool {
        ++calls;
        if (uri != "textures/albedo.png") {
          error = "unexpected uri '" + uri + "'";
          return false;
        }
        bytes = png;
        return true;
      };

  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail,
                                 &loader);
  ASSERT_TRUE(result.is_ok()) << detail;
  const GltfMeshImport& mesh = result.value();
  ASSERT_TRUE(mesh.albedo.present);
  ASSERT_EQ(mesh.images.size(), 1U);
  ASSERT_EQ(mesh.images[0].rgba.size(), 4U);
  EXPECT_EQ(mesh.images[0].rgba[0], 11U);
  EXPECT_EQ(mesh.images[0].rgba[1], 22U);
  EXPECT_EQ(mesh.images[0].rgba[2], 33U);
  EXPECT_EQ(mesh.images[0].rgba[3], 255U);
  EXPECT_EQ(calls, 1U) << "loader must be invoked exactly once";
}

//! Without a loader, referencing an external image file is a hard error whose
//! diagnostic names the missing loader and the unresolved file.
TEST(GltfImporter, ExternalImageWithoutLoaderRejected) {
  const TriangleFixture doc = textured_triangle_doc(
      R"(,"baseColorTexture":{"index":0})",
      R"(,"images":[{"uri":"textures/albedo.png"}],"textures":[{"source":0}])");
  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail);
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
  EXPECT_NE(detail.find("no ExternalFileLoader was provided"),
            std::string::npos)
      << detail;
  EXPECT_NE(detail.find("textures/albedo.png"), std::string::npos) << detail;
}

//! A loader failure surfaces as a malformed import whose diagnostic carries
//! the file URI and the loader's reason.
TEST(GltfImporter, ExternalImageLoaderFailureRejected) {
  const TriangleFixture doc = textured_triangle_doc(
      R"(,"baseColorTexture":{"index":0})",
      R"(,"images":[{"uri":"missing.png"}],"textures":[{"source":0}])");
  omnicpp::asset::ExternalFileLoader loader =
      [&](const std::string&, std::string& error,
          std::vector<std::uint8_t>&) -> bool {
        error = "no such file on the test filesystem";
        return false;
      };
  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail,
                                 &loader);
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
  EXPECT_NE(detail.find("could not load external file 'missing.png'"),
            std::string::npos)
      << detail;
  EXPECT_NE(detail.find("no such file on the test filesystem"),
            std::string::npos)
      << detail;
}

//! An external image referenced only by images/textures entries that no
//! imported material binds must never reach the loader (lazy resolution).
TEST(GltfImporter, UnreferencedExternalImageNeverCallsLoader) {
  // Mesh binds material 0, which has no baseColorTexture; images[0] is
  // external and never used by any imported material.
  const TriangleFixture doc = textured_triangle_doc(
      "", R"(,"images":[{"uri":"unused.png"}],"textures":[{"source":0}])");
  std::size_t calls = 0;
  omnicpp::asset::ExternalFileLoader loader =
      [&](const std::string&, std::string& error,
          std::vector<std::uint8_t>&) -> bool {
        ++calls;
        error = "must not be called";
        return false;
      };
  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail,
                                 &loader);
  ASSERT_TRUE(result.is_ok()) << detail;
  EXPECT_FALSE(result.value().albedo.present);
  EXPECT_TRUE(result.value().images.empty());
  EXPECT_EQ(calls, 0U) << "unreferenced external file must stay untouched";
}

//! A loader returning an empty file is rejected with a diagnostic naming the
//! file rather than mis-decoding zero bytes.
TEST(GltfImporter, ExternalImageLoaderEmptyFileRejected) {
  const TriangleFixture doc = textured_triangle_doc(
      R"(,"baseColorTexture":{"index":0})",
      R"(,"images":[{"uri":"empty.png"}],"textures":[{"source":0}])");
  omnicpp::asset::ExternalFileLoader loader =
      [&](const std::string&, std::string&, std::vector<std::uint8_t>& bytes)
          -> bool {
        bytes.clear();
        return true;
      };
  std::string detail;
  auto result = import_gltf_mesh(doc.json.data(), doc.json.size(),
                                 doc.bin.data(), doc.bin.size(), 0U, &detail,
                                 &loader);
  EXPECT_FALSE(result.is_ok());
  EXPECT_EQ(result.error(), RuntimeError::malformed_asset);
  EXPECT_NE(detail.find("external file 'empty.png' is empty"),
            std::string::npos)
      << detail;
}

//! Scene import threads the loader through to each imported mesh: a mesh in
//! the scene whose material binds an external texture decodes it.
TEST(GltfImporter, ExternalImageResolvedThroughSceneImport) {
  const std::vector<std::uint8_t> png = png_1x1_rgba(200, 30, 30, 255);
  // Mesh 0 textured via external file, plus a scene/node graph.
  const TriangleFixture doc = textured_triangle_doc(
      R"(,"baseColorTexture":{"index":0})",
      R"(,"images":[{"uri":"tex/scene.png"}],"textures":[{"source":0}])");
  // Re-wrap with scenes/nodes referencing meshes[0].
  const std::string json = R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)" +
                           doc.json.substr(doc.json.find(R"("buffers")"));
  std::size_t calls = 0;
  omnicpp::asset::ExternalFileLoader loader =
      [&](const std::string& uri, std::string& error,
          std::vector<std::uint8_t>& bytes) -> bool {
        ++calls;
        if (uri != "tex/scene.png") {
          error = "unexpected uri '" + uri + "'";
          return false;
        }
        bytes = png;
        return true;
      };
  std::string detail;
  auto result = omnicpp::asset::import_gltf_scene(
      json.data(), json.size(), doc.bin.data(), doc.bin.size(), 0U, &detail,
      &loader);
  ASSERT_TRUE(result.is_ok()) << detail;
  ASSERT_EQ(result.value().meshes.size(), 1U);
  const GltfMeshImport& mesh = result.value().meshes[0];
  ASSERT_TRUE(mesh.albedo.present);
  ASSERT_EQ(mesh.images.size(), 1U);
  ASSERT_EQ(mesh.images[0].rgba.size(), 4U);
  EXPECT_EQ(mesh.images[0].rgba[0], 200U);
  EXPECT_EQ(calls, 1U);
}
