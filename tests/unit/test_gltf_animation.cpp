//! @file test_gltf_animation.cpp
//! @brief Skeletal glTF ingestion proofs against the generated mannequin
//!        asset (scripts/generate_mannequin.py): skins/nodes/animations
//!        import, rest-pose skinning is exactly the bind pose, walk-cycle
//!        sampling matches its keyframes, byte-exact determinism, and
//!        rejection of malformed skeletal documents.

#include <gtest/gtest.h>

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "warploom/asset/gltf_animation.hpp"

#ifndef WARPLOOM_TEST_ASSET_DIR
#define WARPLOOM_TEST_ASSET_DIR "assets/models"
#endif
#include "warploom/asset/gltf_importer.hpp"

namespace {

//! Directory of the generated mannequin asset: provided at configure time
//! (like WARPLOOM_TEST_SHADER_DIR) with a repo-relative fallback.
std::string read_file(const std::string& path, std::vector<char>& bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return "cannot open " + path;
  bytes.assign(std::istreambuf_iterator<char>(file),
               std::istreambuf_iterator<char>());
  return {};
}

struct Mannequin {
  std::string json;
  std::vector<char> bin;
};

//! Fails the current test (non-fatally) and returns an empty Mannequin when
//! the asset cannot be read; callers early-out on json.empty().
Mannequin load_mannequin() {
  Mannequin mannequin;
  std::vector<char> json_bytes;
  const std::string asset_dir =
#ifdef WARPLOOM_TEST_ASSET_DIR
      WARPLOOM_TEST_ASSET_DIR
#else
      "assets/models"
#endif
      ;
  if (const std::string error =
          read_file(asset_dir + "/mannequin.gltf", json_bytes);
      !error.empty()) {
    ADD_FAILURE() << error;
    return mannequin;
  }
  mannequin.json.assign(json_bytes.begin(), json_bytes.end());

  std::vector<char> bin_bytes;
  if (const std::string error =
          read_file(asset_dir + "/mannequin.bin", bin_bytes);
      !error.empty()) {
    ADD_FAILURE() << error;
    return mannequin;
  }
  mannequin.bin = bin_bytes;
  return mannequin;
}

//! Compose local matrices down the mannequin hierarchy (mirror of the
//! engine helpers, kept independent so the test cross-checks the engine).
std::vector<omnicpp::asset::GltfTransform> test_locals(
    const omnicpp::asset::GltfAnimationDocument& doc) {
  std::vector<omnicpp::asset::GltfTransform> locals(doc.nodes.size());
  for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
    locals[i] = omnicpp::asset::gltf_local_matrix(doc.nodes[i]);
  }
  return locals;
}

//! Hash a whole imported document (vertices, indices, skins, animations) so
//! the GLB and glTF import paths can be compared byte-exactly.
std::string document_fingerprint(const omnicpp::asset::GltfAnimationDocument& doc) {
  std::size_t hash = 1469598103934665603ULL;  // FNV offset basis.
  auto mix = [&hash](const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ULL;  // FNV prime.
    }
  };
  for (const auto& mesh : doc.meshes) {
    mix(mesh.vertices.data(), mesh.vertices.size() * sizeof(float));
    mix(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
    mix(mesh.base_color.data(), sizeof(mesh.base_color));
    for (const auto& prim : mesh.primitives) {
      mix(&prim.index_count, sizeof(prim.index_count));
      mix(prim.base_color.data(), sizeof(prim.base_color));
    }
  }
  for (const auto& binding : doc.skin_bindings) {
    mix(binding.joints.data(), binding.joints.size() * sizeof(std::uint16_t));
    mix(binding.weights.data(), binding.weights.size() * sizeof(float));
  }
  for (const auto& node : doc.nodes) {
    mix(node.name.data(), node.name.size());
    mix(node.translation, sizeof(node.translation));
    mix(node.rotation, sizeof(node.rotation));
    mix(node.scale, sizeof(node.scale));
    mix(&node.mesh_index, sizeof(node.mesh_index));
    for (const std::size_t child : node.children) {
      mix(&child, sizeof(child));
    }
  }
  for (const auto& skin : doc.skins) {
    mix(skin.name.data(), skin.name.size());
    for (const auto& m : skin.inverse_bind_matrices) {
      mix(m.data(), m.size() * sizeof(float));
    }
  }
  for (const auto& anim : doc.animations) {
    mix(anim.name.data(), anim.name.size());
    mix(&anim.duration, sizeof(anim.duration));
    for (const auto& sampler : anim.samplers) {
      mix(sampler.times.data(), sampler.times.size() * sizeof(float));
      mix(sampler.values.data(), sampler.values.size() * sizeof(float));
    }
    for (const auto& channel : anim.channels) {
      mix(&channel.sampler, sizeof(channel.sampler));
      mix(&channel.target_node, sizeof(channel.target_node));
    }
  }
  return std::to_string(hash);
}

//! Pack a glTF document into a GLB 2.0 container: 12-byte header, JSON chunk
//! (space-padded to 4 bytes), BIN chunk (zero-padded to 4 bytes).
std::vector<char> pack_glb(std::string_view json, const std::vector<char>& bin) {
  const auto align4 = [](std::size_t n) { return (n + 3U) & ~std::size_t{3U}; };
  const std::size_t json_len = align4(json.size());
  const std::size_t bin_len = align4(bin.size());
  const std::size_t total = 12U + 8U + json_len +
                            (bin_len != 0U ? 8U + bin_len : 0U);
  std::vector<char> glb(total);
  auto put32 = [&glb](std::size_t offset, std::uint32_t value) {
    std::memcpy(glb.data() + offset, &value, 4);
  };
  put32(0, 0x46546C67U);          // magic "glTF"
  put32(4, 2U);                   // version
  put32(8, static_cast<std::uint32_t>(total));
  put32(12, static_cast<std::uint32_t>(json_len));
  put32(16, 0x4E4F534AU);         // "JSON"
  std::memcpy(glb.data() + 20, json.data(), json.size());
  for (std::size_t i = json.size(); i < json_len; ++i) {
    glb[20 + i] = ' ';
  }
  std::size_t bin_offset = 20 + json_len;
  if (bin_len != 0U) {
    put32(bin_offset, static_cast<std::uint32_t>(bin_len));
    put32(bin_offset + 4, 0x004E4942U);  // "BIN\0"
    std::memcpy(glb.data() + bin_offset + 8, bin.data(), bin.size());
  }
  return glb;
}

