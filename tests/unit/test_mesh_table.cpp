//! @file test_mesh_table.cpp
//! @brief CPU tests for SceneMeshTableBuilder: index rewriting into the
//!        shared global space, byte-exact geometry dedup, invalid-input
//!        rejection, and clear semantics. Pure CPU — runs headless.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "warploom/render/vulkan_mesh_table.hpp"

namespace {

//! Minimal drawable SceneMesh shell (buffer handles unused by the builder).
omnicpp::render::SceneMesh shell_mesh() {
  omnicpp::render::SceneMesh mesh{};
  mesh.index_count = 36U;
  return mesh;
}

//! Build two distinguishable "meshes": cube A (vertex 0 at x=-1) and cube B
//! (vertex 0 at x=+2). 11 floats/vertex, 24 vertices, 36 indices — the
//! engine's standard layout. Only the first vertex differs, so the streams
//! are NOT byte-equal.
void make_variant(float first_x, std::vector<float>& vertices,
                  std::vector<std::uint32_t>& indices) {
  vertices.assign(24U * 11U, 0.0f);
  for (std::uint32_t v = 0; v < 24U; ++v) {
    vertices[v * 11U + 0U] = (v == 0U) ? first_x : 0.5f;
    vertices[v * 11U + 1U] = 0.25f;
    vertices[v * 11U + 2U] = 0.75f;
    // color(3) + normal(3) + uv(2) left zero: irrelevant to the builder.
  }
  indices.assign(36U, 0U);
  for (std::uint32_t i = 0; i < 36U; ++i) indices[i] = i % 24U;
}

}  // namespace

TEST(MeshTable, RewritesIndicesIntoGlobalSpace) {
  omnicpp::render::SceneMeshTableBuilder builder;
  std::vector<float> va, vb;
  std::vector<std::uint32_t> ia, ib;
  make_variant(-1.0f, va, ia);
  make_variant(2.0f, vb, ib);

  const auto slot_a = builder.add(shell_mesh(), va, ia);
  const auto slot_b = builder.add(shell_mesh(), vb, ib);
  ASSERT_NE(slot_a, omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);
  ASSERT_NE(slot_b, omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);

  const auto& build = builder.build();
  ASSERT_EQ(build.entries.size(), 2U);
  // Second mesh's indices are shifted by the first mesh's index count.
  EXPECT_EQ(build.entries[0].index_offset, 0U);
  EXPECT_EQ(build.entries[1].index_offset, 36U);
  EXPECT_EQ(build.index_data.size(), 72U);
  for (std::size_t i = 0; i < 36U; ++i) {
    EXPECT_EQ(build.index_data[i], ia[i]);
    EXPECT_EQ(build.index_data[36U + i], ib[i] + 24U);
  }
  // Vertex blocks concatenated in add order; table records float offsets.
  EXPECT_EQ(build.entries[0].vertex_base, 0U);
  EXPECT_EQ(build.entries[1].vertex_base, 24U * 11U);
  EXPECT_EQ(build.vertex_data.size(), 2U * 24U * 11U);
}

TEST(MeshTable, DedupsByteIdenticalGeometry) {
  omnicpp::render::SceneMeshTableBuilder builder;
  std::vector<float> v;
  std::vector<std::uint32_t> i;
  make_variant(-1.0f, v, i);

  const auto slot_a = builder.add(shell_mesh(), v, i);
  const auto slot_b = builder.add(shell_mesh(), v, i);
  ASSERT_NE(slot_a, omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);
  ASSERT_NE(slot_b, omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);

  const auto& build = builder.build();
  ASSERT_EQ(build.entries.size(), 2U);
  EXPECT_EQ(build.dedup_count, 1U);
  // The duplicate aliases the first slot's ranges — no second upload.
  EXPECT_EQ(build.entries[1].dedup_of, slot_a);
  EXPECT_EQ(build.entries[1].index_offset, build.entries[0].index_offset);
  EXPECT_EQ(build.entries[1].index_count, build.entries[0].index_count);
  EXPECT_EQ(build.entries[1].vertex_base, build.entries[0].vertex_base);
  EXPECT_EQ(build.vertex_data.size(), 24U * 11U);   // one copy only
  EXPECT_EQ(build.index_data.size(), 36U);          // one copy only
}

TEST(MeshTable, RejectsInvalidGeometry) {
  omnicpp::render::SceneMeshTableBuilder builder;
  const std::vector<float> bad(10U, 0.0f);  // not a multiple of 11 floats
  const std::vector<std::uint32_t> idx(3U, 0U);
  EXPECT_EQ(builder.add(shell_mesh(), bad, idx),
            omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);
  EXPECT_EQ(builder.add(shell_mesh(), {}, {}),
            omnicpp::render::SceneMeshTableBuilder::kInvalidSlot);
  EXPECT_EQ(builder.mesh_count(), 0U);
}

TEST(MeshTable, ClearResetsState) {
  omnicpp::render::SceneMeshTableBuilder builder;
  std::vector<float> v;
  std::vector<std::uint32_t> i;
  make_variant(-1.0f, v, i);
  (void)builder.add(shell_mesh(), v, i);
  builder.clear();
  EXPECT_EQ(builder.mesh_count(), 0U);
  EXPECT_TRUE(builder.build().vertex_data.empty());
  EXPECT_TRUE(builder.build().index_data.empty());

  // After clear, the same geometry is a fresh slot 0 (not a dedup hit).
  const auto slot = builder.add(shell_mesh(), v, i);
  EXPECT_EQ(slot, 0U);
  EXPECT_EQ(builder.build().dedup_count, 0U);
}
