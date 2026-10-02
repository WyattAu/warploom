#pragma once

/**
 * @file gltf_importer.hpp
 * @brief Deterministic, dependency-free glTF 2.0 static-mesh ingestion.
 *
 * Converts a glTF 2.0 document (JSON + optional binary buffer) into the
 * engine's canonical GPU mesh format:
 *   - eleven floats per vertex: position.xyz, linear color.rgb, normal.xyz,
 *     and uv.xy. Missing optional attributes receive defaults: color (1,1,1),
 *     normal (0,0,1), uv (0,0);
 *   - one triangle-list index stream of uint32;
 *   - the first primitive's material base color factor (opaque white default).
 *
 * Supported subset (rigorously validated, everything else is rejected with a
 * descriptive error instead of being mis-decoded):
 *   - one or many `meshes`, each merged primitive-by-primitive into a single
 *     vertex/index stream with one per-primitive material record
 *     (`GltfMeshImport::primitives`), so multi-material meshes import
 *     completely and a render layer can split them into per-material draws
 *     (mode TRIANGLES only);
 *   - POSITION (VEC3 float32), optional COLOR_0 (VEC3/VEC4 float32), optional
 *     NORMAL (VEC3 float32), optional TEXCOORD_0 (VEC2 float32). Optional
 *     attributes must match the POSITION vertex count;
 *   - indices SCALAR uint16 or uint32 (non-indexed primitives become
 *     sequential indices);
 *   - tightly packed or byteStride-interleaved bufferViews over one buffer;
 *   - single external .bin buffers (buffer 0 served by the caller-provided
 *     bytes) and `data:...;base64,...` embedded buffers;   * - optional glTF `samplers`/`textures`/`images` wiring: every material's
   *     `pbrMetallicRoughness.baseColorTexture` (set TEXCOORD_0) is decoded to
   *     an RGBA8 `GltfImage` when a primitive uses it. Bytes stay in the
   *     payload's encoded form; each binding's `encoded_srgb` flag tells the
   *     render layer that `baseColorTexture` is an sRGB colour texture to be
   *     sampled through an sRGB image format (hardware linearisation). Image
   *     payloads are PNG or baseline JPEG, embedded as `data:image/...;base64,`
   *     URIs or stored in a byteStride-free bufferView; sampler filters/wraps
   *     are validated and reported for the render layer to map onto its
 *     sampler. A texture referenced by several primitives of one mesh is
 *     decoded exactly once; unrelated images never fail the import.
 *
 *   - image payloads stored in *external files* (glTF `images[i].uri` that is
 *     not a `data:` URI) are fetched lazily through an optional
 *     `ExternalFileLoader` callback, so the importer itself stays free of
 *     filesystem access and meshes whose materials never reference such an
 *     image import without touching disk. Without a loader, referencing an
 *     external image file is a hard error with a descriptive diagnostic.
 *
 * Whole-scene import (`import_gltf_scene`) additionally consumes the
 * `scenes`/`nodes` graph: node `matrix` or TRS transforms are composed down
 * the hierarchy and flattened into absolute column-major world models, one
 * instance per mesh-bearing node (group nodes contribute transforms only,
 * shared subtrees instantiate once per path, meshes are imported once each).
 *
 * Explicitly rejected with `RuntimeError::malformed_asset`: sparse accessors,
 * non-TRIANGLES primitive modes, unsupported component types, matrices, any
 * external buffer beyond the first, GLB-style buffers without a URI, any
 * reference whose range falls outside its declared buffer, images declaring
 * both a uri and a bufferView, strided image
 * bufferViews, non-zero baseColorTexture texCoord sets, out-of-range texture/
 * image/sampler/material indices (on any primitive), unknown sampler
 * filter/wrap enums, image payloads that are not decodable PNG or baseline
 * JPEG, external image files when no loader is provided (or when the loader
 * cannot fetch them), and — for
 * scenes — node cycles, skinned nodes, nodes declaring both `matrix` and
 * TRS, and malformed TRS/matrix arrays.
 *
 * The importer never allocates except for the returned vectors, never throws,
 * and is deterministic: identical inputs always produce identical outputs.
 * Binary data is decoded as little-endian per the glTF specification.
 */

#include "warploom/core/deterministic_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace warploom::asset {

//! Optional callback that resolves an external image file (a glTF
//! `images[i].uri` that is not a `data:` URI) into its raw file bytes. The
//! importer stays free of filesystem access: the caller decides how `uri`
//! maps to storage (a directory, a virtual file system, an archive, ...) and
//! how percent-escaped URIs decode. Return false — writing a reason into
//! `error_detail` — when the URI cannot be resolved. The callback is only
//! invoked when an imported material actually references the image.
using ExternalFileLoader = std::function<bool(
    const std::string& uri, std::string& error_detail,
    std::vector<std::uint8_t>& bytes)>;