void apply_pose(omnicpp::asset::GltfAnimationDocument& doc,
                const omnicpp::asset::GltfAnimationImport& anim,
                float time) {
  for (const auto& channel : anim.channels) {
    float out[4];
    omnicpp::asset::sample_gltf_channel(
        anim.samplers[channel.sampler], time, out);
    auto& node = doc.nodes[channel.target_node];
    switch (channel.path) {
      case omnicpp::asset::GltfChannel::Path::Translation:
        node.translation[0] = out[0];
        node.translation[1] = out[1];
        node.translation[2] = out[2];
        break;
      case omnicpp::asset::GltfChannel::Path::Rotation:
        node.rotation[0] = out[0];
        node.rotation[1] = out[1];
        node.rotation[2] = out[2];
        node.rotation[3] = out[3];
        break;
      case omnicpp::asset::GltfChannel::Path::Scale:
        node.scale[0] = out[0];
        node.scale[1] = out[1];
        node.scale[2] = out[2];
        break;
    }
  }
}

}  // namespace

TEST(GltfAnimation, MannequinImportsWithSkinAndAnimation) {
  const Mannequin m = load_mannequin();
  ASSERT_FALSE(m.json.empty());
  std::string error;
  auto result = omnicpp::asset::import_gltf_animation_document(
      m.json.data(), m.json.size(),
      reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
      &error);
  ASSERT_TRUE(result.is_ok()) << error;
  const auto& doc = result.value();

  ASSERT_EQ(doc.skins.size(), 1U);
  EXPECT_EQ(doc.skins[0].joints.size(), 15U);
  EXPECT_EQ(doc.skins[0].inverse_bind_matrices.size(), 15U);
  // Two clips: walk (7 channels) + idle breathing (3 channels).
  ASSERT_EQ(doc.animations.size(), 2U);
  EXPECT_EQ(doc.animations[0].channels.size(), 7U);
  EXPECT_NEAR(doc.animations[0].duration, 1.0f, 1e-6f);
  EXPECT_EQ(doc.animations[1].channels.size(), 3U);
  EXPECT_NEAR(doc.animations[1].duration, 1.0f, 1e-6f);
  // 9 mesh nodes: 15 joint nodes + 9 mesh nodes.
  ASSERT_EQ(doc.nodes.size(), 24U);
  std::size_t mesh_nodes = 0;
  for (const auto& node : doc.nodes) {
    if (node.has_mesh) ++mesh_nodes;
  }
  EXPECT_EQ(mesh_nodes, 9U);
  // Geometry came through the canonical importer: 24 verts / 36 indices
  // per box mesh, 11 floats per vertex.
  for (const auto& mesh : doc.meshes) {
    EXPECT_EQ(mesh.vertex_count(), 24U);
    EXPECT_EQ(mesh.indices.size(), 36U);
    EXPECT_EQ(mesh.vertices.size(), 24U * 11U);
  }
  // Every skinned mesh has a binding aligned with its vertex count.
  for (std::size_t i = 0; i < doc.meshes.size(); ++i) {
    ASSERT_EQ(doc.skin_bindings[i].joints.size(),
              static_cast<std::size_t>(doc.meshes[i].vertex_count()) * 4U);
    ASSERT_EQ(doc.skin_bindings[i].weights.size(),
              static_cast<std::size_t>(doc.meshes[i].vertex_count()) * 4U);
  }
}

TEST(GltfAnimation, RestPoseSkinningReproducesBindPose) {
  const Mannequin m = load_mannequin();
  ASSERT_FALSE(m.json.empty());
  std::string error;
  auto result = omnicpp::asset::import_gltf_animation_document(
      m.json.data(), m.json.size(),
      reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
      &error);
  ASSERT_TRUE(result.is_ok()) << error;
  auto doc = result.value();

  // Rest pose: global(j) * IBM(j) must be identity for every joint.
  std::vector<omnicpp::asset::GltfTransform> globals;
  omnicpp::asset::gltf_global_matrices(doc, test_locals(doc), globals);
  const auto& skin = doc.skins[0];
  for (std::size_t j = 0; j < skin.joints.size(); ++j) {
    const auto& g = globals[skin.joints[j]];
    const auto& ibm = skin.inverse_bind_matrices[j];
    float product[16]{};
    for (std::size_t c = 0; c < static_cast<std::size_t>(4); ++c) {
      for (std::size_t r = 0; r < static_cast<std::size_t>(4); ++r) {
        float sum = 0.0f;
        for (std::size_t k = 0; k < static_cast<std::size_t>(4); ++k) {
          sum += g[static_cast<std::size_t>(r + 4 * k)] *
                 ibm[static_cast<std::size_t>(k + 4 * c)];
        }
        product[r + 4 * c] = sum;
      }
    }
    for (std::size_t c = 0; c < static_cast<std::size_t>(16); ++c) {
      EXPECT_NEAR(product[c], (c % 5 == 0) ? 1.0f : 0.0f, 1e-4f)
          << "joint " << j << " element " << c;
    }
  }
  // Skinned vertex at rest == bind vertex (single-joint bindings).
  std::vector<omnicpp::asset::GltfTransform> joints;
  omnicpp::asset::gltf_skin_matrices(doc, 0, globals, joints);
  for (std::size_t mesh_i = 0; mesh_i < doc.meshes.size(); ++mesh_i) {
    const auto& mesh = doc.meshes[mesh_i];
    const auto& binding = doc.skin_bindings[mesh_i];
    for (std::uint32_t v = 0; v < mesh.vertex_count(); ++v) {
      const std::size_t base = static_cast<std::size_t>(v) * 11U;
      float skinned[3] = {0.0f, 0.0f, 0.0f};
      float weight_sum = 0.0f;
      for (std::size_t c = 0; c < 4; ++c) {
        const float w = binding.weights[v * 4U + c];
        weight_sum += w;
        const auto& jm = joints[binding.joints[v * 4U + c]];
        for (std::size_t r = 0; r < static_cast<std::size_t>(3); ++r) {
          skinned[r] += w * (jm[r + 0] * mesh.vertices[base + 0] +
                             jm[r + 4] * mesh.vertices[base + 1] +
                             jm[r + 8] * mesh.vertices[base + 2] +
                             jm[r + 12]);
        }
      }
      EXPECT_NEAR(weight_sum, 1.0f, 1e-5f);
      for (std::size_t r = 0; r < static_cast<std::size_t>(3); ++r) {
        EXPECT_NEAR(skinned[r], mesh.vertices[base + r], 1e-3f)
            << "mesh " << mesh_i << " vertex " << v << " axis " << r;
      }
    }
  }
}

