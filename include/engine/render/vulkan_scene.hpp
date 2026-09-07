#pragma once

/**
 * @file vulkan_scene.hpp
 * @brief Immutable scene snapshots and generation-checked render resources.
 *
 * Gameplay/ECS code extracts a snapshot before command recording. The renderer
 * never walks mutable ECS state. GPU objects remain externally owned, while
 * VulkanSceneResourceRegistry supplies stable handles and prevents a retired
 * slot from being reused until its frame/timeline value has completed.
 */

#include "engine/core/ecs.hpp"
#include "engine/render/vulkan_types.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace omnicpp::render {

using SceneMatrix = std::array<float, 16>;

struct SceneCamera {
  SceneMatrix view_projection{};
};

[[nodiscard]] constexpr SceneMatrix scene_identity_matrix() noexcept {
  return SceneMatrix{1.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 1.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 1.0f};
}

//! Stable generational handle. A handle is invalid after destroy() and cannot
//! resolve again until the slot is recycled with a new generation.
template <typename Tag>
struct SceneResourceHandle {
  std::uint32_t index{0xffffffffU};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return index != 0xffffffffU && generation != 0U;
  }
  friend constexpr bool operator==(const SceneResourceHandle&, const SceneResourceHandle&) = default;
};

struct MeshResourceTag;
struct MaterialResourceTag;
struct TextureResourceTag;
using MeshHandle = SceneResourceHandle<MeshResourceTag>;
using MaterialHandle = SceneResourceHandle<MaterialResourceTag>;
using TextureHandle = SceneResourceHandle<TextureResourceTag>;

//! GPU-resident indexed mesh. Buffers and descriptor sets are externally
//! owned by the Vulkan resource backend and must outlive recorded frames.
struct SceneMesh {
  VkBuffer vertex_buffer{VK_NULL_HANDLE};
  VkBuffer index_buffer{VK_NULL_HANDLE};
  VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
  VkDeviceSize index_offset{0};
  std::uint32_t index_count{0};

  [[nodiscard]] bool is_drawable() const noexcept {
    return vertex_buffer != VK_NULL_HANDLE && index_buffer != VK_NULL_HANDLE &&
           descriptor_set != VK_NULL_HANDLE && index_count != 0U;
  }
};

//! GPU-resident 2D texture used as an albedo (base color) map by the lit
//! material path. The VkImage backing `view` is externally owned (like
//! SceneMesh buffers) and must outlive recorded frames; `bindless_index` is
//! the element of the renderer-bound bindless albedo array (element 0 is the
//! opaque-white fallback, so untextured materials sample it unchanged).
struct SceneTexture {
  VkImageView view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  std::uint32_t bindless_index{0};

  [[nodiscard]] bool is_valid() const noexcept {
    return view != VK_NULL_HANDLE && sampler != VK_NULL_HANDLE;
  }
};

//! Material data used by the material-aware scene path: opaque base color in
//! linear RGBA plus an optional albedo texture (glTF `baseColorTexture`).
//! Trailing members keep aggregate initializers `{base_color}` working.
struct SceneMaterial {
  std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
  //! Optional base-color texture; resolved against the registry during
  //! extraction into SceneObject::albedo_value.
  TextureHandle albedo{};
};

// ---------------------------------------------------------------------------
// PBR material data (glTF 2.0 metallic-roughness workflow).
// ---------------------------------------------------------------------------

//! Flags controlling which bindless texture indices are active for a PBR draw.
//! Bit values match the fragment shader's flag constants.
enum class PbrMaterialFlags : std::uint32_t {
  kNone        = 0x00,
  kHasAlbedo   = 0x01,
  kHasNormal   = 0x02,
  kHasMR       = 0x04,  //!< metallic-roughness texture
  kHasEmissive = 0x08,
  kHasAO       = 0x10,
};

inline PbrMaterialFlags operator|(PbrMaterialFlags a, PbrMaterialFlags b) {
  return static_cast<PbrMaterialFlags>(static_cast<std::uint32_t>(a) |
                                       static_cast<std::uint32_t>(b));
}
inline bool operator&(PbrMaterialFlags a, PbrMaterialFlags b) {
  return (static_cast<std::uint32_t>(a) &
          static_cast<std::uint32_t>(b)) != 0u;
}

//! GPU-side PBR material parameters plus the bindless texture indices of its
//! optional maps.  One 64-byte SSBO slot per material; the byte layout exactly
//! matches the fragment shader's PbrMaterial struct (std430):
//!   base_color_factor (16) + emissive_factor (12) + metallic (4) +
//!   roughness (4) + ao_strength (4) + flags (4) + five texture indices (20)
//!   = 64 bytes, so array stride == sizeof(PbrMaterialData).
//! A texture index of 0 means "no map": element 0 of the bindless set-1
//! array is the opaque-white fallback (albedo/MR multiply by 1, normal and
//! AO maps are skipped via their flag bits).
struct PbrMaterialData {
  std::array<float, 4> base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 3> emissive_factor{0.0f, 0.0f, 0.0f};
  float metallic_factor{1.0f};
  float roughness_factor{1.0f};
  float ao_strength{0.0f};        //!< 0 = no AO map, 1 = full
  std::uint32_t flags{0};         //!< PbrMaterialFlags bitmask
  std::uint32_t albedo_index{0};        //!< bindless set-1 element (0 = white)
  std::uint32_t normal_index{0};        //!< 0 = geometric normal (no map)
  std::uint32_t metallic_roughness_index{0};  //!< 0 = factors only
  std::uint32_t emissive_index{0};      //!< 0 = factors only
  std::uint32_t ao_index{0};            //!< 0 = no AO map
};