//! Floats per vertex in the canonical GPU vertex stream: position.xyz,
//! color.rgb, normal.xyz, uv.xy.
inline constexpr std::size_t kSceneVertexFloats = 11U;

//! Axis-aligned bounding box of the imported mesh in local/model space,
//! derived from the POSITION accessor min/max of every primitive.
struct GltfBounds {
  float min[3]{0.0f, 0.0f, 0.0f};
  float max[3]{0.0f, 0.0f, 0.0f};
  //! False when any primitive's POSITION accessor lacks valid min/max; such a
  //! mesh must not be frustum-culled without user-provided bounds.
  bool valid{false};
};

//! One decoded glTF image: tightly packed 8-bit RGBA (straight alpha) in the
//! payload's *encoded* form — PNG/JPEG decoding never colour-transforms on
//! the CPU (an sRGB-to-linear pass would quantise to 8-bit linear). Colour
//! use is signalled per binding by `GltfMaterialTexture::encoded_srgb`, so
//! uploaders can sample colour images through an sRGB image format and let
//! the hardware linearise; non-colour images are sampled as-is.
struct GltfImage {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba{};
};

//! Optional base-colour texture of the imported material
//! (pbrMetallicRoughness.baseColorTexture on TEXCOORD_0).
struct GltfMaterialTexture {
  //! False when the material has no baseColorTexture.
  bool present{false};
  //! True when this binding is an sRGB-encoded *colour* texture (glTF
  //! `baseColorTexture`). Consumers that sample the image through this
  //! binding must create the image/view in an sRGB format (e.g.
  //! VK_FORMAT_R8G8B8A8_SRGB) so the hardware decodes to linear before
  //! lighting — the canonical convention. Non-colour data bindings (normal /
  //! occlusion / metallic-roughness textures) set this false.
  bool encoded_srgb{true};
  //! Index into GltfMeshImport::images of the decoded albedo image.
  std::size_t image_index{0};
  //! Sampler state declared by the referenced glTF sampler. Filter values are
  //! 0 when unspecified (glTF leaves the choice to the implementation); wrap
  //! defaults are glTF's REPEAT (10497). Values use the glTF enum numbers so
  //! the render layer maps them without interpretation.
  std::uint32_t mag_filter{0};
  std::uint32_t min_filter{0};
  std::uint32_t wrap_s{10497};
  std::uint32_t wrap_t{10497};
};

//! Material binding of one glTF primitive. Geometry of every primitive is
//! merged into the mesh's single vertex/index stream; each record names its
//! contiguous slice of that stream plus the material that draws it, so a
//! render layer can split a multi-material mesh into per-material draws
//! without re-reading the document.
struct GltfPrimitiveMaterial {
  //! Slice of GltfMeshImport::indices drawn by this primitive.
  std::size_t index_offset{0};
  std::size_t index_count{0};
  //! Slice of the merged vertex stream this primitive's indices reference.
  //! Vertices are appended per primitive, so the slice is contiguous.
  std::size_t vertex_offset{0};
  std::size_t vertex_count{0};
  //! glTF material name, or empty when the primitive binds no material
  //! (glTF's default material: opaque white, no texture).
  std::string material_name{};
  //! Material base color factor (pbrMetallicRoughness.baseColorFactor), or
  //! opaque white for the default material.
  std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
  //! The material's optional albedo texture binding (image_index references
  //! GltfMeshImport::images).
  GltfMaterialTexture albedo{};
};

//! Interleaved GPU-ready static mesh. Vertices are eleven floats per vertex
//! (position.xyz, linear color.rgb, normal.xyz, uv.xy); indices index into
//! that vertex list. See kSceneVertexFloats.
struct GltfMeshImport {
  std::vector<float> vertices{};
  std::vector<std::uint32_t> indices{};
  //! One material record per glTF primitive, in document order, each naming
  //! its contiguous index/vertex slice of the merged streams. A mesh whose
  //! primitives all share one material still records one entry per primitive
  //! (all identical); merge them when the render layer only needs draws.
  std::vector<GltfPrimitiveMaterial> primitives{};
  //! Material base color factor of the first primitive that binds a material
  //! (pbrMetallicRoughness.baseColorFactor), or opaque white. Convenience
  //! alias of the matching entry in `primitives`; prefer `primitives` for
  //! multi-material meshes.
  std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
  //! Images decoded from the primitives' baseColorTexture chains (populated
  //! only when an imported primitive actually references one; each referenced
  //! texture appears exactly once even when several primitives share it).
  std::vector<GltfImage> images{};
  //! Albedo texture binding of the first primitive that binds a material
  //! (image_index references `images`). Convenience alias; prefer
  //! `primitives` for multi-material meshes.
  GltfMaterialTexture albedo{};
  std::string name{};
  //! Local-space bounds for the whole merged mesh (union across primitives)
  //! when every primitive declared POSITION min/max.
  GltfBounds bounds{};