TEST(GltfAnimation, WalkCycleSamplingMatchesKeyframes) {
  const Mannequin m = load_mannequin();
  ASSERT_FALSE(m.json.empty());
  std::string error;
  auto result = omnicpp::asset::import_gltf_animation_document(
      m.json.data(), m.json.size(),
      reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
      &error);
  ASSERT_TRUE(result.is_ok()) << error;
  auto doc = result.value();
  const auto& anim = doc.animations[0];

  // Every sampler is LINEAR; rotations/translations share the 5-key grid,
  // the hip-bob translation uses its own 3-key grid.
  for (const auto& sampler : anim.samplers) {
    ASSERT_EQ(sampler.interpolation,
              omnicpp::asset::GltfSamplerInterpolation::Linear);
    ASSERT_TRUE(sampler.times.size() == 5U || sampler.times.size() == 3U)
        << "unexpected keyframe count";
  }
  // l_hip (node 11) rotation keyframes: theta(t) = A*sin(2*pi*t) about X.
  const auto& hip = anim.channels[1];  // 0 = hip bob, 1 = l_hip swing
  ASSERT_EQ(hip.target_node, 11U);
  const auto& hip_sampler = anim.samplers[hip.sampler];
  const float amplitude = 0.6f;
  for (std::size_t k = 0; k < 5; ++k) {
    const float theta =
        amplitude * std::sin(2.0f * static_cast<float>(M_PI) * hip_sampler.times[k]);
    const float expected_x = std::sin(theta / 2.0f);
    const float expected_w = std::cos(theta / 2.0f);
    EXPECT_NEAR(hip_sampler.values[k * 4U + 0], expected_x, 1e-5f);
    EXPECT_NEAR(hip_sampler.values[k * 4U + 1], 0.0f, 1e-6f);
    EXPECT_NEAR(hip_sampler.values[k * 4U + 2], 0.0f, 1e-6f);
    EXPECT_NEAR(hip_sampler.values[k * 4U + 3], expected_w, 1e-5f);
  }
  // Mid-stride interpolation: t=0.125 -> half of the t=[0.0,0.25] swing.
  float out[4];
  omnicpp::asset::sample_gltf_channel(hip_sampler, 0.125f, out);
  const float theta_half =
      amplitude * std::sin(2.0f * static_cast<float>(M_PI) * 0.0f) / 2.0f +
      amplitude * std::sin(2.0f * static_cast<float>(M_PI) * 0.25f) / 2.0f;
  const float slerp_w = std::cos(theta_half / 2.0f);
  EXPECT_NEAR(out[3], slerp_w, 1e-4f);
  // Clamping: t=2.0 holds the last keyframe; t=-1 holds the first.
  omnicpp::asset::sample_gltf_channel(hip_sampler, 2.0f, out);
  EXPECT_NEAR(out[0], hip_sampler.values[4U * 4U + 0], 1e-6f);
  omnicpp::asset::sample_gltf_channel(hip_sampler, -1.0f, out);
  EXPECT_NEAR(out[0], hip_sampler.values[0], 1e-6f);
}

TEST(GltfAnimation, AnimationMovesTheMannequin) {
  const Mannequin m = load_mannequin();
  ASSERT_FALSE(m.json.empty());
  std::string error;
  auto result = omnicpp::asset::import_gltf_animation_document(
      m.json.data(), m.json.size(),
      reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
      &error);
  ASSERT_TRUE(result.is_ok()) << error;
  auto doc = result.value();
  const auto anim = doc.animations[0];

  // Skinned foot position at rest vs mid-stride: the left foot must swing
  // forward (z) when the right swings back — visible skeletal motion.
  auto foot_z = [&](float time) {
    apply_pose(doc, anim, time);
    std::vector<omnicpp::asset::GltfTransform> locals = test_locals(doc);
    std::vector<omnicpp::asset::GltfTransform> globals;
    omnicpp::asset::gltf_global_matrices(doc, locals, globals);
    // l_knee is joint node 12: world z of the knee joint.
    return globals[12][14];
  };
  const float rest_z = foot_z(0.0f);
  const float quarter_z = foot_z(0.25f);
  const float three_quarter_z = foot_z(0.75f);
  EXPECT_NEAR(rest_z, 0.0f, 1e-4f);
  EXPECT_LT(quarter_z, -0.05f);                      // left leg back
  EXPECT_GT(three_quarter_z, 0.05f);                 // left leg forward
  EXPECT_GT(std::abs(quarter_z), std::abs(rest_z));
}

TEST(GltfAnimation, ImportIsByteDeterministic) {
  const Mannequin m = load_mannequin();
  ASSERT_FALSE(m.json.empty());
  std::string error;
  auto first = omnicpp::asset::import_gltf_animation_document(
      m.json.data(), m.json.size(),
      reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
      &error);
  ASSERT_TRUE(first.is_ok()) << error;
  auto second = omnicpp::asset::import_gltf_animation_document(
      m.json.data(), m.json.size(),
      reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
      &error);
  ASSERT_TRUE(second.is_ok()) << error;

  const auto& a = first.value();
  const auto& b = second.value();
  ASSERT_EQ(a.nodes.size(), b.nodes.size());
  ASSERT_EQ(a.meshes.size(), b.meshes.size());
  ASSERT_EQ(a.skins.size(), b.skins.size());
  for (std::size_t i = 0; i < a.meshes.size(); ++i) {
    ASSERT_EQ(a.meshes[i].vertices.size(), b.meshes[i].vertices.size());
    EXPECT_EQ(std::memcmp(a.meshes[i].vertices.data(),
                          b.meshes[i].vertices.data(),
                          a.meshes[i].vertices.size() * sizeof(float)),
              0);
    EXPECT_EQ(std::memcmp(a.skin_bindings[i].joints.data(),
                          b.skin_bindings[i].joints.data(),
                          a.skin_bindings[i].joints.size() *
                              sizeof(std::uint16_t)),
              0);
  }
  EXPECT_EQ(std::memcmp(a.skins[0].inverse_bind_matrices.data(),
                        b.skins[0].inverse_bind_matrices.data(),
                        15U * sizeof(omnicpp::asset::GltfTransform)),
            0);
}