static_assert(sizeof(PbrMaterialData) == 64,
              "PbrMaterialData must be exactly one 64-byte SSBO slot");

struct SceneMaterialPbr {
  SceneMaterial base{};            //!< legacy base_color + albedo handle
  PbrMaterialData pbr{};           //!< PBR parameters
  TextureHandle normal{};         //!< tangent-space normal map
  TextureHandle metallic_roughness{}; //!< metallic (B) / roughness (G)
  TextureHandle emissive{};       //!< emissive RGB texture
  TextureHandle ao{};             //!< ambient occlusion (R channel)
};

struct SceneObject {
  // Legacy/external-resource path. Kept for source compatibility.
  const SceneMesh* mesh{nullptr};
  SceneMatrix model{scene_identity_matrix()};
  // Snapshot-owned copies used by handle-backed extraction.
  SceneMesh mesh_value{};
  MeshHandle mesh_handle{};
  SceneMaterial material_value{};
  MaterialHandle material_handle{};
  SceneTexture albedo_value{};
  bool has_material{false};
  bool has_albedo{false};

  [[nodiscard]] const SceneMesh* effective_mesh() const noexcept {
    return mesh != nullptr ? mesh : &mesh_value;
  }
  [[nodiscard]] const SceneMaterial* effective_material() const noexcept {
    return has_material ? &material_value : nullptr;
  }
  //! Albedo texture resolved from the material's handle during extraction.
  //! Returns nullptr when the material has no texture.
  [[nodiscard]] const SceneTexture* effective_albedo() const noexcept {
    return has_albedo ? &albedo_value : nullptr;
  }
};

struct SceneCameraComponent {
  SceneMatrix view_projection{scene_identity_matrix()};
  std::uint32_t priority{0};
  bool active{true};
};

//! Local/model-space bounds used by deterministic frustum culling during
//! extraction. When invalid, the object is never culled.
struct SceneBounds {
  std::array<float, 3> min{0.0f, 0.0f, 0.0f};
  std::array<float, 3> max{0.0f, 0.0f, 0.0f};
  bool valid{false};
};

struct SceneRenderableComponent {
  const SceneMesh* mesh{nullptr};
  bool visible{true};
  MeshHandle mesh_handle{};
  MaterialHandle material_handle{};
  const SceneMaterial* material{nullptr};
  //! Trailing optional field: existing aggregate initializers keep working.
  SceneBounds bounds{};
};

//! Counters reported by extract_vulkan_scene for culling verification.
struct SceneExtractionStats {
  std::size_t visible_objects{0};  //!< after visibility filtering, before culling
  std::size_t culled_objects{0};   //!< dropped because fully outside the frustum
  bool camera_present{false};      //!< false means no culling was attempted
};

struct SceneTransformComponent {
  SceneMatrix model{scene_identity_matrix()};
};

/**
 * @brief Generation-checked table for externally owned mesh/material records.
 *
 * `destroy(handle, retire_value)` removes the handle immediately from
 * resolution but delays slot reuse until `collect(completed_value)`. This is
 * the CPU-side lifetime boundary required by frames-in-flight and timeline
 * submissions. It does not destroy Vulkan objects; the owner of the buffers
 * and descriptor sets performs destruction after the same retirement point.
 */
class VulkanSceneResourceRegistry final {
public:
  VulkanSceneResourceRegistry() = default;
  VulkanSceneResourceRegistry(const VulkanSceneResourceRegistry&) = delete;
  VulkanSceneResourceRegistry& operator=(const VulkanSceneResourceRegistry&) = delete;

  [[nodiscard]] MeshHandle create_mesh(SceneMesh mesh);
  [[nodiscard]] MaterialHandle create_material(SceneMaterial material);
  [[nodiscard]] TextureHandle create_texture(SceneTexture texture);
  [[nodiscard]] const SceneMesh* resolve(MeshHandle handle) const noexcept;
  [[nodiscard]] const SceneMaterial* resolve(MaterialHandle handle) const noexcept;
  [[nodiscard]] const SceneTexture* resolve(TextureHandle handle) const noexcept;
  [[nodiscard]] bool destroy(MeshHandle handle, std::uint64_t retire_value) noexcept;
  [[nodiscard]] bool destroy(MaterialHandle handle, std::uint64_t retire_value) noexcept;
  [[nodiscard]] bool destroy(TextureHandle handle, std::uint64_t retire_value) noexcept;
  //! Recycle all retired slots whose GPU retirement value has completed.
  void collect(std::uint64_t completed_value) noexcept;
  [[nodiscard]] std::size_t live_mesh_count() const noexcept;
  [[nodiscard]] std::size_t live_material_count() const noexcept;
  [[nodiscard]] std::size_t live_texture_count() const noexcept;

private:
  template <typename T>
  struct Entry {
    T value{};
    std::uint32_t generation{1};
    bool live{false};
  };
  struct RetiredSlot {
    std::uint32_t index{0};
    std::uint32_t generation{0};
    std::uint64_t retire_value{0};
  };