  [[nodiscard]] std::uint32_t vertex_count() const noexcept {
    return static_cast<std::uint32_t>(vertices.size() / kSceneVertexFloats);
  }
  [[nodiscard]] bool empty() const noexcept {
    return vertices.empty() || indices.empty();
  }
};

/**
 * @brief Import one glTF mesh as an engine mesh.
 * @param json_bytes  UTF-8 glTF JSON document (must stay valid for the call).
 * @param json_len    Length of @p json_bytes in bytes.
 * @param bin_bytes   Contents of the single external .bin buffer (buffer 0
 *                    with a non-data URI). May be null when empty.
 * @param bin_len     Length of @p bin_bytes. Must equal the declared
 *                    `buffers[0].byteLength` when that buffer is external.
 * @param mesh_index  Index into the document's `meshes` array.
 * @param error_detail Optional out-parameter receiving a human-readable
 *                    reason when the import fails (never written on success).
 * @param loader      Optional resolver for external image files (see
 *                    `ExternalFileLoader`). When null, an imported material
 *                    that references an external image file fails with a
 *                    diagnostic instead of touching the filesystem.
 */
[[nodiscard]] ::warploom::core::Result<GltfMeshImport> import_gltf_mesh(
    const char* json_bytes, std::size_t json_len,
    const std::uint8_t* bin_bytes, std::size_t bin_len,
    std::size_t mesh_index = 0, std::string* error_detail = nullptr,
    const ExternalFileLoader* loader = nullptr);

//! Column-major 4x4 model transform (translation in [12],[13],[14]), layout-
//! compatible with the render scene path's per-object model matrices.
using GltfTransform = std::array<float, 16>;

[[nodiscard]] constexpr GltfTransform gltf_identity_transform() noexcept {
  return GltfTransform{1.0f, 0.0f, 0.0f, 0.0f,
                       0.0f, 1.0f, 0.0f, 0.0f,
                       0.0f, 0.0f, 1.0f, 0.0f,
                       0.0f, 0.0f, 0.0f, 1.0f};
}

//! A whole glTF scene flattened for rendering: every mesh-bearing node becomes
//! one entry with an absolute (world-space) model transform. Pure group nodes
//! are not emitted; their transforms are folded into their descendants.
struct GltfSceneImport {
  //! Unique meshes referenced by the scene's mesh nodes, in first-encounter
  //! order (shared meshes are imported exactly once).
  std::vector<GltfMeshImport> meshes{};
  //! Mesh instances in depth-first order.
  struct Node {
    std::string name{};
    bool has_mesh{false};
    //! Index into GltfSceneImport::meshes when has_mesh.
    std::size_t mesh_index{0};
    //! Absolute model transform (all ancestor transforms composed in).
    GltfTransform model{gltf_identity_transform()};
  };
  std::vector<Node> nodes{};

  [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }
};

/**
 * @brief Import one glTF scene (its `scenes[scene_index]` roots) as
 *        flattened world-space mesh instances.
 *
 * Accepts the same document/binary inputs as import_gltf_mesh (including the
 * optional `loader` for external image files). Node hierarchy
 * is depth-first traversed from the scene roots; `matrix` (column-major, 16
 * numbers) and TRS (translation + quaternion rotation + scale) transforms are
 * validated and composed into absolute models. Skinned nodes, cycles, and
 * nodes mixing `matrix` with TRS are rejected. Unreferenced meshes are not
 * imported.
 */
[[nodiscard]] ::warploom::core::Result<GltfSceneImport> import_gltf_scene(
    const char* json_bytes, std::size_t json_len,
    const std::uint8_t* bin_bytes, std::size_t bin_len,
    std::size_t scene_index = 0, std::string* error_detail = nullptr,
    const ExternalFileLoader* loader = nullptr);

} // namespace warploom::asset

// S5-B compat footer: legacy `omnicpp::asset` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef WARPLOOM_COMPAT_ASSET_NS
#define WARPLOOM_COMPAT_ASSET_NS
namespace omnicpp::asset {
    using namespace ::warploom::asset;
}
#endif  // WARPLOOM_COMPAT_ASSET_NS