TEST(GltfAnimation, RejectsMalformedSkeletalDocuments) {
  const Mannequin m = load_mannequin();
  ASSERT_FALSE(m.json.empty());
  std::string error;
  auto import = [&](const std::string& json) {
    return omnicpp::asset::import_gltf_animation_document(
        json.data(), json.size(),
        reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
        &error);
  };

  // Singular node matrix (degenerate zero column cannot be decomposed to
  // TRS; well-formed matrices are legal since the exact-decomposition
  // support). The identity matrix would import fine — this one must not.
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"n","matrix":[0,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}],
      "meshes":[],"skins":[],
      "buffers":[],"bufferViews":[],"accessors":[]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("singular"), std::string::npos)
        << "error was: " << error;
  }
  // Skin without inverseBindMatrices.
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"root","children":[1]},{"name":"j"}],
      "meshes":[],
      "skins":[{"joints":[0,1]}],
      "buffers":[],"bufferViews":[],"accessors":[]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("inverseBindMatrices"), std::string::npos)
        << "error was: " << error;
  }
  // Duplicate joints in one skin.
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"a"},{"name":"b"}],
      "meshes":[],
      "skins":[{"joints":[0,0]}],
      "buffers":[],"bufferViews":[],"accessors":[]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("duplicate joints"), std::string::npos)
        << "error was: " << error;
  }
  // One animation, two channels writing the same node component.
  // Blob: times [0,1]; outputs (0,0,0) and (1,1,1) -> 32 bytes.
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"a"}],
      "meshes":[],
      "animations":[{"samplers":[
        {"input":0,"output":1,"interpolation":"LINEAR"},
        {"input":0,"output":1,"interpolation":"LINEAR"}],
       "channels":[
        {"sampler":0,"target":{"node":0,"path":"translation"}},
        {"sampler":1,"target":{"node":0,"path":"translation"}}]}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":8},
        {"buffer":0,"byteOffset":8,"byteLength":24}],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
        {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],
      "buffers":[{"byteLength":32,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAgD8="}]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("same node component"), std::string::npos)
        << "error was: " << error;
  }
  // CUBICSPLINE with a keyframe-count mismatch (output must be 3x input).
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"a"}],
      "meshes":[],
      "animations":[{"samplers":[
        {"input":0,"output":1,"interpolation":"CUBICSPLINE"}],
       "channels":[
        {"sampler":0,"target":{"node":0,"path":"translation"}}]}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":8},
        {"buffer":0,"byteOffset":8,"byteLength":24}],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
        {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],
      "buffers":[{"byteLength":32,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAgD8="}]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("3x .input count"), std::string::npos)
        << "error was: " << error;
  }
  // Non-monotonic sampler input times: times [1,0], outputs zeroed.
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"a"}],
      "meshes":[],
      "animations":[{"samplers":[
        {"input":0,"output":1,"interpolation":"LINEAR"}],
       "channels":[
        {"sampler":0,"target":{"node":0,"path":"translation"}}]}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":8},
        {"buffer":0,"byteOffset":8,"byteLength":24}],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
        {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],
      "buffers":[{"byteLength":32,"uri":"data:application/octet-stream;base64,AACAPwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("strictly increasing"), std::string::npos)
        << "error was: " << error;
  }
  // Rotation channel bound to a VEC3 sampler.
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"a"}],
      "meshes":[],
      "animations":[{"samplers":[
        {"input":0,"output":1,"interpolation":"LINEAR"}],
       "channels":[
        {"sampler":0,"target":{"node":0,"path":"rotation"}}]}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":8},
        {"buffer":0,"byteOffset":8,"byteLength":24}],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
        {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],
      "buffers":[{"byteLength":32,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AACAPwAAgD8="}]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("output width"), std::string::npos)
        << "error was: " << error;
  }
}

TEST(GltfSamplerMath, StepAndExactKeyframeSemantics) {
  omnicpp::asset::GltfSampler step;
  step.interpolation = omnicpp::asset::GltfSamplerInterpolation::Step;
  step.stride = 3;
  step.times = {0.0f, 1.0f, 2.0f};
  step.values = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f};

  float out[4];
  // STEP holds the previous keyframe inside a segment.
  omnicpp::asset::sample_gltf_channel(step, 0.5f, out);
  EXPECT_FLOAT_EQ(out[0], 0.0f);
  omnicpp::asset::sample_gltf_channel(step, 1.5f, out);
  EXPECT_FLOAT_EQ(out[0], 1.0f);
  // Exact keyframe hits return the exact keyframe.
  omnicpp::asset::sample_gltf_channel(step, 1.0f, out);
  EXPECT_FLOAT_EQ(out[1], 1.0f);

  // LINEAR lerp on a translation channel.
  omnicpp::asset::GltfSampler linear;
  linear.interpolation = omnicpp::asset::GltfSamplerInterpolation::Linear;
  linear.stride = 3;
  linear.times = {0.0f, 1.0f};
  linear.values = {0.0f, 0.0f, 0.0f, 2.0f, 4.0f, -6.0f};
  omnicpp::asset::sample_gltf_channel(linear, 0.25f, out);
  EXPECT_FLOAT_EQ(out[0], 0.5f);
  EXPECT_FLOAT_EQ(out[1], 1.0f);
  EXPECT_FLOAT_EQ(out[2], -1.5f);

  // 90-degree X rotation slerped half-way is 45 degrees.
  omnicpp::asset::GltfSampler rot;
  rot.interpolation = omnicpp::asset::GltfSamplerInterpolation::Linear;
  rot.stride = 4;
  rot.times = {0.0f, 1.0f};
  rot.values = {0.0f, 0.0f, 0.0f, 1.0f,
                std::sin(1.5707963f / 2.0f), 0.0f, 0.0f,
                std::cos(1.5707963f / 2.0f)};
  omnicpp::asset::sample_gltf_channel(rot, 0.5f, out);
  EXPECT_NEAR(out[0], std::sin(0.78539816f / 2.0f), 1e-6f);
  EXPECT_NEAR(out[3], std::cos(0.78539816f / 2.0f), 1e-6f);
}