  std::vector<Entry<SceneMesh>> meshes_;
  std::vector<Entry<SceneMaterial>> materials_;
  std::vector<Entry<SceneTexture>> textures_;
  std::vector<std::uint32_t> free_meshes_;
  std::vector<std::uint32_t> free_materials_;
  std::vector<std::uint32_t> free_textures_;
  std::vector<RetiredSlot> retired_meshes_;
  std::vector<RetiredSlot> retired_materials_;
  std::vector<RetiredSlot> retired_textures_;
};

//! One drawable in a PBR scene snapshot: mesh + model + the SSBO slot of the
//! material that shades it. GPU records stay externally owned; recording never
//! retains pointers beyond the call.
struct ScenePbrObject {
  //! Legacy/external-resource path. Kept for source compatibility.
  const SceneMesh* mesh{nullptr};
  //! Snapshot-owned copy used by handle-backed extraction.
  SceneMesh mesh_value{};
  SceneMatrix model{scene_identity_matrix()};
  //! Index into the material SSBO bound at set 2 (record_pbr_scene).
  std::uint32_t material_index{0xffffffffU};

  [[nodiscard]] const SceneMesh* effective_mesh() const noexcept {
    return mesh != nullptr ? mesh : &mesh_value;
  }
};

struct VulkanScene {
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  SceneCamera camera{};
  //! Set when the pipeline layout exposes the extended material ABI
  //! (160-byte push range: view-projection, model, base color, albedo index).
  bool material_push_constants{false};
  //! Bindless albedo-texture array (set 1) bound before the object loop by
  //! record_scene when non-null. Element 0 must be an opaque-white 1x1
  //! texture; the material path samples albedos[albedo_index] per object.
  VkDescriptorSet texture_set{VK_NULL_HANDLE};
  std::vector<SceneObject> objects;
};

//! Immutable snapshot for the PBR (metallic-roughness) scene path recorded by
//! record_pbr_scene. Set 0 = per-mesh vertex storage, set 1 = bindless
//! sampler array (element 0 opaque-white fallback), set 2 = material SSBO
//! (one 64-byte PbrMaterialData slot per material). The push block is the
//! 160-byte PBR ABI: view-projection (64) + model (64) + camera position
//! (16) + material index (4) + padding (12).
struct VulkanPbrScene {
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  SceneCamera camera{};
  //! World-space eye position for the view vector (specular).
  std::array<float, 4> camera_position{0.0f, 0.0f, 0.0f, 1.0f};
  VkDescriptorSet texture_set{VK_NULL_HANDLE};   //!< set 1, bindless samplers
  VkDescriptorSet material_set{VK_NULL_HANDLE};  //!< set 2, material SSBO
  //! Set 3, IBL environment resources (prefiltered cube, irradiance cube,
  //! BRDF LUT) when the scene's pipeline is the 4-set IBL variant built from
  //! pbr_ibl.frag. record_pbr_scene binds it when non-null; pipelines whose
  //! fragment stage never samples set 3 may leave it null.
  VkDescriptorSet ibl_set{VK_NULL_HANDLE};
  std::vector<ScenePbrObject> objects;
};

//! Extract from const ECS state using legacy externally owned mesh pointers.
//! When the active camera provides a view-projection matrix, renderables with
//! valid bounds are culled when fully outside the frustum.
[[nodiscard]] VulkanScene extract_vulkan_scene(
    const omnicpp::core::World& world,
    VkPipeline pipeline = VK_NULL_HANDLE,
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE,
    SceneExtractionStats* stats = nullptr);

//! Convenience overload carrying no Vulkan handles (usable in headless and
//! deterministic contexts): extraction with culling stats only.
[[nodiscard]] VulkanScene extract_vulkan_scene(
    const omnicpp::core::World& world, SceneExtractionStats* stats);

//! Extract using generation-checked mesh/material handles. Resolved GPU records
//! are copied into the snapshot, so recording does not retain registry pointers.
[[nodiscard]] VulkanScene extract_vulkan_scene(
    const omnicpp::core::World& world,
    const VulkanSceneResourceRegistry& resources,
    VkPipeline pipeline = VK_NULL_HANDLE,
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE,
    SceneExtractionStats* stats = nullptr);

} // namespace omnicpp::render
