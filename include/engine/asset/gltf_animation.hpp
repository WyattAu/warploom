#pragma once

/**
 * @file gltf_animation.hpp
 * @brief Deterministic, dependency-free glTF 2.0 skeletal ingestion:
 *        skins, node hierarchies, animations, and skinned vertex bindings.
 *
 * Composes with `import_gltf_mesh` (which provides the proven geometry /
 * material path): each unique mesh referenced by a node is imported through
 * the canonical 11-float vertex pipeline, then JOINTS_0/WEIGHTS_0 slices are
 * concatenated primitive-by-primitive in `GltfPrimitiveMaterial` order so
 * skin data aligns with the merged vertex stream.
 *
 * Node graph: the full `nodes` array is preserved with local TRS components
 * (never baked), because animations drive TRS per frame. Absolute models are
 * intentionally NOT flattened here — a skinned mesh is drawn in the space of
 * its skeleton, not its node's world transform. Hierarchy cycles, duplicate
 * channel targets, and out-of-range references are rejected with descriptive
 * diagnostics; nothing is silently repaired.
 *
 * Animation sampling (`sample_gltf_channel`) is deterministic: exact
 * keyframe hits return the exact keyframe; times outside the keyframe range
 * clamp to the boundary; LINEAR interpolates component-wise (rotations use
 * shortest-arc slerp with nlerp fallback near parallel); STEP holds the
 * previous keyframe; CUBICSPLINE is rejected at import.
 */

#include "engine/asset/gltf_importer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omnicpp::asset {

//! Index of a node with no parent (root of the node forest).
inline constexpr std::size_t kGltfNoParent = static_cast<std::size_t>(-1);

//! One node of the preserved hierarchy. Local TRS is kept decomposed because
//! animation channels overwrite these components every frame.
struct GltfSkinNode {
  std::string name{};
  std::size_t parent{kGltfNoParent};
  std::vector<std::size_t> children{};
  //! Mesh binding (index into GltfAnimationDocument::meshes); SIZE_MAX-free:
  //! use has_mesh instead.
  bool has_mesh{false};
  std::size_t mesh_index{0};
  //! Skinned-mesh binding (index into GltfAnimationDocument::skins).
  std::size_t skin{0};
  bool has_skin{false};
  float translation[3]{0.0f, 0.0f, 0.0f};
  //! xyzw quaternion, normalized at import.
  float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float scale[3]{1.0f, 1.0f, 1.0f};
  //! False when the node declares none of translation/rotation/scale (the
  //! sampler contract still writes all three; identity then).
  bool has_trs{false};
};

//! One skin: joint node order defines the bone-matrix upload order.
struct GltfSkinImport {
  std::string name{};
  //! Node indices of the joints, in glTF order (the order the runtime must
  //! upload skinning matrices in).
  std::vector<std::size_t> joints{};
  //! Inverse bind matrices (column-major), one per joint, same order.
  std::vector<GltfTransform> inverse_bind_matrices{};
  //! Optional skeleton root (glTF `skin.skeleton`); kGltfNoParent when absent.
  std::size_t skeleton_root{kGltfNoParent};
};

//! Interpolation mode of one animation sampler.
enum class GltfSamplerInterpolation { Linear, Step };

//! Pre-decoded keyframes of one animation sampler. `values` holds
//! `count * stride` floats (stride 3 for translation/scale, 4 for rotation).
struct GltfSampler {
  GltfSamplerInterpolation interpolation{GltfSamplerInterpolation::Linear};
  std::vector<float> times{};
  std::vector<float> values{};
  std::size_t stride{0};  //!< floats per keyframe (3 or 4)
};

//! One animation channel: sampler drives one node component over time.
struct GltfChannel {
  enum class Path { Translation, Rotation, Scale };
  std::size_t sampler{0};
  std::size_t target_node{0};
  Path path{Path::Translation};
};

//! One playable animation clip.
struct GltfAnimationImport {
  std::string name{};
  //! Largest input time across the animation's samplers (0 when empty).
  float duration{0.0f};
  std::vector<GltfSampler> samplers{};
  std::vector<GltfChannel> channels{};
};

//! JOINTS_0/WEIGHTS_0 data of one skinned mesh, aligned with the merged
//! vertex stream of the matching GltfMeshImport: 4 joint indices and 4
//! weights per vertex, index `v` reads elements [4*v, 4*v+4). Joint values
//! are skin-local joint indices (0..joint_count-1), normalized weights
//! summing to 1 (already renormalized when the asset does not).
struct GltfSkinBinding {
  std::vector<std::uint16_t> joints{};
  std::vector<float> weights{};
};

//! Whole-document skeletal import: geometry via the canonical mesh path,
//! plus the node forest, skins, animations, and per-mesh skin bindings.
struct GltfAnimationDocument {
  //! Unique meshes referenced by any node, in first-encounter order
  //! (imported through import_gltf_mesh with full material fidelity).
  std::vector<GltfMeshImport> meshes{};
  //! Mesh -> skin binding (same index as `meshes`); empty when the mesh
  //! bears no skin.
  std::vector<GltfSkinBinding> skin_bindings{};
  //! The complete node forest in document order (indices are stable).
  std::vector<GltfSkinNode> nodes{};
  std::vector<GltfSkinImport> skins{};
  std::vector<GltfAnimationImport> animations{};
};

/**
 * @brief Import a glTF document with skins and animations.
 *
 * Accepts the same document/binary inputs as import_gltf_mesh. Rejections
 * include everything import_gltf_mesh rejects for geometry, plus: node
 * `matrix` (animations drive TRS), node cycles, skin without
 * inverseBindMatrices, IBM count != joint count, out-of-range or duplicate
 * joint indices, CUBICSPLINE samplers, duplicate (node, path) channels,
 * channel targets with a `weights` path, and non-monotonic sampler input.
 */
[[nodiscard]] omnicpp::core::Result<GltfAnimationDocument>
import_gltf_animation_document(const char* json_bytes, std::size_t json_len,
                               const std::uint8_t* bin_bytes,
                               std::size_t bin_len,
                               std::string* error_detail = nullptr);

//! Compose local TRS into a column-major matrix (translation * rotation *
//! scale).
[[nodiscard]] GltfTransform gltf_local_matrix(const GltfSkinNode& node) noexcept;

//! Compose each node's local matrix down the hierarchy into absolute
//! (model-space) matrices. `out` must have nodes.size() entries; nodes are
//! visited parent-before-child (import guarantees the forest is acyclic).
void gltf_global_matrices(const GltfAnimationDocument& document,
                          const std::vector<GltfTransform>& locals,
                          std::vector<GltfTransform>& out_globals) noexcept;

//! Skinning matrices for one skin: global_joint * inverse_bind, in joint
//! order — ready for the bone SSBO the skinned pipeline consumes.
void gltf_skin_matrices(const GltfAnimationDocument& document,
                        std::size_t skin_index,
                        const std::vector<GltfTransform>& globals,
                        std::vector<GltfTransform>& out_joint_matrices);

//! Sample one channel's sampler at `time`. Exact keyframe hits return the
//! exact keyframe; out-of-range times clamp; LINEAR lerps component-wise
//! (rotations: shortest-arc slerp, nlerp within 0.9995 of parallel). Writes
//! `sampler.stride` floats into `out` (out[0..3]; rotation is xyzw).
void sample_gltf_channel(const GltfSampler& sampler, float time,
                         float* out) noexcept;

}  // namespace omnicpp::asset