TEST(GltfAnimation, CesiumManRealAssetImports) {
  // Khronos sample asset: matrix root nodes (Z_UP fixup), 22-node forest,
  // 19-joint skin, 2 s 57-channel walk cycle, external JPEG texture
  // (repacked as PNG for the engine's baseline-only decoder).
  std::vector<char> json_bytes;
  ASSERT_TRUE(read_file(std::string(WARPLOOM_TEST_ASSET_DIR) +
                            "/cesiumman/CesiumMan.gltf",
                        json_bytes).empty());
  std::vector<char> bin_bytes;
  ASSERT_TRUE(read_file(std::string(WARPLOOM_TEST_ASSET_DIR) +
                            "/cesiumman/CesiumMan_data.bin",
                        bin_bytes).empty());
  const std::string asset_dir =
      std::string(WARPLOOM_TEST_ASSET_DIR) + "/cesiumman/";
  const omnicpp::asset::ExternalFileLoader loader =
      [&asset_dir](const std::string& uri, std::string& load_error,
                   std::vector<std::uint8_t>& out_bytes) {
        std::vector<char> bytes;
        if (!read_file(asset_dir + uri, bytes).empty()) {
          load_error = "cannot open " + uri;
          return false;
        }
        out_bytes.assign(bytes.begin(), bytes.end());
        return true;
      };

  std::string error;
  auto imported = omnicpp::asset::import_gltf_animation_document(
      json_bytes.data(), json_bytes.size(),
      reinterpret_cast<const std::uint8_t*>(bin_bytes.data()),
      bin_bytes.size(), &error, &loader);
  ASSERT_TRUE(imported.is_ok()) << error;
  const auto& doc = imported.value();

  EXPECT_EQ(doc.nodes.size(), 22U);
  EXPECT_EQ(doc.meshes.size(), 1U);
  ASSERT_EQ(doc.skins.size(), 1U);
  EXPECT_EQ(doc.skins[0].joints.size(), 19U);
  ASSERT_EQ(doc.animations.size(), 1U);
  EXPECT_NEAR(doc.animations[0].duration, 2.0F, 1e-4F);
  EXPECT_EQ(doc.animations[0].samplers.size(), 57U);
  EXPECT_EQ(doc.animations[0].channels.size(), 57U);
  EXPECT_EQ(doc.meshes[0].vertex_count(), 3273U);
  EXPECT_EQ(doc.meshes[0].indices.size(), 14016U);
  // The walk-cycle texture was repacked to PNG and is decoded.
  ASSERT_EQ(doc.meshes[0].images.size(), 1U);
  EXPECT_FALSE(doc.meshes[0].images[0].rgba.empty());

  // Sampling the 2 s cycle at its 1 s midpoint must move joints: global
  // joint transforms differ from rest pose for >= 3 of the 19 joints.
  const auto rest = test_locals(doc);
  std::vector<omnicpp::asset::GltfTransform> rest_globals;
  omnicpp::asset::gltf_global_matrices(doc, rest, rest_globals);
  omnicpp::asset::GltfAnimationDocument posed = doc;
  apply_pose(posed, posed.animations[0], 1.0F);
  const auto mid = test_locals(posed);
  std::vector<omnicpp::asset::GltfTransform> mid_globals;
  omnicpp::asset::gltf_global_matrices(posed, mid, mid_globals);
  std::size_t moved = 0;
  for (std::size_t j = 0; j < 19U; ++j) {
    const auto& a = rest_globals[doc.skins[0].joints[j]];
    const auto& b = mid_globals[posed.skins[0].joints[j]];
    if (std::abs(a[12] - b[12]) > 1e-4F ||
        std::abs(a[13] - b[13]) > 1e-4F ||
        std::abs(a[14] - b[14]) > 1e-4F) {
      ++moved;
    }
  }
  EXPECT_GE(moved, 3U);
}

