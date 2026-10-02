#include "warploom/render/vulkan_mesh_table.hpp"

#include <cstring>

namespace warploom::render {

namespace {

//! FNV-1a 64-bit over raw bytes. Deterministic across runs; used only as a
//! cheap prefilter — exact byte equality confirms duplicates.
[[nodiscard]] std::uint64_t fnv1a64(const void* data, std::size_t bytes) {
  const auto* p = static_cast<const unsigned char*>(data);
  std::uint64_t h = 14695981039346656037ULL;
  for (std::size_t i = 0; i < bytes; ++i) {
    h ^= static_cast<std::uint64_t>(p[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace

std::uint32_t SceneMeshTableBuilder::add(const SceneMesh& mesh,
                                         const float* vertex_data,
                                         std::size_t vertex_float_count,
                                         const std::uint32_t* index_data,
                                         std::size_t index_count) {
  (void)mesh;  // descriptor_set/buffers are the legacy draw path's concern.
  if (vertex_data == nullptr || index_data == nullptr ||
      vertex_float_count == 0U || index_count == 0U ||
      vertex_float_count % 11U != 0U) {
    return SceneMeshTableBuilder::kInvalidSlot;
  }

  const std::size_t vertex_bytes = vertex_float_count * sizeof(float);
  const std::size_t index_bytes = index_count * sizeof(std::uint32_t);
  const std::uint64_t hash =
      fnv1a64(vertex_data, vertex_bytes) ^ fnv1a64(index_data, index_bytes);

  // Dedup prefilter by hash, confirmed by exact byte comparison.
  for (const Accepted& prior : accepted_) {
    if (prior.hash != hash) continue;
    if (prior.vertex_float_count != vertex_float_count ||
        prior.index_count != index_count) {
      continue;
    }
    if (std::memcmp(build_.vertex_data.data() + prior.vertex_first,
                    vertex_data, vertex_bytes) == 0 &&
        std::memcmp(build_.index_data.data() + prior.index_first,
                    index_data, index_bytes) == 0) {
      // Exact duplicate: alias the first slot's ranges.
      const MeshTableEntry first = build_.entries[prior.slot];
      MeshTableEntry entry = first;
      entry.dedup_of = prior.slot;
      build_.entries.push_back(entry);
      build_.dedup_count += 1U;
      Accepted acc{hash, prior.vertex_first, vertex_float_count,
                   prior.index_first, index_count};
      acc.slot = static_cast<std::uint32_t>(build_.entries.size() - 1U);
      accepted_.push_back(acc);
      return static_cast<std::uint32_t>(build_.entries.size() - 1U);
    }
  }

  // New geometry: append vertex block, rewrite indices into the global
  // index space, and record the table entry.
  const std::size_t vertex_first = build_.vertex_data.size();
  const std::size_t index_first = build_.index_data.size();
  build_.vertex_data.insert(build_.vertex_data.end(), vertex_data,
                            vertex_data + vertex_float_count);
  // Rewrite local indices into the shared global VERTEX space: a global
  // index addresses global vertices; the draw command's firstIndex covers
  // the index-buffer offset separately.
  const std::uint32_t vertex_base_vertices =
      static_cast<std::uint32_t>(vertex_first / 11U);
  for (std::size_t i = 0; i < index_count; ++i) {
    build_.index_data.push_back(index_data[i] + vertex_base_vertices);
  }

  MeshTableEntry entry;
  entry.index_count = static_cast<std::uint32_t>(index_count);
  entry.index_offset = static_cast<std::uint32_t>(index_first);
  entry.vertex_base = static_cast<std::uint32_t>(vertex_first);
  entry.vertex_count =
      static_cast<std::uint32_t>(vertex_float_count / 11U);
  entry.dedup_of = 0xFFFFFFFFU;
  build_.entries.push_back(entry);

  Accepted acc{hash, vertex_first, vertex_float_count, index_first,
               index_count};
  acc.slot = static_cast<std::uint32_t>(build_.entries.size() - 1U);
  accepted_.push_back(acc);
  return acc.slot;
}

std::uint32_t SceneMeshTableBuilder::add(
    const SceneMesh& mesh, const std::vector<float>& vertices,
    const std::vector<std::uint32_t>& indices) {
  return add(mesh, vertices.data(), vertices.size(), indices.data(),
             indices.size());
}

void SceneMeshTableBuilder::clear() noexcept {
  accepted_.clear();
  build_ = MeshTableBuild{};
}

}  // namespace warploom::render
