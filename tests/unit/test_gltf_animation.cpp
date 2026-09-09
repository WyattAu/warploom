//! @file test_gltf_animation.cpp
//! @brief Skeletal glTF ingestion proofs against the generated mannequin
//!        asset (scripts/generate_mannequin.py): skins/nodes/animations
//!        import, rest-pose skinning is exactly the bind pose, walk-cycle
//!        sampling matches its keyframes, byte-exact determinism, and
//!        rejection of malformed skeletal documents.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/asset/gltf_animation.hpp"

#ifndef OMNICPP_TEST_ASSET_DIR
#define OMNICPP_TEST_ASSET_DIR "assets/models"
#endif
#include "engine/asset/gltf_importer.hpp"

namespace {

//! Directory of the generated mannequin asset: provided at configure time
//! (like OMNICPP_TEST_SHADER_DIR) with a repo-relative fallback.
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
      OMNICPP_TEST_ASSET_DIR[0] != 0 ? OMNICPP_TEST_ASSET_DIR
                                     : "assets/models";
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
  EXPECT_EQ(doc.animations.size(), 1U);
  EXPECT_EQ(doc.animations[0].channels.size(), 7U);
  EXPECT_NEAR(doc.animations[0].duration, 1.0f, 1e-6f);
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
  auto doc = std::move(result.value());

  // Rest pose: global(j) * IBM(j) must be identity for every joint.
  std::vector<omnicpp::asset::GltfTransform> globals;
  omnicpp::asset::gltf_global_matrices(doc, test_locals(doc), globals);
  const auto& skin = doc.skins[0];
  for (std::size_t j = 0; j < skin.joints.size(); ++j) {
    const auto& g = globals[skin.joints[j]];
    const auto& ibm = skin.inverse_bind_matrices[j];
    float product[16]{};
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
          sum += g[static_cast<std::size_t>(r + 4 * k)] *
                 ibm[static_cast<std::size_t>(k + 4 * c)];
        }
        product[r + 4 * c] = sum;
      }
    }
    for (int c = 0; c < 16; ++c) {
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
        for (int r = 0; r < 3; ++r) {
          skinned[r] += w * (jm[r + 0] * mesh.vertices[base + 0] +
                             jm[r + 4] * mesh.vertices[base + 1] +
                             jm[r + 8] * mesh.vertices[base + 2] +
                             jm[r + 12]);
        }
      }
      EXPECT_NEAR(weight_sum, 1.0f, 1e-5f);
      for (int r = 0; r < 3; ++r) {
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
  auto doc = std::move(result.value());
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
  auto doc = std::move(result.value());
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

  // Node matrix (skinned docs must drive TRS).
  {
    error.clear();
    auto r = import(R"({"asset":{"version":"2.0"},
      "scenes":[{"nodes":[0]}],
      "nodes":[{"name":"n","matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}],
      "meshes":[],"skins":[],
      "buffers":[],"bufferViews":[],"accessors":[]})");
    EXPECT_FALSE(r.is_ok());
    EXPECT_NE(error.find("matrix"), std::string::npos)
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
  // CUBICSPLINE sampler.
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
    EXPECT_NE(error.find("CUBICSPLINE"), std::string::npos)
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
