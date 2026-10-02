#pragma once

/**
 * @file vulkan_mesh_table.hpp
 * @brief CPU geometry merge/dedup for the GPU-driven draw path.
 *
 * GPU-driven rendering (docs/gpu-driven-rendering.md) needs every mesh's
 * vertices in shared SSBOs and its indices in ONE index buffer, addressed
 * by per-mesh table entries. SceneMeshTableBuilder accepts SceneMesh
 * geometry (11 floats/vertex, uint32 indices — the engine's standard
 * layout), rewrites indices into the concatenated space, and dedups
 * identical geometry (byte-wise on vertex+index streams). The output is
 * the exact bytes to upload plus the per-mesh table the vertex-pull
 * shader and the GPU cull pass consume.
 *
 * Pure CPU: no Vulkan objects, headless-testable. Deterministic (stable
 * order, no pointers hashed).
 */

#include "warploom/render/vulkan_scene.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>
namespace warploom::render {

//! One mesh-table entry (std430-aligned CPU struct; the GPU mirror packs
//! index_count/index_offset/vertex_base first so a uvec4 load matches).
struct MeshTableEntry {
  std::uint32_t index_count{0};      //!< Indices for this mesh.
  std::uint32_t index_offset{0};     //!< FirstIndex into the shared buffer.
  std::uint32_t vertex_base{0};      //!< Float offset of the vertex block.
  std::uint32_t vertex_count{0};     //!< Vertices in the block.
  std::uint32_t dedup_of{0xFFFFFFFFU};  //!< Table slot this duplicates.
};

//! Result of a build: upload payloads + the table.
struct MeshTableBuild {
  std::vector<float> vertex_data;        //!< All vertex blocks concatenated.
  std::vector<std::uint32_t> index_data; //!< Rewritten global indices.
  std::vector<MeshTableEntry> entries;   //!< One per added mesh, in order.
  std::size_t dedup_count{0};            //!< How many adds were dedup hits.

  //! Byte sizes for buffer allocation.
  [[nodiscard]] VkDeviceSize vertex_bytes() const noexcept {
    return vertex_data.size() * sizeof(float);
  }
  [[nodiscard]] VkDeviceSize index_bytes() const noexcept {
    return index_data.size() * sizeof(std::uint32_t);
  }
};

class SceneMeshTableBuilder final {
public:
  SceneMeshTableBuilder() = default;

  //! Add one mesh's geometry. The SceneMesh's buffers are read as host
  //! pointers (set vertex_data/index_data explicitly; descriptor_set is
  //! ignored). Returns the table slot index, or kInvalidSlot on failure.
  [[nodiscard]] std::uint32_t add(const SceneMesh& mesh,
                                  const float* vertex_data,
                                  std::size_t vertex_float_count,
                                  const std::uint32_t* index_data,
                                  std::size_t index_count);

  //! Convenience overload: reads pointers from host vectors.
  [[nodiscard]] std::uint32_t add(const SceneMesh& mesh,
                                  const std::vector<float>& vertices,
                                  const std::vector<std::uint32_t>& indices);

  //! Finish: returns the merged payloads + table. The builder keeps its
  //! state (builds are additive until clear()).
  [[nodiscard]] const MeshTableBuild& build() const noexcept { return build_; }
  void clear() noexcept;

  [[nodiscard]] std::size_t mesh_count() const noexcept {
    return build_.entries.size();
  }

  static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFU;

private:
  //! One accepted mesh's payload (kept for exact-match comparisons).
  struct Accepted {
    std::uint64_t hash;
    std::size_t vertex_first;      // offset into build_.vertex_data
    std::size_t vertex_float_count;
    std::size_t index_first;       // offset into build_.index_data
    std::size_t index_count;
    std::uint32_t slot;            // table slot this payload backs
  };
  std::vector<Accepted> accepted_;
  MeshTableBuild build_;
};

}  // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef OMNICPP_COMPAT_RENDER_NS
#define OMNICPP_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // OMNICPP_COMPAT_RENDER_NS