TEST(GltfAnimation, MatrixNodeDecomposesToTrs) {
  // A document whose root node carries a 90-degree X rotation matrix must
  // decompose to the equivalent TRS: sampling and skinning then behave as
  // if the node had been authored in TRS form.
  const std::string json = R"glTF({"asset":{"version":"2.0"},
   "scenes":[{"nodes":[0]}],
   "nodes":[
     {"name":"root","matrix":[1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,5,0,1]},
     {"name":"child","translation":[1,2,3]}],
   "meshes":[],
   "animations":[{"samplers":[
     {"input":0,"output":1}],
    "channels":[
     {"sampler":0,"target":{"node":1,"path":"translation"}}]}],
   "bufferViews":[
     {"buffer":0,"byteOffset":0,"byteLength":8},
     {"buffer":0,"byteOffset":8,"byteLength":12}],
   "accessors":[
     {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
     {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],
   "buffers":[{"byteLength":32,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AAIA/AAAAQAAAQEAAAIBAAACgQAAAwEA="}]})glTF";
  omnicpp::asset::GltfAnimationDocument doc;
  std::string error;
  const auto imported = omnicpp::asset::import_gltf_animation_document(
      json.data(), json.size(), nullptr, 0U, &error);
  ASSERT_TRUE(imported.is_ok()) << error;
  doc = std::move(imported).value();

  const auto& root = doc.nodes[0];
  EXPECT_NEAR(root.translation[1], 5.0F, 1e-5F);
  EXPECT_NEAR(root.scale[0], 1.0F, 1e-5F);
  EXPECT_NEAR(root.scale[1], 1.0F, 1e-5F);
  EXPECT_NEAR(root.scale[2], 1.0F, 1e-5F);
  // 90-degree X rotation: (x,y,z,w) = (sqrt(0.5), 0, 0, sqrt(0.5)).
  EXPECT_NEAR(std::abs(root.rotation[0]), 0.7071068F, 1e-5F);
  EXPECT_NEAR(std::abs(root.rotation[3]), 0.7071068F, 1e-5F);
  EXPECT_NEAR(root.rotation[1], 0.0F, 1e-6F);
  EXPECT_NEAR(root.rotation[2], 0.0F, 1e-6F);

  // Recomposing local TRS must reproduce the input matrix exactly.
  const float m[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 5, 0, 1};
  const auto local = omnicpp::asset::gltf_local_matrix(root);
  for (std::size_t i = 0; i < 16U; ++i) {
    EXPECT_NEAR(local[i], m[i], 1e-5F) << "index " << i;
  }
}

TEST(GltfAnimation, MatrixNodeWithShearIsRejected) {
  // A sheared matrix (non-orthogonal columns) cannot be animated via TRS
  // components and must be rejected with a diagnostic.
  const std::string json = R"glTF({"asset":{"version":"2.0"},
   "scenes":[{"nodes":[0]}],
   "nodes":[
     {"name":"sheared",
      "matrix":[1,0,0,0, 0.6,1,0,0, 0,0,1,0, 0,0,0,1]},
     {"name":"child"}],
   "meshes":[],
   "animations":[],
   "bufferViews":[],
   "accessors":[],
   "buffers":[]})glTF";
  std::string error;
  const auto imported = omnicpp::asset::import_gltf_animation_document(
      json.data(), json.size(), nullptr, 0U, &error);
  EXPECT_FALSE(imported.is_ok());
  EXPECT_NE(error.find("shear"), std::string::npos) << error;
}


TEST(GltfAnimation, BlendPoseEndpointsAndMidpoint) {
  // Endpoints are exact; the midpoint of a 90-degree rotation pair is the
  // 45-degree quaternion, and translation/scale blend linearly.
  omnicpp::asset::GltfSkinNode a{};
  a.translation[1] = 1.0F;
  a.scale[2] = 2.0F;
  // Identity rotation.
  const float qa[4] = {0.0F, 0.0F, 0.0F, 1.0F};
  std::copy(qa, qa + 4, a.rotation);

  omnicpp::asset::GltfSkinNode b{};
  b.translation[1] = 3.0F;
  b.scale[2] = 4.0F;
  // 90 degrees about X.
  const float qb[4] = {std::sqrt(0.5F), 0.0F, 0.0F, std::sqrt(0.5F)};
  std::copy(qb, qb + 4, b.rotation);

  omnicpp::asset::GltfSkinNode out{};
  omnicpp::asset::blend_pose(a, b, 0.0F, out);
  EXPECT_FLOAT_EQ(out.translation[1], 1.0F);
  EXPECT_FLOAT_EQ(out.scale[2], 2.0F);
  EXPECT_FLOAT_EQ(out.rotation[3], 1.0F);

  omnicpp::asset::blend_pose(a, b, 1.0F, out);
  EXPECT_FLOAT_EQ(out.translation[1], 3.0F);
  EXPECT_FLOAT_EQ(out.rotation[0], qb[0]);

  omnicpp::asset::blend_pose(a, b, 0.5F, out);
  EXPECT_FLOAT_EQ(out.translation[1], 2.0F);
  EXPECT_FLOAT_EQ(out.scale[2], 3.0F);
  // Midpoint rotation: 45 degrees about X, unit norm.
  const float norm = std::sqrt(out.rotation[0] * out.rotation[0] +
                               out.rotation[3] * out.rotation[3]);
  EXPECT_NEAR(norm, 1.0F, 1e-6F);
  EXPECT_NEAR(out.rotation[0], 0.3826834F, 1e-5F);
  EXPECT_NEAR(out.rotation[3], 0.9238795F, 1e-5F);

  // Alpha clamps outside [0,1].
  omnicpp::asset::blend_pose(a, b, -1.0F, out);
  EXPECT_FLOAT_EQ(out.translation[1], 1.0F);
  omnicpp::asset::blend_pose(a, b, 2.0F, out);
  EXPECT_FLOAT_EQ(out.translation[1], 3.0F);
}

TEST(GltfAnimation, CrossFadeWalkToIdleOnMannequin) {
  const Mannequin m = load_mannequin();
  ASSERT_FALSE(m.json.empty());
  std::string error;
  auto imported = omnicpp::asset::import_gltf_animation_document(
      m.json.data(), m.json.size(),
      reinterpret_cast<const std::uint8_t*>(m.bin.data()), m.bin.size(),
      &error);
  ASSERT_TRUE(imported.is_ok()) << error;
  auto doc = std::move(imported).value();
  ASSERT_EQ(doc.animations.size(), 2U);
  const auto& walk = doc.animations[0];
  const auto& idle = doc.animations[1];

  // alpha = 0 keeps the base (rest) pose; alpha = 1 is exactly the clip.
  // The chest is sampled at t = 0.5 (its scale keyframe trough; t = 0.25 is
  // the exact keyframe midpoint, where the breath is 1.0 by construction).
  std::vector<omnicpp::asset::GltfSkinNode> rest = doc.nodes;
  std::vector<omnicpp::asset::GltfSkinNode> idle_full = rest;
  omnicpp::asset::sample_clip_blended(doc, idle, 0.5F, 1.0F, idle_full);
  std::vector<omnicpp::asset::GltfSkinNode> half = rest;
  omnicpp::asset::sample_clip_blended(doc, idle, 0.5F, 0.5F, half);

  // Idle drives the chest scale (node 2): the half fade must land strictly
  // between rest (1.0) and the idle pose on that component.
  const std::size_t chest = 2U;
  const float rest_scale = 1.0F;
  const float idle_scale = idle_full[chest].scale[1];
  EXPECT_NE(idle_scale, rest_scale);
  const float half_scale = half[chest].scale[1];
  EXPECT_NEAR(half_scale, 0.5F * (rest_scale + idle_scale), 1e-6F);

  // Idle drives arm rotations (nodes 5 and 8): blending moves them too.
  // Arms peak at t = 0.25 (sin(pi/2) = 1), so sample those there.
  std::vector<omnicpp::asset::GltfSkinNode> arms_in = rest;
  omnicpp::asset::sample_clip_blended(doc, idle, 0.25F, 1.0F, arms_in);
  std::vector<omnicpp::asset::GltfSkinNode> arms_half = rest;
  omnicpp::asset::sample_clip_blended(doc, idle, 0.25F, 0.5F, arms_half);
  const float arm_rest = doc.nodes[5].rotation[0];
  const float arm_idle = arms_in[5].rotation[0];
  EXPECT_NE(arm_rest, arm_idle);
  EXPECT_NEAR(arms_half[5].rotation[0],
              0.5F * (arm_rest + arm_idle), 1e-6F);

  // Walk at a keyframe, blended fully in, must equal direct channel
  // sampling (the determinism contract carries through blending).
  std::vector<omnicpp::asset::GltfSkinNode> walk_full = rest;
  omnicpp::asset::sample_clip_blended(doc, walk, 0.25F, 1.0F, walk_full);
  omnicpp::asset::GltfAnimationDocument direct = doc;
  apply_pose(direct, walk, 0.25F);
  for (std::size_t i = 0; i < walk_full.size(); ++i) {
    EXPECT_NEAR(walk_full[i].rotation[0], direct.nodes[i].rotation[0], 1e-6F);
    EXPECT_NEAR(walk_full[i].rotation[3], direct.nodes[i].rotation[3], 1e-6F);
  }

  // The walk cycle must move at least the 4 leg joints.
  std::size_t moved = 0;
  for (std::size_t i = 0; i < walk_full.size(); ++i) {
    if (std::abs(walk_full[i].rotation[0] - doc.nodes[i].rotation[0]) >
        1e-5F) {
      ++moved;
    }
  }
  EXPECT_GE(moved, 4U);
}

TEST(GltfAnimation, GlbContainerImportsIdenticallyToJson) {
  const Mannequin json_asset = load_mannequin();
  ASSERT_FALSE(json_asset.json.empty());

  const std::vector<char> glb_bytes =
      pack_glb(json_asset.json, json_asset.bin);

  omnicpp::asset::GltfAnimationDocument glb_doc;
  std::string glb_error;
  const auto glb_import = omnicpp::asset::import_gltf_animation_document(
      glb_bytes.data(), glb_bytes.size(), nullptr, 0U, &glb_error);
  ASSERT_TRUE(glb_import.is_ok()) << glb_error;
  glb_doc = std::move(glb_import).value();

  omnicpp::asset::GltfAnimationDocument json_doc;
  std::string json_error;
  const auto json_import = omnicpp::asset::import_gltf_animation_document(
      json_asset.json.data(), json_asset.json.size(),
      reinterpret_cast<const std::uint8_t*>(json_asset.bin.data()),
      json_asset.bin.size(), &json_error);
  ASSERT_TRUE(json_import.is_ok()) << json_error;
  json_doc = std::move(json_import).value();

  // The two container formats must yield byte-identical import results.
  EXPECT_EQ(document_fingerprint(json_doc), document_fingerprint(glb_doc));

  // And the sampled walk cycle must match too: same pose at t = 0.5 s.
  apply_pose(json_doc, json_doc.animations[0], 0.5F);
  apply_pose(glb_doc, glb_doc.animations[0], 0.5F);
  for (std::size_t i = 0; i < json_doc.nodes.size(); ++i) {
    EXPECT_EQ(0, std::memcmp(json_doc.nodes[i].translation,
                             glb_doc.nodes[i].translation,
                             sizeof(json_doc.nodes[i].translation)));
    EXPECT_EQ(0, std::memcmp(json_doc.nodes[i].rotation,
                             glb_doc.nodes[i].rotation,
                             sizeof(json_doc.nodes[i].rotation)));
  }
}

TEST(GltfAnimation, GlbContainerRejectsMalformedContainers) {
  const Mannequin json_asset = load_mannequin();
  ASSERT_FALSE(json_asset.json.empty());

  std::vector<char> glb = pack_glb(json_asset.json, json_asset.bin);

  // Bad magic: not a GLB header and not a JSON document.
  {
    std::vector<char> broken = glb;
    ASSERT_FALSE(broken.empty()) << "pack_glb produced no bytes to corrupt";
    broken[0] = 'X';
    std::string error;
    const auto imported = omnicpp::asset::import_gltf_animation_document(
        broken.data(), broken.size(), nullptr, 0U, &error);
    EXPECT_FALSE(imported.is_ok());
    EXPECT_NE(error.find("neither a JSON glTF document"), std::string::npos)
        << error;
  }
  // Bad version.
  {
    std::vector<char> broken = glb;
    std::uint32_t version = 3;
    std::memcpy(broken.data() + 4, &version, 4);
    std::string error;
    const auto imported = omnicpp::asset::import_gltf_animation_document(
        broken.data(), broken.size(), nullptr, 0U, &error);
    EXPECT_FALSE(imported.is_ok());
    EXPECT_NE(error.find("version"), std::string::npos) << error;
  }
  // Header length exceeds the data.
  {
    std::vector<char> broken = glb;
    std::uint32_t length = static_cast<std::uint32_t>(broken.size()) + 100U;
    std::memcpy(broken.data() + 8, &length, 4);
    std::string error;
    const auto imported = omnicpp::asset::import_gltf_animation_document(
        broken.data(), broken.size(), nullptr, 0U, &error);
    EXPECT_FALSE(imported.is_ok());
    EXPECT_NE(error.find("length"), std::string::npos) << error;
  }
  // JSON chunk length runs past the container.
  {
    std::vector<char> broken = glb;
    std::uint32_t length = 1U << 20;
    std::memcpy(broken.data() + 12, &length, 4);
    std::string error;
    const auto imported = omnicpp::asset::import_gltf_animation_document(
        broken.data(), broken.size(), nullptr, 0U, &error);
    EXPECT_FALSE(imported.is_ok());
    EXPECT_NE(error.find("length"), std::string::npos) << error;
  }
  // BIN chunk size disagrees with buffers[0].byteLength: rewrite the
  // document's declared byteLength so only the BIN-chunk check can catch it.
  {
    std::string tweaked = json_asset.json;
    const std::string length_key =
        "\"byteLength\": " + std::to_string(json_asset.bin.size());
    const std::size_t length_pos = tweaked.find(length_key);
    ASSERT_NE(length_pos, std::string::npos) << length_key;
    tweaked.replace(length_pos, length_key.size(),
                    "\"byteLength\": " +
                        std::to_string(json_asset.bin.size() - 8U));
    const std::vector<char> broken = pack_glb(tweaked, json_asset.bin);
    std::string error;
    const auto imported = omnicpp::asset::import_gltf_animation_document(
        broken.data(), broken.size(), nullptr, 0U, &error);
    EXPECT_FALSE(imported.is_ok());
    // Either the GLB-specific check or the generic buffer-size check may
    // fire first; both must name the mismatch.
    EXPECT_TRUE(error.find("BIN chunk size") != std::string::npos ||
                error.find("does not match declared byteLength") !=
                    std::string::npos)
        << error;
  }
  // A second URI-less buffer: only buffer 0 may be uri-less in a GLB.
  {
    std::string tweaked = json_asset.json;
    const std::size_t buffers_pos = tweaked.find("\"buffers\": [");
    ASSERT_NE(buffers_pos, std::string::npos) << tweaked.substr(0, 300);
    const std::size_t insert_at = buffers_pos + std::string("\"buffers\": [").size();
    // A second uri-less buffer with a nonzero byteLength: valid JSON, but
    // only buffer 0 may omit its uri, so the import must reject it.
    tweaked.insert(insert_at, "{ \"byteLength\" : 16 },");
    const std::vector<char> broken = pack_glb(tweaked, json_asset.bin);
    std::string error;
    const auto imported = omnicpp::asset::import_gltf_animation_document(
        broken.data(), broken.size(), nullptr, 0U, &error);
    EXPECT_FALSE(imported.is_ok()) << error;
  }
}



TEST(GltfAnimation, CubicSplineTranslationHermite) {
  // Zero end tangents: pure Hermite basis on the keyframe values.
  const std::string json = R"glTF({"asset":{"version":"2.0"},
 "scenes":[{"nodes":[0]}],
 "nodes":[{"name":"a"}],
 "meshes":[],
 "animations":[{"samplers":[
   {"input":0,"output":1,"interpolation":"CUBICSPLINE"}],
  "channels":[
   {"sampler":0,"target":{"node":0,"path":"translation"}}]}],
 "bufferViews":[
   {"buffer":0,"byteOffset":0,"byteLength":8},
   {"buffer":0,"byteOffset":8,"byteLength":72}],
 "accessors":[
   {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
   {"bufferView":1,"componentType":5126,"count":6,"type":"VEC3"}],
 "buffers":[{"byteLength":80,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAACBBAACgQQAA8EEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACBCAABIQgAAcEIAAAAAAAAAAAAAAAA="}]})glTF";
  omnicpp::asset::GltfAnimationDocument doc;
  std::string error;
  const auto imported = omnicpp::asset::import_gltf_animation_document(
      json.data(), json.size(), nullptr, 0U, &error);
  ASSERT_TRUE(imported.is_ok()) << error;
  doc = std::move(imported).value();
  ASSERT_EQ(doc.animations.size(), 1U);
  const auto& sampler = doc.animations[0].samplers[0];
  EXPECT_EQ(sampler.interpolation,
            omnicpp::asset::GltfSamplerInterpolation::CubicSpline);
  ASSERT_EQ(sampler.values.size(), 2U * 3U);
  ASSERT_EQ(sampler.in_tangents.size(), 2U * 3U);

  float out[4];
  // Quarter point: h00 = 0.84375, h01 = 0.15625 (tangent terms vanish):
  // 0.84375 * 10 + 0.15625 * 40 = 14.6875.
  omnicpp::asset::sample_gltf_channel(sampler, 0.25F, out);
  EXPECT_NEAR(out[0], 14.6875F, 1e-4F);
  // Midpoint: every component is the average of the two keyframe values.
  omnicpp::asset::sample_gltf_channel(sampler, 0.5F, out);
  EXPECT_NEAR(out[0], 25.0F, 1e-4F);
  EXPECT_NEAR(out[1], 35.0F, 1e-4F);
  EXPECT_NEAR(out[2], 45.0F, 1e-4F);
  // Exact keyframe hits are exact (determinism contract).
  omnicpp::asset::sample_gltf_channel(sampler, 0.0F, out);
  EXPECT_FLOAT_EQ(out[0], 10.0F);
  omnicpp::asset::sample_gltf_channel(sampler, 1.0F, out);
  EXPECT_FLOAT_EQ(out[2], 60.0F);
  // Clamp outside the keyframe range.
  omnicpp::asset::sample_gltf_channel(sampler, -1.0F, out);
  EXPECT_FLOAT_EQ(out[1], 20.0F);
  omnicpp::asset::sample_gltf_channel(sampler, 2.0F, out);
  EXPECT_FLOAT_EQ(out[0], 40.0F);
}

TEST(GltfAnimation, CubicSplineTangentsSteerTheCurve) {
  // v0 = 0 with out-tangent 1; v1 = 1 with in-tangent 1 (dt = 1).
  const std::string json = R"glTF({"asset":{"version":"2.0"},
 "scenes":[{"nodes":[0]}],
 "nodes":[{"name":"a"}],
 "meshes":[],
 "animations":[{"samplers":[
   {"input":0,"output":1,"interpolation":"CUBICSPLINE"}],
  "channels":[
   {"sampler":0,"target":{"node":0,"path":"translation"}}]}],
 "bufferViews":[
   {"buffer":0,"byteOffset":0,"byteLength":8},
   {"buffer":0,"byteOffset":8,"byteLength":72}],
 "accessors":[
   {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
   {"bufferView":1,"componentType":5126,"count":6,"type":"VEC3"}],
 "buffers":[{"byteLength":80,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAAAA="}]})glTF";
  omnicpp::asset::GltfAnimationDocument doc;
  std::string error;
  const auto imported = omnicpp::asset::import_gltf_animation_document(
      json.data(), json.size(), nullptr, 0U, &error);
  ASSERT_TRUE(imported.is_ok()) << error;
  doc = std::move(imported).value();
  const auto& sampler = doc.animations[0].samplers[0];

  float out[4];
  // Quarter point: h00*0 + h10*1 + h01*1 + h11*1 = 0.078125 + 0.15625 +
  // 0.015625 = 0.25.
  omnicpp::asset::sample_gltf_channel(sampler, 0.25F, out);
  EXPECT_NEAR(out[0], 0.25F, 1e-4F);
}

TEST(GltfAnimation, CubicSplineRotationStaysUnit) {
  const std::string json = R"glTF({"asset":{"version":"2.0"},
 "scenes":[{"nodes":[0]}],
 "nodes":[{"name":"a"}],
 "meshes":[],
 "animations":[{"samplers":[
   {"input":0,"output":1,"interpolation":"CUBICSPLINE"}],
  "channels":[
   {"sampler":0,"target":{"node":0,"path":"rotation"}}]}],
 "bufferViews":[
   {"buffer":0,"byteOffset":0,"byteLength":8},
   {"buffer":0,"byteOffset":8,"byteLength":96}],
 "accessors":[
   {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
   {"bufferView":1,"componentType":5126,"count":6,"type":"VEC4"}],
 "buffers":[{"byteLength":104,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABTvwz4AAAAAXoNsPwAAAAAAAAAAAAAAAAAAgD8="}]})glTF";
  omnicpp::asset::GltfAnimationDocument doc;
  std::string error;
  const auto imported = omnicpp::asset::import_gltf_animation_document(
      json.data(), json.size(), nullptr, 0U, &error);
  ASSERT_TRUE(imported.is_ok()) << error;
  doc = std::move(imported).value();
  const auto& sampler = doc.animations[0].samplers[0];

  float out[4];
  omnicpp::asset::sample_gltf_channel(sampler, 0.5F, out);
  const float norm = std::sqrt(out[0] * out[0] + out[1] * out[1] +
                               out[2] * out[2] + out[3] * out[3]);
  EXPECT_NEAR(norm, 1.0F, 1e-5F);
  // Zero tangents: renormalized lerp = slerp = 22.5 degrees about Z.
  EXPECT_NEAR(out[3], 0.980785F, 1e-4F);
  EXPECT_NEAR(out[1], 0.195090F, 1e-4F);
}

TEST(GltfAnimation, CubicSplineRejectsWrongOutputCount) {
  // Output declares 2 keyframes (not 3x the 2 input times).
  const std::string json = R"glTF({"asset":{"version":"2.0"},
 "scenes":[{"nodes":[0]}],
 "nodes":[{"name":"a"}],
 "meshes":[],
 "animations":[{"samplers":[
   {"input":0,"output":1,"interpolation":"CUBICSPLINE"}],
  "channels":[
   {"sampler":0,"target":{"node":0,"path":"translation"}}]}],
 "bufferViews":[
   {"buffer":0,"byteOffset":0,"byteLength":8},
   {"buffer":0,"byteOffset":8,"byteLength":24}],
 "accessors":[
   {"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR"},
   {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],
 "buffers":[{"byteLength":32,"uri":"data:application/octet-stream;base64,AAAAAAAAgD8AACBBAACgQQAA8EEAACBCAABIQgAAcEI="}]})glTF";
  std::string error;
  const auto imported = omnicpp::asset::import_gltf_animation_document(
      json.data(), json.size(), nullptr, 0U, &error);
  EXPECT_FALSE(imported.is_ok());
  EXPECT_NE(error.find("3x .input count"), std::string::npos)
      << "error was: " << error;
}
