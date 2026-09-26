//! @file main.cpp
//! @brief OmniCpp viewport: a real windowed application rendering a lit PBR
//!        scene through the engine's swapchain path.
//!
//! Composition-only code: every subsystem (XCB window, platform surface,
//! swapchain, render pass, renderer frame loop, PBR scene recording, orbit
//! camera math) is an existing engine facility — this app wires them into a
//! running loop. The scene (rotating cubes over a ground slab) is recorded
//! per frame through VulkanRenderer::set_scene_record_callback, replacing
//! the built-in demo triangle.
//!
//! Controls: ESC closes; the camera slowly orbits automatically.

#include <chrono>
#include <cmath>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include "engine/core/control_server.hpp"
#include "engine/core/editor_session.hpp"
#include "engine/editor/graph_anim_bridge.hpp"
#include "engine/editor/inspector.hpp"
#include "engine/core/document.hpp"
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <mutex>
#include <vector>

#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#include "engine/asset/gltf_animation.hpp"
#include "engine/asset/gltf_importer.hpp"
#include "engine/core/input_state.hpp"
#include "engine/core/input_translators.hpp"
#include "engine/core/animation_state_machine.hpp"
#include "engine/core/physics_world.hpp"
#include "engine/editor/node_editor.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_frame_upload.hpp"
#include "engine/render/vulkan_acceleration_structure.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_ibl_baker.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_render_pass.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_scene.hpp"
#include "engine/render/vulkan_mesh_table.hpp"
#include "engine/render/scene_camera.hpp"
#include "engine/render/vulkan_surface.hpp"
#include "engine/render/vulkan_swapchain.hpp"
#include "engine/render/vulkan_ui_renderer.hpp"
#include "telemetry.hpp"

using SceneMatrix = omnicpp::render::SceneMatrix;

namespace {

//! Set on SIGTERM/SIGINT so the run loop exits through the normal shutdown
//! path (renderer wait-idle, control-server stop + socket unlink, telemetry
//! flush) instead of dying at the default termination handler and leaving
//! the control socket file stale. Async-signal-safe: only an atomic store.
std::atomic<bool> g_shutdown_requested{false};
void handle_shutdown_signal(int) { g_shutdown_requested.store(true); }

constexpr std::uint32_t kWidth = 1280U;
constexpr std::uint32_t kHeight = 720U;

// ============================================================================
// Flat-shaded unit cube in the engine's canonical eleven-float vertex layout.
// ============================================================================

void build_unit_cube(std::vector<float>& vertices,
                     std::vector<std::uint32_t>& indices) {
  struct Face {
    float normal[3];
    float corners[4][3];
  };
  const Face faces[6] = {
      {{0, 0, 1}, {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}},
      {{0, 0, -1}, {{-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1}}},
      {{1, 0, 0}, {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}}},
      {{-1, 0, 0}, {{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}}},
      {{0, 1, 0}, {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}},
      {{0, -1, 0}, {{-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}}},
  };
  const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  const std::uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
  for (const Face& face : faces) {
    const std::uint32_t base =
        static_cast<std::uint32_t>(vertices.size() / 11U);
    for (int i = 0; i < 4; ++i) {
      const float* p = face.corners[i];
      vertices.insert(vertices.end(), {p[0], p[1], p[2], 1.0f, 1.0f, 1.0f,
                                       face.normal[0], face.normal[1],
                                       face.normal[2], uvs[i][0], uvs[i][1]});
    }
    for (std::uint32_t index : quad) indices.push_back(base + index);
  }
}

SceneMatrix scale_matrix(float x, float y, float z) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  m[0] = x;
  m[5] = y;
  m[10] = z;
  return m;
}

SceneMatrix translation_matrix(float x, float y, float z) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  m[12] = x;
  m[13] = y;
  m[14] = z;
  return m;
}

SceneMatrix rotation_y_matrix(float radians) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  m[0] = c;
  m[2] = -s;
  m[8] = s;
  m[10] = c;
  return m;
}

//! Property-registry type id for document-authoritative cubes (resolved once
//! at startup; the mirror skips objects whose type_id does not match).
std::uint32_t kDocumentCubeTypeId{0xFFFFFFFFU};

//! Column-major product a * b.
SceneMatrix multiply(const SceneMatrix& a, const SceneMatrix& b) {
  SceneMatrix out{};
  for (std::size_t col = 0; col < 4; ++col) {
    for (std::size_t row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (std::size_t k = 0; k < 4; ++k) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      out[col * 4 + row] = sum;
    }
  }
  return out;
}

//! Orthographic projection matching the proven shadow test's convention
//! (row-diagonal m[0]/m[5], GL z-remap m[10] = 1/(zn-zf), m[14] = zn/(zn-zf)).
SceneMatrix make_ortho(float l, float r, float b, float t, float zn,
                       float zf) {
  SceneMatrix m = omnicpp::render::scene_identity_matrix();
  m[0] = 2.0f / (r - l);
  m[5] = 2.0f / (t - b);
  m[10] = 1.0f / (zn - zf);
  m[12] = -(r + l) / (r - l);
  m[13] = -(t + b) / (t - b);
  m[14] = zn / (zn - zf);
  return m;
}

// ============================================================================
// Viewport state
// ============================================================================

//! GPU-driven scene: ground slab + two cubes (payload/cull/dispatch count).
constexpr std::uint32_t kGdObjectCount = 3U;
//! Maximum GPU-driven instances (physics-scene cap; payload sized once).
constexpr std::uint32_t kGdMaxInstances = 4096U;

struct ViewportApp;
//! ControlHost over the viewport (defined after ViewportApp).
class ViewportControlHost;

struct ViewportApp {
  // Vulkan stack.
  omnicpp::render::VulkanContext context;
  omnicpp::render::VulkanSwapchain swapchain;
  omnicpp::render::VulkanRenderPass render_pass;
  omnicpp::render::VulkanRenderer renderer;
  omnicpp::render::VulkanMemoryAllocator allocator;
  omnicpp::render::VulkanDescriptorManager descriptors;

  // Scene resources.
  omnicpp::render::VulkanPipeline pbr_pipeline;
  omnicpp::render::VulkanPipeline skinned_pipeline;
  //! Composed full-lighting pipelines (pbr_full.frag: IBL set 5 + shadow
  //! set 4 + optional bones set 3). Built in setup_lighting; when ready,
  //! they replace the basic pipelines in the recorded scene.
  omnicpp::render::VulkanPipeline full_pipeline;
  omnicpp::render::VulkanPipeline full_skinned_pipeline;
  //! True when the composed lighting stack initialized (graceful fallback:
  //! false keeps the basic pbr_scene path and no shadow map).
  bool lighting_ready{false};
  //! Forced legacy path (OMNICPP_LEGACY_LIGHTING=1) for A/B proofs.
  bool legacy_lighting{false};
  //! Diagnostic toggle: composed shading with the shadow term removed
  //! (OMNICPP_NO_SHADOW=1). The pixel diff vs the composed run is then the
  //! exact shadow footprint.
  bool no_shadow{false};
  //! Diagnostic: dump the raw shadow map (kShadowRes^2 float32) after a
  //! frame (OMNICPP_DUMP_SHADOW=<frame>). Makes the depth-only pre-pass
  //! directly observable instead of inferred.
  bool dump_shadow{false};
  std::uint32_t dump_shadow_frame{0};
  VkBuffer dump_shadow_buffer{VK_NULL_HANDLE};
  omnicpp::render::Allocation dump_shadow_allocation{};
  //! GPU-driven draw path (OMNICPP_GPU_DRIVEN=1, cubes scene only): the
  //! compute cull/LOD pass writes indirect draw commands and the main pass
  //! draws the whole scene with ONE vkCmdDrawIndexedIndirect — no CPU
  //! visibility, LOD, or per-draw submission inside the frame.
  //! Physics-driven scene state (OMNICPP_PHYSICS=1): bodies stepped on the
  //! CPU each frame; instanceCount includes them + the ground slab.
  omnicpp::physics::PhysicsWorld physics_world;
  std::vector<omnicpp::physics::PhysicsBody> physics_bodies;
  std::uint32_t gd_instance_count{kGdObjectCount};

  bool gpu_driven{false};
  //! 11-float cube geometry through the mesh table (shared vertex/index
  //! buffers + per-mesh table slots); one slot per mesh (no LOD chain yet,
  //! lod_count = 1).
  omnicpp::render::SceneMeshTableBuilder gd_table_builder;
  omnicpp::render::MeshTableBuild gd_table;
  std::uint32_t gd_slot_cube{0};
  std::uint32_t gd_slot_ground{0};
  VkBuffer gd_shared_vertex_buffer{VK_NULL_HANDLE};
  omnicpp::render::Allocation gd_shared_vertex_allocation{};
  VkBuffer gd_shared_index_buffer{VK_NULL_HANDLE};
  omnicpp::render::Allocation gd_shared_index_allocation{};
  //! Per-frame payload (2 header words + 3 objects x 24 words), one copy
  //! per swapchain image so CPU writes never race in-flight GPU reads.
  std::vector<VkBuffer> gd_payload_buffers;
  std::vector<omnicpp::render::Allocation> gd_payload_allocations;
  VkBuffer gd_indirect_buffer{VK_NULL_HANDLE};
  omnicpp::render::Allocation gd_indirect_allocation{};
  VkBuffer gd_table_buffer{VK_NULL_HANDLE};
  omnicpp::render::Allocation gd_table_allocation{};
  //! Set 0 for the cull pass and the draw pass (vertices + payload +
  //! mesh table + draw commands; per-image payload copy -> per-image set).
  VkDescriptorSetLayout gd_set0_layout{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> gd_cull_sets;
  std::vector<VkDescriptorSet> gd_draw_sets;
  omnicpp::render::VulkanPipeline gd_cull_pipeline;
  omnicpp::render::VulkanPipeline gd_draw_pipeline;
  // --- Node editor overlay (OMNICPP_NODE_EDITOR=1) --------------------------
  // UI paint path over the scene: graph cards, pins, bezier wires, value
  // readouts; mouse selects/drags node cards. UI failures fail the frame.
  bool node_editor{false};
  std::unique_ptr<omnicpp::editor::NodeEditorView> node_view;
  omnicpp::editor::InspectorPanel inspector;
  //! M13 fusion: graph outputs -> animation actions (OMNICPP_GRAPH_ANIM=1).
  bool graph_anim_enabled{false};
  std::unique_ptr<omnicpp::editor::GraphSignalAdapter> graph_anim;
  std::uint32_t inspector_canvas{warploom::ui::kInvalidWidget};
  std::uint32_t inspector_root{warploom::ui::kInvalidWidget};
  warploom::ui::WidgetTree ui_tree;
  warploom::ui::PaintList ui_paint;
  std::uint32_t node_canvas{warploom::ui::kInvalidWidget};
  omnicpp::render::VulkanUiRenderer ui_renderer;
  //! Toolbar buttons (per registered type, then undo/redo) + their handles.
  std::vector<std::uint32_t> node_toolbar_buttons{};
  std::uint32_t node_toolbar{warploom::ui::kInvalidWidget};
  // Mouse tracking (view-space pixels) for node select/drag.
  float mouse_x{0.0f};
  float mouse_y{0.0f};
  float last_mouse_x{0.0f};
  float last_mouse_y{0.0f};
  std::uint64_t drag_node_id{0};
  float drag_grab_dx{0.0f};
  float drag_grab_dy{0.0f};
  omnicpp::editor::PinRef selected_pin{};
  //! Position a card had when a drag started (undo commit on release).
  float drag_start_x{0.0f};
  float drag_start_y{0.0f};
  //! Editor interaction flags (processed on the frame thread, next tick).
  bool node_dirty{false};      //!< graph mutated: rebuild the view
  bool undo_requested{false};  //!< toolbar undo button
  bool redo_requested{false};  //!< toolbar redo button
  bool inspector_rebuild_requested{false};  //!< selection changed
  //! Event thread -> frame-thread command queue (M10): mouse/keyboard edits
  //! are queued as protocol commands and drained ON the frame thread through
  //! editor.on_control, so every mutation crosses the single session stack.
  struct QueuedEdit {
    omnicpp::core::ControlCommand command;
  };
  std::vector<QueuedEdit> edit_queue;
  std::mutex edit_queue_mutex;
  //! Optional document loaded at startup via OMNICPP_DOC=<path>.
  std::string doc_path{};
  //! The draw pipeline's full layout (sets 0..2, 160-byte push) so the
  //! pre-pass hook can bind the compute set under the draw layout when
  //! chaining the cull dispatch ahead of the indirect draw.
  VkPipelineLayout gd_draw_pipeline_layout{VK_NULL_HANDLE};
  //! Staging for the 144-byte cull push (rebuilt every frame).
  std::array<std::byte, 144> gd_cull_push_staging{};
  VkDescriptorSetLayout mesh_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout textures_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout material_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout bone_layout{VK_NULL_HANDLE};
  VkDescriptorSet textures_set{VK_NULL_HANDLE};
  VkDescriptorSet material_set{VK_NULL_HANDLE};
  VkDescriptorSet bone_set{VK_NULL_HANDLE};
  omnicpp::render::Allocation material_allocation{};
  omnicpp::render::Allocation bone_allocation{};

  struct MeshBuffers {
    omnicpp::render::Allocation vertex_allocation{};
    omnicpp::render::Allocation index_allocation{};
    omnicpp::render::SceneMesh mesh{};
  };
  MeshBuffers cube{};
  MeshBuffers ground{};
  std::vector<MeshBuffers> mannequin_meshes;

  // --- City scene (OMNICPP_SCENE=city) --------------------------------------
  // Static meshes (buildings, streetlights, each Sponza material-split slice)
  // plus their material slots; the record path swaps the whole object list
  // and camera framing for the city.
  struct CityPart {
    MeshBuffers buffers{};
    std::uint32_t material_index{0U};
    omnicpp::render::SceneMatrix model{omnicpp::render::scene_identity_matrix()};
    //! World-space triangles (x,y,z per vertex) for exact-geometry BLASes.
    std::vector<float> rt_triangles;
  };
  std::vector<CityPart> city_parts;
  struct CityLight {
    float pos[3];
    float radius;
    float color[3];
    float intensity;
  };
  std::vector<CityLight> city_lights;

  // --- Sponza landmark (OMNICPP_SPONZA=1, city scene) ----------------------
  // One shared vertex/index pair; one draw per glTF primitive (SceneMesh
  // index_offset/count slices sharing a single mesh SSBO descriptor set).
  struct SponzaLandmark {
    omnicpp::render::Allocation vertex_allocation{};
    omnicpp::render::Allocation index_allocation{};
    VkDescriptorSet mesh_set{VK_NULL_HANDLE};
    std::vector<omnicpp::render::SceneMesh> parts;
    std::vector<SceneMatrix> part_models;
    std::uint32_t material_base{0U};
  };
  SponzaLandmark sponza;
  bool sponza_enabled{false};
  struct SponzaTexture {
    VkImage image{VK_NULL_HANDLE};
    omnicpp::render::Allocation allocation{};
    omnicpp::render::SceneTexture scene{};
  };
  std::vector<SponzaTexture> sponza_textures;
  VkSampler sponza_sampler{VK_NULL_HANDLE};
  VkDescriptorSetLayout sponza_sampler_layout{VK_NULL_HANDLE};
  VkDescriptorSet sponza_sampler_set{VK_NULL_HANDLE};
  //! World-space triangles for the merged landmark BLAS.
  std::vector<float> rt_sponza_triangles;
  //! World-space triangle count of the merged landmark BLAS (telemetry).
  std::uint32_t sponza_triangle_count{0U};
  VkBuffer city_lights_buffer{VK_NULL_HANDLE};
  omnicpp::render::Allocation city_lights_allocation{};
  VkDescriptorSetLayout city_lights_layout{VK_NULL_HANDLE};
  VkDescriptorSet city_lights_set{VK_NULL_HANDLE};
  // Walking actors: per-actor world placement + walk-clock phase offset.
  struct CityActor {
    omnicpp::render::SceneMatrix model{omnicpp::render::scene_identity_matrix()};
    float walk_phase{0.0f};
  };
  std::vector<CityActor> city_actors;
  // Many-light composed pipelines (pbr_full_ml / pbr_rt_full_ml fragments;
  // lights SSBO at set 6). Built in setup_lighting when city_scene is on.
  omnicpp::render::VulkanPipeline full_ml_pipeline;
  omnicpp::render::VulkanPipeline full_ml_skinned_pipeline;
  omnicpp::render::VulkanPipeline rt_full_ml_pipeline;
  omnicpp::render::VulkanPipeline rt_full_ml_skinned_pipeline;
  // Skinning arena: slot 0 = identity bone for static meshes, actors at
  // [1 + a * joints_per_actor, ...). One bone SSBO + one bone set serves
  // every actor (per-object joint_base push).
  std::uint32_t joints_per_actor{0U};
  std::uint32_t city_actor_count{0U};

  // Skeletal mannequin (assets/models/mannequin.gltf); empty when the asset
  // is unavailable and the scene renders cubes only.
  omnicpp::asset::GltfAnimationDocument mannequin;
  bool has_mannequin{false};

  // --- Full lighting (C1): composed PBR pipeline + IBL + shadow map. ------
  //! Set-5 IBL layout for the composed pbr_full variant (the baker's set
  //! views/samplers are written into a viewport-owned set at slot 5).
  VkDescriptorSetLayout ibl5_layout{VK_NULL_HANDLE};
  VkDescriptorSet ibl5_set{VK_NULL_HANDLE};
  //! Engine-side GPU bake: analytic sky -> prefiltered/irradiance/LUT.
  omnicpp::render::VulkanIblBaker ibl_baker;
  //! Shadow resources: D32 image (depth+sampled usage), depth/sample views,
  //! compare sampler, 64-byte light-VP UBO + set-4 descriptor, depth-only
  //! render pass + framebuffer, static and skinned depth-only pipelines.
  static constexpr std::uint32_t kShadowRes = 2048U;
  VkImage shadow_image{VK_NULL_HANDLE};
  omnicpp::render::Allocation shadow_memory{};
  VkImageView shadow_depth_view{VK_NULL_HANDLE};
  VkImageView shadow_sample_view{VK_NULL_HANDLE};
  VkSampler shadow_sampler{VK_NULL_HANDLE};
  //! Neutral 1x1 depth image for OMNICPP_NO_SHADOW: the composed fragment
  //! stage statically uses set 4, so it must stay bound (UB otherwise) — a
  //! cleared-to-1.0 map makes every PCF comparison pass (fully lit).
  VkImage neutral_shadow_image{VK_NULL_HANDLE};
  omnicpp::render::Allocation neutral_shadow_memory{};
  VkImageView neutral_shadow_sample_view{VK_NULL_HANDLE};
  VkDescriptorSet neutral_shadow_set{VK_NULL_HANDLE};
  VkRenderPass shadow_render_pass{VK_NULL_HANDLE};
  VkFramebuffer shadow_framebuffer{VK_NULL_HANDLE};
  VkDescriptorSetLayout shadow_layout{VK_NULL_HANDLE};
  VkDescriptorSet shadow_set{VK_NULL_HANDLE};
  omnicpp::render::Allocation shadow_ubo_allocation{};
  omnicpp::render::VulkanPipeline shadow_pipeline_static;
  omnicpp::render::VulkanPipeline shadow_pipeline_skinned;
  //! Sun direction (normalized, toward the sun) shared by the shadow VP,
  //! the IBL bake, and telemetry.
  std::array<float, 3> sun_direction{0.45f, 0.7f, 0.55f};

  // --- RT mode (OMNICPP_RT_MODE=1): hard ray-query shadows. --------------
  //! The scene TLAS replaces the shadow map entirely: the composed fragment
  //! stage traces one occlusion ray per pixel (pbr_rt_full.frag, set 4),
  //! the shadow pre-pass and depth map are skipped, and BLASes are built
  //! once at startup from bind-pose geometry (TLAS instance transforms are
  //! the per-frame object models, so rigid objects animate exactly; the
  //! skinned mannequin approximates its walk at instance granularity).
  bool city_scene{false};
  bool rt_mode{false};
  omnicpp::render::VulkanAccelerationStructureBuilder rt_builder;
  omnicpp::render::VulkanScratchPool rt_scratch;
  struct RtBlas {
    omnicpp::render::BottomLevelAS as{};
    //! Geometry feeding the build (must stay alive for rebuilds).
    omnicpp::render::Allocation geometry{};
    omnicpp::render::BlasBuildInput input{};
  };
  //! One BLAS per scene mesh (cube, ground, each mannequin part), ordered
  //! like the scene-draw objects they correspond to.
  std::vector<RtBlas> rt_blas_cube;
  std::vector<RtBlas> rt_blas_ground;
  std::vector<RtBlas> rt_blas_mannequin;
  //! Sponza landmark: one merged world-space BLAS, identity instance.
  std::vector<RtBlas> rt_blas_sponza;
  //! City statics: one BLAS per CityPart (exact geometry, TLAS instance
  //! applies the part model). Indexed like city_parts.
  std::vector<RtBlas> rt_blas_city;
  //! E5 animated TLAS: per-part dominant joint index (single-joint
  //! binding). Authored skinned vertices are already in world bind pose
  //! (rest pose == bind pose was proven in the skinning E2E), and the
  //! bone matrix B_j(t) = global_j(t) * IB_j maps them straight to the
  //! posed world position — so BLASes stay STATIC and only the TLAS
  //! instance transform (object_model * bones[j]) animates per frame.
  std::vector<std::size_t> rt_part_joint;
  omnicpp::render::TopLevelAS rt_tlas{};
  //! Geometry scratch needed by the largest BLAS (scratch-pool sizing).
  std::uint64_t rt_blas_scratch_bytes{0};
  std::uint64_t rt_tlas_scratch_bytes{0};
  //! Per-frame TLAS instance list (rebuilt each frame from object models).
  std::vector<omnicpp::render::TlasInstance> rt_instances;
  //! Instance count of the last build_rt_frame_tlas (telemetry/verification).
  std::uint32_t rt_last_instance_count{0};
  VkDescriptorSetLayout rt_layout{VK_NULL_HANDLE};
  VkDescriptorSet rt_set{VK_NULL_HANDLE};
  //! Composed pipelines with the pbr_rt_full fragment stage (same 6-set
  //! layout shape as the PCF family; set 4 layout is the TLAS).
  omnicpp::render::VulkanPipeline rt_full_pipeline;
  omnicpp::render::VulkanPipeline rt_full_skinned_pipeline;

  std::vector<omnicpp::render::ScenePbrObject> objects;
  omnicpp::render::VulkanPbrScene scene;

  // XCB window.
  xcb_connection_t* connection{nullptr};
  xcb_window_t window{0};
  xcb_atom_t wm_protocols{0};
  xcb_atom_t wm_delete_window{0};
  VkSurfaceKHR surface{VK_NULL_HANDLE};

  // Animation state.
  float time{0.0f};       //!< sim clock, advanced by run_config.fixed_dt
  float walk_time{0.0f};  //!< wraps at the 1 s walk-cycle duration

  // Observability: env-configured telemetry + GPU-side frame capture.
  viewport::RunConfig run_config{};
  viewport::TelemetryLogger telemetry;
  viewport::FrameCapture capture;
  bool telemetry_enabled{false};

  // ---- M0 control channel (OMNICPP_CONTROL_SOCKET=path). -----------------
  // Editor/automation surface: pause/step/camera/sun/cube/capture commands
  // over a unix-socket JSONL server polled once per frame. Never blocks.
  std::unique_ptr<ViewportControlHost> control_host;
  std::unique_ptr<omnicpp::core::ControlServer> control_server;

  bool control_paused() const noexcept { return control_paused_; }
  bool control_paused_{false};
  std::uint32_t control_steps_requested_{0};
  bool control_capture_requested_{false};
  //! Editor selection mirrored from the `select` control command (0 = none).
  std::uint64_t selected_object_id{0};
  bool camera_override_{false};
  std::array<float, 3> camera_eye_{16.0f, 14.0f, 0.0f};
  std::array<float, 3> camera_target_{0.0f, 1.0f, 0.0f};
  std::uint32_t frame_index{0};
  std::uint32_t captures_done{0};
  //! Scene times actually recorded by the last window frame (the capture
  //! re-records exactly these so the captured image matches what was shown).
  float last_recorded_time{0.0f};
  float last_recorded_walk{0.0f};
  //! Wall time of the last scene recording (scene-callback scope only).
  double last_record_us{0.0};
  //! Idle-clip weight used by the last recorded pose (telemetry).
  float last_idle_weight{0.0f};
  //! Largest joint swing of the last recorded pose (telemetry pose line).
  std::string last_swing_joint;
  float last_swing_deg{0.0f};

  // Input: virtual driver replays OMNICPP_INPUT_SCRIPT (JSONL by tick);
  // the camera and cross-fade consume actions/axes so scripted runs and
  // human play are indistinguishable downstream.
  omnicpp::core::InputState input;
  omnicpp::core::VirtualInputDriver virtual_input;
  bool input_scripted{false};
  //! Real-device translation: the XCB event loop feeds raw key/mouse
  //! events in; apply() writes the same actions/axes the virtual driver
  //! produces. The gamepad driver is optional (absent device = no-op).
  omnicpp::core::XcbKeyMouseTranslator kb_mouse;
#if defined(__linux__)
  std::unique_ptr<omnicpp::core::LinuxJoystickDriver> gamepad;
#endif
  //! Camera response accumulators driven by input actions.
  float camera_orbit_bias{0.0f};   //!< extra rad/s from orbit_left/right
  float camera_radius_bias{0.0f};  //!< zoom accumulator
  //! Input-driven cross-fade: fade_toggle flips the target, current eases
  //! toward it each frame. Nonzero target/current puts the recorder in
  //! input-fade mode (overriding the periodic cross-fade demo).
  float fade_target{0.0f};
  float fade_current{0.0f};
  //! Deterministic animation state machine (D1). Owned pointer because the
  //! mannequin's clip set determines the configuration; null when no
  //! mannequin is loaded. Ticked once per window frame after input commit;
  //! the capture path re-records without ticking so both paths render the
  //! identical pose.
  std::unique_ptr<omnicpp::anim::AnimationStateMachine<omnicpp::core::InputSnapshot>>
      machine;
  std::string machine_last_state{"walk"};
  std::chrono::steady_clock::time_point frame_started{};
  double fps_smoothed{0.0};

  [[nodiscard]] bool initialize();
  void run();
  void shutdown();

  //! The app's authoritative editable document (owns the node graph the
  //! editor view projects; graph edits go through CommandStack semantics).
  //! M10: the embedded EditorSession IS the mutation authority — every edit
  //! path (protocol, mouse, keys) funnels through its CommandStack, so
  //! undo/redo/replay are one mechanism.
  [[nodiscard]] omnicpp::editor::SceneDocument& session_document() {
    return editor.document();
  }
  omnicpp::editor::EditorSession editor{};
  //! The session document's graph (borrowed view; the document owns it).
  omnicpp::editor::NodeGraph* node_graph{nullptr};
};

//! ControlHost implementation over the viewport: translates protocol
//! commands into the app's authoritative state (pause/step/camera/sun/cube)
//! and reports it via snapshot_json. Methods run on the frame thread inside
//! ControlServer::poll — non-blocking by construction.
class ViewportControlHost final : public omnicpp::core::ControlHost {
 public:
  explicit ViewportControlHost(ViewportApp& app) : app_(app) {}

  omnicpp::core::ControlReply on_control(
      const omnicpp::core::ControlCommand& command) override {
    using CK = omnicpp::core::ControlCommand::Kind;
    omnicpp::core::ControlReply reply;
    reply.ok = true;
    switch (command.kind) {
      case CK::Ping:
        reply.detail = "pong";
        break;
      case CK::Pause:
        app_.control_paused_ = true;
        reply.detail = "paused";
        break;
      case CK::Resume:
        app_.control_paused_ = false;
        reply.detail = "resumed";
        break;
      case CK::Step:
        app_.control_steps_requested_ +=
            command.number_count > 0
                ? static_cast<std::uint32_t>(command.numbers[0])
                : 1U;
        reply.detail = "stepped " +
                       std::to_string(command.number_count > 0
                                          ? static_cast<unsigned>(command.numbers[0])
                                          : 1U);
        break;
      case CK::SetCamera: {
        if (command.number_count >= 6U) {
          app_.camera_override_ = true;
          app_.camera_eye_ = {static_cast<float>(command.numbers[0]),
                              static_cast<float>(command.numbers[1]),
                              static_cast<float>(command.numbers[2])};
          app_.camera_target_ = {static_cast<float>(command.numbers[3]),
                                 static_cast<float>(command.numbers[4]),
                                 static_cast<float>(command.numbers[5])};
          reply.detail = "camera set";
        } else {
          reply.ok = false;
          reply.error = "set_camera needs ex,ey,ez,tx,ty,tz";
        }
        break;
      }
      case CK::SetSun: {
        if (command.number_count >= 3U) {
          app_.sun_direction = {
              static_cast<float>(command.numbers[0]),
              static_cast<float>(command.numbers[1]),
              static_cast<float>(command.numbers[2])};
          reply.detail = "sun set";
        } else {
          reply.ok = false;
          reply.error = "set_sun needs x,y,z";
        }
        break;
      }      case CK::SpawnCube: {
        // M11: spawn is DOCUMENT-authoritative. The session bridge creates
        // a registry-typed cube object (undoable, saveable, queryable); the
        // record path mirrors every document cube into the scene each frame
        // (mirror_document_objects). No render-side spawn list exists.
        {
          const bool structural = true;
          reply = app_.editor.on_control(command);
          if (reply.ok && structural) app_.node_dirty = true;
        }
        break;
      }
      case CK::Select: {
        // Mirror editor selection onto the render view (outline/highlight
        // hooks read this); selection is session state, not undoable.
        if (command.number_count >= 1U) {
          app_.selected_object_id =
              static_cast<std::uint64_t>(command.numbers[0]);
          reply.detail = "selection set";
        } else {
          reply.ok = false;
          reply.error = "select needs oid";
        }
        break;
      }
      case CK::Capture:
        app_.control_capture_requested_ = true;
        reply.detail = "capture scheduled";
        break;
      // M10: every document command delegates to the embedded EditorSession
      // — the single mutation authority. Protocol edits, mouse edits, and
      // key edits now share one CommandStack, so undo/redo cover ALL paths
      // and the document stays replayable no matter where the edit came
      // from. The view flags cover the resulting rebuild.
      case CK::ListObjects:
      case CK::GetObject:
      case CK::SetProperty:
      case CK::DestroyObject:
      case CK::Undo:
      case CK::Redo:
      case CK::Schema:
      case CK::NodeAdd:
      case CK::NodeRemove:
      case CK::LinkNodes:
      case CK::UnlinkNodes:
      case CK::SetNodeParam:
      case CK::SetNodePosition:
      case CK::GetGraph:
      case CK::SaveDocument:
      case CK::LoadDocument:
      case CK::BindNodeProperty:
      case CK::UnbindNodeProperty:
      case CK::ListBindings: {
        const bool structural = command.kind == CK::NodeAdd ||
                                command.kind == CK::NodeRemove ||
                                command.kind == CK::LinkNodes ||
                                command.kind == CK::UnlinkNodes;
        reply = app_.editor.on_control(command);
        if (reply.ok && structural) app_.node_dirty = true;
        break;
      }
      default:
        reply.ok = false;
        reply.error = "unhandled command";
        break;
    }
    return reply;
  }

  [[nodiscard]] std::string snapshot_json() const override {
    std::size_t drawn = 0;
    for (const auto& object : app_.scene.objects) {
      if (object.mesh != nullptr && object.mesh->is_drawable()) ++drawn;
    }
    std::string json = "{\"scene\":\"";
    json += app_.sponza_enabled ? "city+sponza" : (app_.city_scene ? "city" : "cubes");
    json += "\",\"paused\":";
    json += app_.control_paused_ ? "true" : "false";
    json += ",\"objects\":" + std::to_string(app_.scene.objects.size());
    json += ",\"drawn\":" + std::to_string(drawn);
    json += ",\"camera_override\":";
    json += app_.camera_override_ ? "true" : "false";
    json += ",\"nodes\":" +
            std::to_string(app_.editor.document().node_graph.node_count());
    json += ",\"links\":" +
            std::to_string(app_.editor.document().node_graph.link_count());
    json += ",\"undo_depth\":" +
            std::to_string(app_.editor.stack().undo_count());
    json += ",\"redo_depth\":" +
            std::to_string(app_.editor.stack().redo_count());
    json += "}";
    return json;
  }

 private:
  ViewportApp& app_;
};

//! M11: mirror every document-authoritative cube into the render scene.
//! The document (registry type "cube": position/rotation/scale/color) is the
//! single source of truth — the record path consumes it read-only each frame
//! after scene.objects.clear(), so undo/redo/save/load/queries apply to
//! exactly what renders. Rotation uses the document's Euler XYZ degrees
//! (yaw about Y first, matching the city's actor convention).
void mirror_document_objects(ViewportApp& app);

//! Free-function RT-shadow helpers (defined near setup_lighting).
bool setup_rt_shadows(ViewportApp& app);
void build_rt_frame_tlas(ViewportApp& app, VkCommandBuffer command_buffer);
void destroy_rt_shadows(ViewportApp& app);
//! Bind-pose unit cube (half-extent 1) in BLAS build-input layout: 12
//! triangles x 9 floats, canonical quad indices per face.
constexpr std::array<float, 8U * 3U> kRtCubePositions = {
    -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1,   // +z face
    -1, -1, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1,  // -z face
};
constexpr std::array<std::uint32_t, 36U> kRtCubeIndices = {
    0, 1, 2, 0, 2, 3,        // +z
    4, 5, 6, 4, 6, 7,        // -z
    1, 7, 6, 1, 6, 2,        // +x
    4, 0, 3, 4, 3, 5,        // -x
    3, 2, 6, 3, 6, 5,        // +y
    4, 7, 1, 4, 1, 0,        // -y
};

// ============================================================================
// XCB window + WM delete protocol
// ============================================================================

bool setup_window(ViewportApp& app) {
  const char* display_name = std::getenv("DISPLAY");
  app.connection = xcb_connect(display_name, nullptr);
  if (!app.connection || xcb_connection_has_error(app.connection)) {
    std::fprintf(stderr, "viewport: cannot connect to the X display\n");
    return false;
  }
  const auto* setup = xcb_get_setup(app.connection);
  const auto* screen = xcb_setup_roots_iterator(setup).data;

  app.window = xcb_generate_id(app.connection);
  const std::uint32_t event_mask = XCB_EVENT_MASK_KEY_PRESS |
                                   XCB_EVENT_MASK_KEY_RELEASE |
                                   XCB_EVENT_MASK_BUTTON_PRESS |
                                   XCB_EVENT_MASK_BUTTON_RELEASE |
                                   XCB_EVENT_MASK_POINTER_MOTION |
                                   XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_create_window(app.connection, XCB_COPY_FROM_PARENT, app.window,
                    screen->root, 0, 0, kWidth, kHeight, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    XCB_CW_EVENT_MASK, &event_mask);
  xcb_change_property(app.connection, XCB_PROP_MODE_REPLACE, app.window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                      sizeof("OmniCpp Viewport") - 1, "OmniCpp Viewport");
  xcb_map_window(app.connection, app.window);
  xcb_flush(app.connection);
  return true;
}

bool setup_wm_delete_protocol(ViewportApp& app) {
  xcb_intern_atom_cookie_t protocols_cookie = xcb_intern_atom(
      app.connection, 1, 12, "WM_PROTOCOLS");
  xcb_intern_atom_cookie_t delete_cookie = xcb_intern_atom(
      app.connection, 0, 16, "WM_DELETE_WINDOW");
  xcb_intern_atom_reply_t* protocols_reply =
      xcb_intern_atom_reply(app.connection, protocols_cookie, nullptr);
  xcb_intern_atom_reply_t* delete_reply =
      xcb_intern_atom_reply(app.connection, delete_cookie, nullptr);
  if (protocols_reply && delete_reply) {
    app.wm_protocols = protocols_reply->atom;
    app.wm_delete_window = delete_reply->atom;
    xcb_change_property(app.connection, XCB_PROP_MODE_REPLACE, app.window,
                        app.wm_protocols, XCB_ATOM_ATOM, 32, 1,
                        &app.wm_delete_window);
  }
  free(protocols_reply);
  free(delete_reply);
  return app.wm_protocols != 0 && app.wm_delete_window != 0;
}

//! Poll the event queue; returns false when the window should close.
bool poll_events(ViewportApp& app) {
  while (xcb_generic_event_t* event =
             xcb_poll_for_event(app.connection)) {
    const std::uint8_t type = event->response_type & 0x7f;
    if (type == XCB_CLIENT_MESSAGE) {
      const auto* msg =
          reinterpret_cast<const xcb_client_message_event_t*>(event);
      if (msg->data.data32[0] == app.wm_delete_window) {
        free(event);
        return false;
      }
    } else if (type == XCB_KEY_PRESS) {
      const auto* key =
          reinterpret_cast<const xcb_key_press_event_t*>(event);
      // M12: active param editor consumes keys first. Enter (36) commits,
      // ESC (9) cancels (without closing the window), printable + backspace
      // edit the buffer. XCB keycodes: digits 10..19 map to 1..9,0;
      // letters use the evdev+offset layout via the host lookup below.
      if (app.node_editor && app.node_view != nullptr &&
          app.node_view->param_edit_active()) {
        if (key->detail == 36) {  // Return: commit through the session
          std::uint64_t nid = 0;
          std::string param;
          double number = 0.0;
          std::string text;
          bool is_number = false;
          if (app.node_view->end_param_edit(
                  true, nullptr, nid, param, number, text, is_number)) {
            omnicpp::core::ControlCommand cmd;
            cmd.kind = omnicpp::core::ControlCommand::Kind::SetNodeParam;
            cmd.numbers[0] = static_cast<double>(nid);
            cmd.number_count = 1;
            cmd.text = param;
            if (is_number) {
              cmd.numbers[1] = number;
              cmd.number_count = 2;
            } else {
              cmd.text2 = text;
            }
            {
              std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
              app.edit_queue.push_back({std::move(cmd)});
            }
            app.node_dirty = true;
          }
          free(event);
          return true;
        }
        if (key->detail == 9) {  // ESC: cancel the edit, keep the window
          std::uint64_t nid = 0;
          std::string param;
          double number = 0.0;
          std::string text;
          bool is_number = false;
          (void)app.node_view->end_param_edit(
              false, nullptr, nid, param, number, text, is_number);
          free(event);
          return true;
        }
        if (key->detail == 22) {  // BackSpace
          app.node_view->edit_param_char('\b');
          free(event);
          return true;
        }
        static constexpr char kDigitRow[10] = {'1', '2', '3', '4', '5',
                                               '6', '7', '8', '9', '0'};
        if (key->detail >= 10 && key->detail <= 19) {
          app.node_view->edit_param_char(kDigitRow[key->detail - 10]);
          free(event);
          return true;
        }
        if (key->detail == 60) app.node_view->edit_param_char('.');
        if (key->detail == 61) app.node_view->edit_param_char('-');
        if ((key->detail >= 10 && key->detail <= 19) || key->detail == 60 ||
            key->detail == 61) {
          free(event);
          return true;
        }
        // Letters (evdev q..p=24..33, a..l=38..46, z..m=52..58) are accepted
        // for string params; numbers ignore them at parse time.
        static const char* kRows = "qqq";  // placeholder to keep structure
        (void)kRows;
        if (key->detail >= 24 && key->detail <= 58) {
          static const char kLower[] =
              "?qwertzuiop?asdfghjkl?yxcvbnm";  // index by keycode-24
          const char c = kLower[key->detail - 24];
          if (c != '?') {
            app.node_view->edit_param_char(c);
            free(event);
            return true;
          }
        }
        free(event);
        return true;  // swallow all other keys while editing
      }
      // ESC (keycode 9 on most servers).
      if (key->detail == 9) {
        free(event);
        return false;
      }
      // Delete (119) / BackSpace (22) with a selected node: remove it
      // through the session's undoable command.
      if (app.node_editor && (key->detail == 119 || key->detail == 22) &&
          app.node_view != nullptr) {
        std::uint64_t selected = 0;
        for (const auto& v : app.node_view->views()) {
          if (v.selected) {
            selected = v.node_id;
            break;
          }
        }
        if (selected != 0U) {
          omnicpp::core::ControlCommand cmd;
          cmd.kind = omnicpp::core::ControlCommand::Kind::NodeRemove;
          cmd.numbers[0] = static_cast<double>(selected);
          cmd.number_count = 1;
          {
            std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
            app.edit_queue.push_back({std::move(cmd)});
          }
          app.node_dirty = true;
        }
      }
      app.kb_mouse.on_key(key->detail, true);
    } else if (type == XCB_KEY_RELEASE) {
      const auto* key =
          reinterpret_cast<const xcb_key_release_event_t*>(event);
      app.kb_mouse.on_key(key->detail, false);
    } else if (type == XCB_MOTION_NOTIFY) {
      const auto* motion =
          reinterpret_cast<const xcb_motion_notify_event_t*>(event);
      app.kb_mouse.on_motion(motion->event_x, motion->event_y);
      app.last_mouse_x = app.mouse_x;
      app.last_mouse_y = app.mouse_y;
      app.mouse_x = static_cast<float>(motion->event_x);
      app.mouse_y = static_cast<float>(motion->event_y);
      // Node drag: translate the grabbed card by the mouse delta.
      if (app.node_editor && app.drag_node_id != 0U &&
          app.node_view != nullptr) {
        (void)app.node_view->translate(
            app.drag_node_id, app.mouse_x - app.last_mouse_x,
            app.mouse_y - app.last_mouse_y);
      }
      // Link drag: the rubber band follows the pointer; the view resolves
      // the hovered compatible pin (drop target highlight is the band
      // color change upstream in the paint path).
      if (app.node_editor && app.node_view != nullptr &&
          app.node_view->link_drag_active()) {
        app.node_view->update_link_drag(app.mouse_x, app.mouse_y);
      }
    } else if (type == XCB_BUTTON_PRESS) {
      const auto* button =
          reinterpret_cast<const xcb_button_press_event_t*>(event);
      app.kb_mouse.on_button(button->detail, true);
      // Right press on a wire: unlink (undoable). Order matters — wires sit
      // under cards, so this must be checked before card grabs.
      if (app.node_editor && button->detail == 3 && app.node_view != nullptr) {
        const auto link_index =
            app.node_view->link_at(app.mouse_x, app.mouse_y, 8.0F);
        const auto* link =
            app.node_view->link_at_index(link_index);
        if (link != nullptr) {
          omnicpp::core::ControlCommand cmd;
          cmd.kind = omnicpp::core::ControlCommand::Kind::UnlinkNodes;
          cmd.numbers[0] = static_cast<double>(link->to_node);
          cmd.number_count = 1;
          cmd.text2 = link->to_pin;
          {
            std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
            app.edit_queue.push_back({std::move(cmd)});
          }
          app.node_dirty = true;
        }
      }
      // Left press on the node canvas: toolbar > pin (link drag) > card
      // (move drag) > deselect.
      if (app.node_editor && button->detail == 1 && app.node_view != nullptr) {
        // Toolbar buttons first (they live over the canvas).
        if (!app.node_toolbar_buttons.empty()) {
          const auto tb = omnicpp::editor::hit_test_toolbar(
              app.ui_tree, app.node_toolbar_buttons, *app.node_graph,
              app.mouse_x, app.mouse_y);
          using TA = omnicpp::editor::ToolbarAction;
          if (tb.action == TA::AddType) {
            omnicpp::core::ControlCommand cmd;
            cmd.kind = omnicpp::core::ControlCommand::Kind::NodeAdd;
            cmd.text = app.node_graph->types()[tb.type_index].name;
            cmd.numbers[0] = static_cast<double>(app.mouse_x);
            cmd.numbers[1] = static_cast<double>(app.mouse_y);
            cmd.number_count = 2;
            {
              std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
              app.edit_queue.push_back({std::move(cmd)});
            }
            app.node_dirty = true;
          } else if (tb.action == TA::Undo) {
            app.undo_requested = true;
          } else if (tb.action == TA::Redo) {
            app.redo_requested = true;
          }
          if (tb.action != TA::None) {
            free(event);
            return true;  // toolbar consumed the click
          }
        }
        // M12: param rows next — click begins editing (one at a time).
        {
          const auto prow = app.node_view->param_row_at(app.mouse_x,
                                                        app.mouse_y);
          if (prow.valid()) {
            (void)app.node_view->begin_param_edit(prow.node_id, prow.param);
            free(event);
            return true;
          }
        }
        const auto pin = app.node_view->pin_at(app.mouse_x, app.mouse_y);
        if (pin.valid()) {
          app.node_view->begin_link_drag(pin);
        } else {
          const auto hit = app.node_view->hit_test(app.mouse_x, app.mouse_y);
          if (hit != 0U) {
            app.drag_node_id = hit;
            (void)app.node_view->select(hit);
            float cx = 0.0F;
            float cy = 0.0F;
            float cw = 0.0F;
            float ch = 0.0F;
            if (app.node_view->node_rect(hit, cx, cy, cw, ch)) {
              app.drag_grab_dx = app.mouse_x - cx;
              app.drag_grab_dy = app.mouse_y - cy;
              app.drag_start_x = cx;
              app.drag_start_y = cy;
            }
          } else {
            // Empty canvas space: the inspector gets the click BEFORE
            // deselecting (its rows select document objects, undoable
            // through the same session path).
            const auto ihit = app.inspector.hit_test(app.ui_tree, app.mouse_x,
                                                     app.mouse_y);
            if (ihit.kind == omnicpp::editor::InspectorHit::Kind::SelectObject) {
              omnicpp::core::ControlCommand sel_cmd;
              sel_cmd.kind = omnicpp::core::ControlCommand::Kind::Select;
              sel_cmd.numbers[0] = static_cast<double>(ihit.object_id);
              sel_cmd.number_count = 1;
              {
                std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
                app.edit_queue.push_back({std::move(sel_cmd)});
              }
              app.inspector_rebuild_requested = true;
            } else if (ihit.kind ==
                       omnicpp::editor::InspectorHit::Kind::None) {
              (void)app.node_view->select(0U);
            }
          }
        }
      }
    } else if (type == XCB_BUTTON_RELEASE) {
      const auto* button =
          reinterpret_cast<const xcb_button_press_event_t*>(event);
      app.kb_mouse.on_button(button->detail, false);
      if (app.node_editor && button->detail == 1) {
        // Finish a link drag first: commit a resolved drop through the
        // session (undoable); an unresolved drop just cancels.
        if (app.node_view != nullptr &&
            app.node_view->link_drag_active()) {
          app.node_view->update_link_drag(app.mouse_x, app.mouse_y);
          const auto drop = app.node_view->end_link_drag(true);
          if (drop.valid()) {
            // Source pin determines direction: output->input, or reversed.
            const auto src = app.node_view->drag_source_pin();
            omnicpp::core::ControlCommand cmd;
            cmd.kind = omnicpp::core::ControlCommand::Kind::LinkNodes;
            cmd.numbers[0] = static_cast<double>(src.is_input ? drop.node_id
                                                              : src.node_id);
            cmd.numbers[1] = static_cast<double>(src.is_input ? src.node_id
                                                              : drop.node_id);
            cmd.number_count = 2;
            cmd.text = src.is_input ? drop.pin_name : src.pin_name;
            cmd.text2 = src.is_input ? src.pin_name : drop.pin_name;
            {
              std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
              app.edit_queue.push_back({std::move(cmd)});
            }
          }
        } else if (app.drag_node_id != 0U) {
          // Card move finished: commit the position as ONE undoable step.
          float cx = 0.0F;
          float cy = 0.0F;
          float cw = 0.0F;
          float ch = 0.0F;
          if (app.node_view != nullptr &&
              app.node_view->node_rect(app.drag_node_id, cx, cy, cw, ch)) {
            omnicpp::core::ControlCommand cmd;
            cmd.kind = omnicpp::core::ControlCommand::Kind::SetNodePosition;
            cmd.numbers[0] = static_cast<double>(app.drag_node_id);
            cmd.numbers[1] = static_cast<double>(cx);
            cmd.numbers[2] = static_cast<double>(cy);
            cmd.number_count = 3;
            {
              std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
              app.edit_queue.push_back({std::move(cmd)});
            }
          }
        }
        app.drag_node_id = 0U;
      }
    } else if (type == XCB_DESTROY_NOTIFY) {
      free(event);
      return false;
    }
    free(event);
  }
  return true;
}

// ============================================================================
// Mannequin: load, GPU upload, and per-frame animation sampling.
// ============================================================================

bool make_mesh(ViewportApp& app, const std::vector<float>& vertices,
               const std::vector<std::uint32_t>& indices,
               ViewportApp::MeshBuffers& out);

//! City scene (OMNICPP_SCENE=city): defined after the RT helpers.
bool setup_city_scene(ViewportApp& app);

//! Frames of payload copies (must match the renderer's frames in flight).
constexpr std::uint32_t kViewportMaxFramesInFlight = 2U;
//! Push block for the vertex-pull draw pipeline (pbr_gpu_driven.vert +
//! pbr_gpu_driven_full.frag): 160 bytes, model/material arrive via payload.
struct GdPush {
  omnicpp::render::SceneMatrix view_projection;
  omnicpp::render::SceneMatrix model_unused;
  std::array<float, 4> camera_position;
  std::uint32_t material_index_unused;
  std::array<std::uint32_t, 3> pad{};
};
static_assert(sizeof(GdPush) == 160U);

//! Read a small binary file fully; false when unavailable.
bool read_file_bytes(const std::string& path, std::string& out_text,
                     std::vector<char>& out_bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  out_bytes.assign((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  out_text.assign(out_bytes.begin(), out_bytes.end());
  return !out_text.empty();
}

//! Extract buffers[0].uri from a glTF JSON text (both "uri" and
//! "uri"-with-spaces forms). Returns "" when absent (embedded data: URIs are
//! decoded inside the importer; a uri-less buffer 0 only exists in GLB).
std::string find_buffer0_uri(const std::string& json_text) {
  const std::size_t buffers_pos = json_text.find("\"buffers\"");
  if (buffers_pos == std::string::npos) return "";
  const std::size_t scan_end =
      std::min(json_text.find(']', buffers_pos), json_text.size());
  const std::size_t uri_key = json_text.find("\"uri\"", buffers_pos);
  if (uri_key == std::string::npos || uri_key > scan_end) return "";
  const std::size_t colon = json_text.find(':', uri_key + 5);
  if (colon == std::string::npos) return "";
  const std::size_t open = json_text.find('"', colon + 1);
  const std::size_t close = json_text.find('"', open + 1);
  if (open == std::string::npos || close == std::string::npos) return "";
  return json_text.substr(open + 1, close - open - 1);
}

bool setup_mannequin(ViewportApp& app) {
  // Asset resolution: OMNICPP_MODEL selects the skeletal glTF document
  // (mannequin by default; may name a subdirectory, e.g. "cesiumman/
  // CesiumMan"), resolved via OMNICPP_ASSET_DIR then conventional fallbacks.
  // Buffer 0 and image files are resolved relative to the document's own
  // directory, exactly as glTF URIs are specified.
  // OMNICPP_NO_MODEL forces the cubes-only scene (A/B harness hook).
  if (std::getenv("OMNICPP_NO_MODEL") != nullptr) {
    std::fprintf(stderr,
                 "viewport: OMNICPP_NO_MODEL set; rendering cubes only\n");
    return true;  // non-fatal: cubes-only scene
  }
  const char* model_env = std::getenv("OMNICPP_MODEL");
  const std::string model = model_env != nullptr ? model_env : "mannequin";
  const char* asset_dir_env = std::getenv("OMNICPP_ASSET_DIR");
  std::vector<std::string> candidates;
  if (asset_dir_env != nullptr) candidates.emplace_back(asset_dir_env);
  candidates.insert(candidates.end(), {"assets/models", "../assets/models",
                                       "../../assets/models"});
  std::string json;
  std::vector<char> bin_bytes;
  bool loaded = false;
  for (const auto& dir : candidates) {
    // An absolute model path is used verbatim; a bare name/subdirectory is
    // resolved against each candidate asset directory in turn.
    const std::string model_path =
        (model.size() > 0 && model[0] == '/')
            ? model + ".gltf"
            : dir + "/" + model + ".gltf";
    std::vector<char> json_bytes;
    if (!read_file_bytes(model_path, json, json_bytes)) {
      continue;
    }
    const std::string model_dir =
        model_path.substr(0, model_path.find_last_of('/'));

    // Buffer 0: external file -> read from the document dir; data: URI ->
    // the importer decodes it (pass null bytes).
    const std::string buffer_uri = find_buffer0_uri(json);
    if (!buffer_uri.empty() &&
        buffer_uri.compare(0, 5, "data:") != 0) {
      std::string bin_text;
      if (!read_file_bytes(model_dir + "/" + buffer_uri, bin_text,
                           bin_bytes)) {
        continue;
      }
    } else {
      bin_bytes.clear();
    }

    // External images (textures) resolve relative to the document dir too.
    const omnicpp::asset::ExternalFileLoader loader =
        [&model_dir](const std::string& uri, std::string& load_error,
                     std::vector<std::uint8_t>& out_bytes) {
          std::vector<char> bytes;
          std::string unused;
          if (!read_file_bytes(model_dir + "/" + uri, unused, bytes)) {
            load_error = "cannot open " + uri;
            return false;
          }
          out_bytes.assign(bytes.begin(), bytes.end());
          return true;
        };

    std::string import_error;
    auto imported = omnicpp::asset::import_gltf_animation_document(
        json_bytes.data(), json_bytes.size(),
        bin_bytes.empty() ? nullptr
                          : reinterpret_cast<const std::uint8_t*>(
                                bin_bytes.data()),
        bin_bytes.size(), &import_error, &loader);
    if (imported.is_ok()) {
      app.mannequin = std::move(imported.value());
      loaded = true;
      break;
    }
    std::fprintf(stderr, "viewport: model import failed from %s: %s\n",
                 dir.c_str(), import_error.c_str());
  }
  if (!loaded) {
    std::fprintf(stderr,
                 "viewport: mannequin asset not found (set OMNICPP_ASSET_DIR "
                 "to assets/models); rendering cubes only\n");
    return true;  // non-fatal: cubes-only scene
  }
  if (app.mannequin.skins.empty() || app.mannequin.animations.empty()) {
    std::fprintf(stderr, "viewport: mannequin has no skin or animation\n");
    return true;
  }

  const VkDevice device = app.context.device();

  // Bone SSBO (set 3): one 64-byte matrix per joint.
  const std::size_t joint_count = app.mannequin.skins[0].joints.size();
  auto bone_buffer = app.allocator.create_buffer(
      joint_count * 64U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!bone_buffer.is_ok()) return false;
  app.bone_allocation = bone_buffer.value();
  auto bone_set = app.descriptors.allocate_set(app.bone_layout);
  if (!bone_set.is_ok()) return false;
  app.bone_set = bone_set.value();
  if (!app.descriptors
           .write_buffer(app.bone_set, 0U,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         app.bone_allocation.buffer, 0U, VK_WHOLE_SIZE)
           .is_ok()) {
    return false;
  }

  // Upload each mannequin mesh: combined SSBO [static verts][joints x4 as
  // float][weights x4] exactly as skinned_scene.vert reads it.
  app.mannequin_meshes.resize(app.mannequin.meshes.size());
  for (std::size_t i = 0; i < app.mannequin.meshes.size(); ++i) {
    const auto& import = app.mannequin.meshes[i];
    const auto& binding = app.mannequin.skin_bindings[i];
    std::vector<float> combined = import.vertices;
    const std::size_t vertex_count = import.vertex_count();
    combined.reserve(combined.size() + vertex_count * 8U);
    for (std::size_t v = 0; v < vertex_count; ++v) {
      for (std::size_t c = 0; c < 4; ++c) {
        combined.push_back(
            static_cast<float>(binding.joints[v * 4U + c]));
      }
      for (std::size_t c = 0; c < 4; ++c) {
        combined.push_back(binding.weights[v * 4U + c]);
      }
    }
    if (!make_mesh(app, combined, import.indices,
                   app.mannequin_meshes[i])) {
      return false;
    }
  }

  // Skinned pipeline (4 sets: mesh / textures / material / bones) over the
  // swapchain render pass.
  const char* shader_dir_env = std::getenv("OMNICPP_SHADER_DIR");
  const std::string shader_dir =
      shader_dir_env != nullptr ? shader_dir_env : "assets/shaders";
  if (!app.skinned_pipeline
           .load_shader_stage_file(device,
                                   shader_dir + "/skinned_scene.vert.spv",
                                   "vertex")
           .is_ok() ||
      !app.skinned_pipeline
           .load_shader_stage_file(device,
                                   shader_dir + "/pbr_scene.frag.spv",
                                   "fragment")
           .is_ok()) {
    std::fprintf(stderr, "viewport: cannot load skinned shaders from %s\n",
                 shader_dir.c_str());
    return false;
  }
  const VkDescriptorSetLayout skinned_layouts[4] = {
      app.mesh_layout, app.textures_layout, app.material_layout,
      app.bone_layout};
  const VkPushConstantRange push_range{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};
  if (!app.skinned_pipeline
           .create_pipeline_layout(device, skinned_layouts, 4U, &push_range)
           .is_ok() ||
      !app.skinned_pipeline
           .create_graphics_pipeline(
               device, app.render_pass.render_pass(),
               app.swapchain.image_format(),
               app.skinned_pipeline.pipeline_layout(), true, true, false)
           .is_ok()) {
    return false;
  }
  app.has_mannequin = true;
  // D1: build the walk/idle machine (only when both clips exist).
  if (app.mannequin.animations.size() >= 2U) {
    using AnimMachine =
        omnicpp::anim::AnimationStateMachine<omnicpp::core::InputSnapshot>;
    app.machine = std::make_unique<AnimMachine>();
    app.machine->add_state("walk", 0.0f);
    app.machine->add_state("idle", 1.0f);
    omnicpp::anim::AnimTransition to_idle;
    to_idle.from = "walk";
    to_idle.to = "idle";
    to_idle.action = "fade_toggle";
    to_idle.min_time_in_state = 0.25f;
    to_idle.fade_duration = 0.4f;
    app.machine->add_transition(to_idle);
    omnicpp::anim::AnimTransition to_walk;
    to_walk.from = "idle";
    to_walk.to = "walk";
    to_walk.action = "fade_toggle";
    to_walk.min_time_in_state = 0.25f;
    to_walk.fade_duration = 0.4f;
    app.machine->add_transition(to_walk);
    app.machine->set_initial("walk");
  }
  return true;
}

//! Sample the walk cycle at `time` and upload joint matrices. Computes
//! joints = global_pose(j) * inverse_bind(j) directly (same math the GPU
//! test cross-checks).
void update_mannequin_pose(ViewportApp& app, float time,
                           std::uint32_t bone_offset = 0U) {
  const auto& skin = app.mannequin.skins[0];
  const auto& anim = app.mannequin.animations[0];

  // Sample channels into node TRS.
  for (const auto& channel : anim.channels) {
    float out[4];
    omnicpp::asset::sample_gltf_channel(anim.samplers[channel.sampler], time,
                                        out);
    auto& node = app.mannequin.nodes[channel.target_node];
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

  // Compose locals.
  std::vector<SceneMatrix> locals(app.mannequin.nodes.size());
  for (std::size_t i = 0; i < app.mannequin.nodes.size(); ++i) {
    const auto& n = app.mannequin.nodes[i];
    SceneMatrix m = omnicpp::render::scene_identity_matrix();
    const float x = n.rotation[0];
    const float y = n.rotation[1];
    const float z = n.rotation[2];
    const float w = n.rotation[3];
    m[0] = (1.0f - 2.0f * (y * y + z * z)) * n.scale[0];
    m[1] = 2.0f * (x * y + z * w) * n.scale[0];
    m[2] = 2.0f * (x * z - y * w) * n.scale[0];
    m[4] = 2.0f * (x * y - z * w) * n.scale[1];
    m[5] = (1.0f - 2.0f * (x * x + z * z)) * n.scale[1];
    m[6] = 2.0f * (y * z + x * w) * n.scale[1];
    m[8] = 2.0f * (x * z + y * w) * n.scale[2];
    m[9] = 2.0f * (y * z - x * w) * n.scale[2];
    m[10] = (1.0f - 2.0f * (x * x + y * y)) * n.scale[2];
    m[12] = n.translation[0];
    m[13] = n.translation[1];
    m[14] = n.translation[2];
    locals[i] = m;
  }

  // Parent-before-child globals from the roots.
  std::vector<SceneMatrix> globals(app.mannequin.nodes.size(),
                                   omnicpp::render::scene_identity_matrix());
  std::vector<std::uint8_t> done(app.mannequin.nodes.size(), 0);
  std::vector<std::size_t> stack;
  for (std::size_t root = 0; root < app.mannequin.nodes.size(); ++root) {
    if (app.mannequin.nodes[root].parent !=
        omnicpp::asset::kGltfNoParent) {
      continue;
    }
    stack.push_back(root);
    while (!stack.empty()) {
      const std::size_t current = stack.back();
      stack.pop_back();
      if (done[current] != 0) continue;
      const SceneMatrix& local = locals[current];
      const std::size_t parent = app.mannequin.nodes[current].parent;
      globals[current] =
          parent == omnicpp::asset::kGltfNoParent
              ? local
              : multiply(globals[parent], local);
      done[current] = 1;
      for (const std::size_t child : app.mannequin.nodes[current].children) {
        if (done[child] == 0) stack.push_back(child);
      }
    }
  }

  // Joint matrices: global * inverse bind, in joint order, written at
  // bone_offset (multi-actor bone arena: actor a's slice starts at
  // 1 + a * joints_per_actor; slot 0 is the identity bone).
  auto* bones = static_cast<SceneMatrix*>(app.bone_allocation.mapped);
  std::size_t swing_index = 0;
  float swing_max = -1.0f;
  for (std::size_t j = 0; j < skin.joints.size(); ++j) {
    const SceneMatrix& g = globals[skin.joints[j]];
    const SceneMatrix& ibm = skin.inverse_bind_matrices[j];
    bones[bone_offset + j] = multiply(g, ibm);
    // Telemetry: track the joint with the largest rotation away from its
    // bind pose (the trace of the rotation part drops to cos(2*theta) as a
    // joint rotates by theta). Root translations don't count.
    const float trace = g[0] + g[5] + g[10];
    const float deviation = (3.0f - trace) * 0.5f;  // 0..2, rad^2-ish
    if (deviation > swing_max) {
      swing_max = deviation;
      swing_index = j;
    }
  }
  if (swing_max > 0.0f && skin.joints.size() > swing_index) {
    app.last_swing_joint = app.mannequin.nodes[skin.joints[swing_index]].name;
    app.last_swing_deg = std::acos(std::min(
                        std::max((swing_max - 1.0f) * -1.0f, -1.0f), 1.0f)) *
                        57.2958f;
  }
}

// ============================================================================
// Scene setup (mirrors the proven PBR harness layout)
// ============================================================================

bool make_mesh(ViewportApp& app, const std::vector<float>& vertices,
               const std::vector<std::uint32_t>& indices,
               ViewportApp::MeshBuffers& out) {
  const VkDeviceSize vertex_bytes = vertices.size() * sizeof(float);
  const VkDeviceSize index_bytes = indices.size() * sizeof(std::uint32_t);
  auto vb = app.allocator.create_buffer(
      vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto ib = app.allocator.create_buffer(
      index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!vb.is_ok() || !ib.is_ok()) return false;
  out.vertex_allocation = vb.value();
  out.index_allocation = ib.value();
  std::memcpy(out.vertex_allocation.mapped, vertices.data(), vertex_bytes);
  std::memcpy(out.index_allocation.mapped, indices.data(), index_bytes);

  auto set = app.descriptors.allocate_set(app.mesh_layout);
  if (!set.is_ok()) return false;
  if (!app.descriptors
           .write_buffer(set.value(), 0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         out.vertex_allocation.buffer, 0U, VK_WHOLE_SIZE)
           .is_ok()) {
    return false;
  }
  out.mesh.vertex_buffer = out.vertex_allocation.buffer;
  out.mesh.index_buffer = out.index_allocation.buffer;
  out.mesh.index_count = static_cast<std::uint32_t>(indices.size());
  out.mesh.descriptor_set = set.value();
  return true;
}

bool setup_scene(ViewportApp& app) {
  if (!app.allocator
           .initialize(app.context.device(), app.context.physical_device())
           .is_ok()) {
    return false;
  }
  if (!app.descriptors.initialize(app.context.device()).is_ok()) {
    return false;
  }

  // Set 0: per-mesh vertex storage (SSBO).
  const std::vector<omnicpp::render::ReflectedBinding> mesh_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT}};
  // Pool sized for cubes + ground + mannequin meshes + the city scene's
  // static meshes (buildings, streetlights, actors).
  auto mesh_layout = app.descriptors.create_layout(mesh_bindings, 96U);
  if (!mesh_layout.is_ok()) return false;
  app.mesh_layout = mesh_layout.value();

  // Set 1: bindless sampler array (element 0 = opaque-white fallback).
  const std::vector<omnicpp::render::ReflectedBinding> textures_bindings = {
      {1U, 0U, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto textures_layout =
      app.descriptors.create_layout(textures_bindings, 1U, true);
  if (!textures_layout.is_ok()) return false;
  app.textures_layout = textures_layout.value();
  auto textures_set = app.descriptors.allocate_set(app.textures_layout);
  if (!textures_set.is_ok()) return false;
  app.textures_set = textures_set.value();

  // Set 2: material SSBO (city scene: 4 base slots + per-part Sponza
  // materials; each city instance reuses its material slots).
  const std::vector<omnicpp::render::ReflectedBinding> material_bindings = {
      {2U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto material_layout = app.descriptors.create_layout(material_bindings, 96U);
  if (!material_layout.is_ok()) return false;
  app.material_layout = material_layout.value();
  auto material_set = app.descriptors.allocate_set(app.material_layout);
  if (!material_set.is_ok()) return false;
  app.material_set = material_set.value();

  auto material_buffer = app.allocator.create_buffer(
      256U * sizeof(omnicpp::render::PbrMaterialData),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!material_buffer.is_ok()) return false;
  app.material_allocation = material_buffer.value();
  if (!app.descriptors
           .write_buffer(app.material_set, 0U,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         app.material_allocation.buffer, 0U, VK_WHOLE_SIZE)
           .is_ok()) {
    return false;
  }
  auto* materials = static_cast<omnicpp::render::PbrMaterialData*>(
      app.material_allocation.mapped);
  // 0: brushed metal cube, 1: rough dielectric cube, 2: ground, 3: skin.
  // Slots 4+ are city-scene dynamic (Sponza imports at kSponzaMaterialBase).
  for (int i = 0; i < 256; ++i) materials[i] = {};
  materials[0] = {};
  materials[0].base_color_factor = {0.95f, 0.35f, 0.15f, 1.0f};
  materials[0].metallic_factor = 0.9f;
  materials[0].roughness_factor = 0.25f;
  materials[1] = {};
  materials[1].base_color_factor = {0.15f, 0.45f, 0.95f, 1.0f};
  materials[1].metallic_factor = 0.0f;
  materials[1].roughness_factor = 0.7f;
  materials[2] = {};
  materials[2].base_color_factor = {0.35f, 0.38f, 0.42f, 1.0f};
  materials[2].metallic_factor = 0.0f;
  materials[2].roughness_factor = 0.95f;

  // Geometry: two unit cubes + a wide thin ground slab.
  std::vector<float> vertices;
  std::vector<std::uint32_t> indices;
  build_unit_cube(vertices, indices);
  if (!make_mesh(app, vertices, indices, app.cube)) return false;
  if (!make_mesh(app, vertices, indices, app.ground)) return false;

  // PBR pipeline over the swapchain's render pass. Shader paths are
  // relative to the build's compiled-shader output directory.
  const char* shader_dir_env = std::getenv("OMNICPP_SHADER_DIR");
  const std::string shader_dir =
      shader_dir_env != nullptr ? shader_dir_env : "assets/shaders";
  if (!app.pbr_pipeline
           .load_shader_stage_file(app.context.device(),
                                   shader_dir + "/pbr_scene.vert.spv",
                                   "vertex")
           .is_ok() ||
      !app.pbr_pipeline
           .load_shader_stage_file(app.context.device(),
                                   shader_dir + "/pbr_scene.frag.spv",
                                   "fragment")
           .is_ok()) {
    std::fprintf(stderr,
                 "viewport: cannot load shaders from %s (set OMNICPP_SHADER_"
                 "DIR to the directory containing pbr_scene.*.spv)\n",
                 shader_dir.c_str());
    return false;
  }
  const VkDescriptorSetLayout set_layouts[3] = {app.mesh_layout,
                                                app.textures_layout,
                                                app.material_layout};
  const VkPushConstantRange push_range{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};
  if (!app.pbr_pipeline
           .create_pipeline_layout(app.context.device(), set_layouts, 3U,
                                   &push_range)
           .is_ok() ||
      !app.pbr_pipeline
           .create_graphics_pipeline(
               app.context.device(), app.render_pass.render_pass(),
               app.swapchain.image_format(), app.pbr_pipeline.pipeline_layout(),
               true, true, false)
           .is_ok()) {
    return false;
  }

  // Scene description.
  app.scene.pipeline = app.pbr_pipeline.pipeline();
  app.scene.pipeline_layout = app.pbr_pipeline.pipeline_layout();
  app.scene.camera_position = {0.0f, 0.0f, 0.0f, 1.0f};
  app.scene.texture_set = app.textures_set;
  app.scene.material_set = app.material_set;
  app.scene.objects.reserve(3);

  // Fourth material slot: mannequin skin tone.
  materials[3] = {};
  materials[3].base_color_factor = {0.82f, 0.62f, 0.48f, 1.0f};
  materials[3].metallic_factor = 0.0f;
  materials[3].roughness_factor = 0.85f;

  // Skinned-pipeline descriptor layouts (bone SSBO at set 3) and the
  // mannequin itself. Non-fatal when the asset is missing.
  const std::vector<omnicpp::render::ReflectedBinding> bone_bindings = {
      {3U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT}};
  auto bone_layout = app.descriptors.create_layout(bone_bindings, 8U);
  if (!bone_layout.is_ok()) return false;
  app.bone_layout = bone_layout.value();
  if (!setup_mannequin(app)) return false;
  if (app.city_scene && !setup_city_scene(app)) {
    std::fprintf(stderr,
                 "viewport: city scene setup failed; single-actor scene\n");
    app.city_scene = false;
  }

  // Set 6: dynamic point lights (many-light city variant). Created lazily
  // when the city scene is enabled (needs the engine lights layout).

  return true;
}

//! GPU-driven path (cubes scene): shared geometry through the mesh table
//! (one slot per mesh, no LOD chain), payload/indirect buffers with per-image
//! payload copies, the cull/LOD compute pipeline, and the vertex-pull draw
//! pipeline over the swapchain render pass. Composes on top of composed
//! lighting (IBL + shadows) when lighting_ready.
bool setup_gpu_driven(ViewportApp& app) {
  // 144-byte cull push: object/draw/visible word indices + 6 planes +
  // tan/viewport + LOD thresholds (matches cull_and_draw_lod.comp).
  const VkPushConstantRange kGdCullPush{VK_SHADER_STAGE_COMPUTE_BIT, 0U, 144U};
  VkDevice dev = app.context.device();
  const char* shader_dir_env = std::getenv("OMNICPP_SHADER_DIR");
  const std::string shader_dir =
      shader_dir_env != nullptr ? shader_dir_env : "assets/shaders";

  // --- Shared geometry through the mesh table ----------------------------
  // Host-side copies of the unit-cube geometry (same 11-float layout as the
  // per-draw path) go through the table builder, which rewrites indices into
  // one global vertex space. The ground reuses the SAME unit cube — its
  // (8, 0.1, 8) scale lives in the payload's model matrix, exactly like the
  // per-draw path, so the A/B parity is byte-exact (both meshes dedupe to
  // one table slot).
  std::vector<float> cube_verts;
  std::vector<std::uint32_t> cube_idx;
  build_unit_cube(cube_verts, cube_idx);

  app.gd_slot_cube = app.gd_table_builder.add(
      omnicpp::render::SceneMesh{}, cube_verts, cube_idx);
  app.gd_slot_ground = app.gd_table_builder.add(
      omnicpp::render::SceneMesh{}, cube_verts, cube_idx);
  if (app.gd_slot_cube ==
          omnicpp::render::SceneMeshTableBuilder::kInvalidSlot ||
      app.gd_slot_ground ==
          omnicpp::render::SceneMeshTableBuilder::kInvalidSlot) {
    std::fprintf(stderr, "viewport: gd mesh table add failed\n");
    return false;
  }
  app.gd_table = app.gd_table_builder.build();

  // --- Shared vertex/index buffers (host-visible, app convention) --------
  auto vbuf = app.allocator.create_buffer(
      app.gd_table.vertex_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto ibuf = app.allocator.create_buffer(
      app.gd_table.index_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!vbuf.is_ok() || !ibuf.is_ok()) {
    std::fprintf(stderr, "viewport: gd shared geometry buffers failed\n");
    return false;
  }
  app.gd_shared_vertex_allocation = vbuf.value();
  app.gd_shared_vertex_buffer = app.gd_shared_vertex_allocation.buffer;
  app.gd_shared_index_allocation = ibuf.value();
  app.gd_shared_index_buffer = app.gd_shared_index_allocation.buffer;
  std::memcpy(app.gd_shared_vertex_allocation.mapped,
              app.gd_table.vertex_data.data(), app.gd_table.vertex_bytes());
  std::memcpy(app.gd_shared_index_allocation.mapped,
              app.gd_table.index_data.data(), app.gd_table.index_bytes());

  // --- Payload (per-image) + indirect buffers -----------------------------
  const std::uint32_t instance_count = app.gd_instance_count;
  constexpr VkDeviceSize kPayloadBytes =
      (2U + 24U * kGdMaxInstances) * 4U;
  constexpr VkDeviceSize kDrawWords = 5U * kGdMaxInstances + 1U;  // + counter
  app.gd_payload_buffers.resize(kViewportMaxFramesInFlight);
  app.gd_payload_allocations.resize(kViewportMaxFramesInFlight);
  for (std::uint32_t i = 0; i < kViewportMaxFramesInFlight; ++i) {
    auto pbuf = app.allocator.create_buffer(
        kPayloadBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!pbuf.is_ok()) {
      std::fprintf(stderr, "viewport: gd payload buffer failed\n");
      return false;
    }
    app.gd_payload_allocations[i] = pbuf.value();
    app.gd_payload_buffers[i] = app.gd_payload_allocations[i].buffer;
  }
  auto dbuf = app.allocator.create_buffer(
      kDrawWords * 4U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!dbuf.is_ok()) {
    std::fprintf(stderr, "viewport: gd indirect buffer failed\n");
    return false;
  }
  app.gd_indirect_allocation = dbuf.value();
  app.gd_indirect_buffer = app.gd_indirect_allocation.buffer;

  // Mesh table buffer (5 words per slot, read by the cull pass at binding 2).
  const VkDeviceSize table_bytes =
      static_cast<VkDeviceSize>(app.gd_table.entries.size()) * 5U * 4U;
  auto tbuf = app.allocator.create_buffer(
      table_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!tbuf.is_ok()) {
    std::fprintf(stderr, "viewport: gd table buffer failed\n");
    return false;
  }
  app.gd_table_allocation = tbuf.value();
  app.gd_table_buffer = app.gd_table_allocation.buffer;
  std::memcpy(app.gd_table_allocation.mapped, app.gd_table.entries.data(),
              static_cast<std::size_t>(table_bytes));

  // --- Set-0 layout (shared by cull + draw): vertices/payload/table/cmds --
  const std::vector<omnicpp::render::ReflectedBinding> gd_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT},
      {0U, 1U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT},
      {0U, 2U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT},
      {0U, 3U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT}};
  auto gd_layout = app.descriptors.create_layout(gd_bindings, 16U);
  if (!gd_layout.is_ok()) {
    std::fprintf(stderr, "viewport: gd set0 layout failed\n");
    return false;
  }
  app.gd_set0_layout = gd_layout.value();
  app.gd_cull_sets.resize(kViewportMaxFramesInFlight);
  app.gd_draw_sets.resize(kViewportMaxFramesInFlight);
  for (std::uint32_t i = 0; i < kViewportMaxFramesInFlight; ++i) {
    auto cs = app.descriptors.allocate_set(app.gd_set0_layout);
    auto ds = app.descriptors.allocate_set(app.gd_set0_layout);
    if (!cs.is_ok() || !ds.is_ok()) {
      std::fprintf(stderr, "viewport: gd set allocation failed\n");
      return false;
    }
    app.gd_cull_sets[i] = cs.value();
    app.gd_draw_sets[i] = ds.value();
    for (VkDescriptorSet* set : {&app.gd_cull_sets[i], &app.gd_draw_sets[i]}) {
      if (!app.descriptors
               .write_buffer(*set, 0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             app.gd_shared_vertex_buffer, 0U, VK_WHOLE_SIZE)
               .is_ok() ||
          !app.descriptors
               .write_buffer(*set, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             app.gd_payload_buffers[i], 0U, VK_WHOLE_SIZE)
               .is_ok() ||
          !app.descriptors
               .write_buffer(*set, 2U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             app.gd_table_buffer, 0U, VK_WHOLE_SIZE)
               .is_ok() ||
          !app.descriptors
               .write_buffer(*set, 3U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             app.gd_indirect_buffer, 0U, VK_WHOLE_SIZE)
               .is_ok()) {
        std::fprintf(stderr, "viewport: gd set writes failed\n");
        return false;
      }
    }
  }

  // --- Pipelines ----------------------------------------------------------
  if (!app.gd_cull_pipeline
           .load_shader_stage_file(dev, shader_dir + "/cull_and_draw_lod.comp.spv", "compute")
           .is_ok() ||
      !app.gd_cull_pipeline
           .create_pipeline_layout(dev, &app.gd_set0_layout, 1U, &kGdCullPush)
           .is_ok() ||
      !app.gd_cull_pipeline
           .create_compute_pipeline(dev,
                                    app.gd_cull_pipeline.pipeline_layout())
           .is_ok()) {
    std::fprintf(stderr, "viewport: gd cull pipeline failed\n");
    app.gd_cull_pipeline.cleanup(dev);
    return false;
  }
  // The driven fragment shader statically uses the shadow (set 4) and IBL
  // (set 5) slots, so the draw layout declares all six sets (set 3 bones is
  // unused by the driven path but must occupy its array position). gd mode is
  // only enabled with composed lighting (both sets are always bound).
  const VkDescriptorSetLayout draw_layouts[6] = {
      app.gd_set0_layout, app.textures_layout, app.material_layout,
      app.bone_layout, app.shadow_layout, app.ibl5_layout};
  const VkPushConstantRange draw_push{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};
  if (!app.gd_draw_pipeline
           .load_shader_stage_file(dev,
                                   shader_dir + "/pbr_gpu_driven.vert.spv",
                                   "vertex")
           .is_ok() ||
      !app.gd_draw_pipeline
           .load_shader_stage_file(
               dev, shader_dir + "/pbr_gpu_driven_full.frag.spv", "fragment")
           .is_ok() ||
      !app.gd_draw_pipeline
           .create_pipeline_layout(               dev, draw_layouts, 6U, &draw_push)
           .is_ok() ||
      !app.gd_draw_pipeline
           .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                     app.swapchain.image_format(),
                                     app.gd_draw_pipeline.pipeline_layout(),
                                     /*depth_test=*/true,
                                     /*depth_write=*/true, /*cull=*/true)
           .is_ok()) {
    std::fprintf(stderr, "viewport: gd draw pipeline failed\n");
    return false;
  }
  app.gd_draw_pipeline_layout = app.gd_draw_pipeline.pipeline_layout();
  return true;
}

//! Per-frame CPU work for the GPU-driven path: compute the same object
//! transforms as the per-draw scene (pure function of sim time), write the
//! payload slot for THIS frame's in-flight copy, and build the cull push
//! (eye-anchored frustum planes in the cull shader's convention).
void write_gpu_driven_payload(ViewportApp& app, std::uint32_t frame_slot,
                              float t, std::uint32_t height) {
  const std::uint32_t kObjectCount = app.gd_instance_count;
  auto* words = static_cast<std::uint32_t*>(
      app.gd_payload_allocations[frame_slot].mapped);
  words[0] = kObjectCount;
  words[1] = 0U;  // reserved

  const auto write_obj = [&](std::uint32_t obj, const SceneMatrix& model,
                             std::uint32_t material, std::uint32_t slot,
                             const std::array<float, 3>& center,
                             float radius) {
    auto* u = words + 2U + 24U * obj;
    std::memcpy(u, model.data(), 64U);
    u[16] = material;
    u[17] = slot;
    auto* sph = reinterpret_cast<float*>(u + 18U);
    sph[0] = center[0]; sph[1] = center[1]; sph[2] = center[2];
    sph[3] = radius;
    u[22] = 1U;  // lod_count
    u[23] = 0U;
  };

  // Same transforms AND geometry as record_scene_into's cubes branch: the
  // ground is the shared unit cube scaled x(8, 0.1, 8) (not baked geometry),
  // so the A/B parity is byte-exact.
  write_obj(0U,
            multiply(translation_matrix(0.0f, -0.05f, 0.0f),
                     scale_matrix(8.0f, 0.1f, 8.0f)),
            2U, app.gd_slot_cube, {0.0f, -0.05f, 0.0f},
            0.8660254f * 8.0f);
  // Objects 1..N: physics bodies (OMNICPP_PHYSICS=1) falling/rolling on the
  // slab, or the static spinner + rough cube. Physics transforms come straight
  // from the stepped bodies; spheres render as scaled cubes at body positions.
  if (!app.physics_bodies.empty()) {
    const std::uint32_t n =
        std::min<std::uint32_t>(
            static_cast<std::uint32_t>(app.physics_bodies.size()),
            kGdMaxInstances - 1U);
    for (std::uint32_t i = 0; i < n; ++i) {
      const auto& b = app.physics_bodies[i];
      write_obj(1U + i,
                multiply(translation_matrix(b.position[0], b.position[1],
                                            b.position[2]),
                         scale_matrix(b.radius * 0.9f, b.radius * 0.9f,
                                      b.radius * 0.9f)),
                (i & 1U), app.gd_slot_cube,
                {b.position[0], b.position[1], b.position[2]},
                b.radius * 0.8660254f * 0.9f * 1.7320508f);
    }
  } else {
    write_obj(1U,
              multiply(translation_matrix(0.0f, 1.4f, 0.0f),
                       rotation_y_matrix(t * 0.8f)),
              0U, app.gd_slot_cube, {0.0f, 1.4f, 0.0f},
              0.8660254f * 1.35f);
    write_obj(2U,
              multiply(translation_matrix(-2.4f, 1.0f, 0.6f),
                       multiply(rotation_y_matrix(-t * 0.5f),
                                scale_matrix(0.7f, 0.7f, 0.7f))),
              1U, app.gd_slot_cube, {-2.4f, 1.0f, 0.6f},
              0.8660254f * 0.7f);
  }

  // Cull push (144 bytes): word indices + 6 planes + LOD params.
  // Camera basis: computed from the SAME pure orbit formula record_scene_into
  // uses for time t (the hook runs before it, with app.time == t), so the
  // cull frustum matches the frame's view projection exactly.
  auto* pushu =
      reinterpret_cast<std::uint32_t*>(app.gd_cull_push_staging.data());
  auto* pushf = reinterpret_cast<float*>(app.gd_cull_push_staging.data());
  pushu[0] = kObjectCount;
  pushu[1] = 2U + 18U;        // sphere word of object 0
  pushu[2] = 0U;              // draw-command word 0
  pushu[3] = 5U * kObjectCount;  // visible-counter word
  const float orbit_base = std::isnan(app.run_config.camera_radius)
                               ? 6.5f
                               : app.run_config.camera_radius;
  const float orbit_height = std::isnan(app.run_config.camera_height)
                                 ? 3.2f
                                 : app.run_config.camera_height;
  const float orbit_angle = t * 0.25f + app.camera_orbit_bias * t;
  const float orbit_radius =
      std::min(std::max(orbit_base + app.camera_radius_bias, 1.2f), 12.0f);
  const float eye[3] = {orbit_radius * std::cos(orbit_angle), orbit_height,
                        orbit_radius * std::sin(orbit_angle)};
  const float target_y = 0.8f;
  float fwd[3] = {-eye[0], target_y - eye[1], -eye[2]};
  const float fl =
      std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  fwd[0] /= fl; fwd[1] /= fl; fwd[2] /= fl;
  // Camera basis: right = normalize(cross(fwd, world_up)), up =
  // cross(right, fwd).
  float right[3] = {-fwd[2], 0.0f, fwd[0]};
  const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
  right[0] /= rl; right[2] /= rl;
  const float up[3] = {right[1] * fwd[2] - right[2] * fwd[1],
                       right[2] * fwd[0] - right[0] * fwd[2],
                       right[0] * fwd[1] - right[1] * fwd[0]};
  const float th = std::tan(1.05f * 0.5f);  // tan(fov_y/2), fov matches
                                            // record_scene_into's 1.05f
  const float r_eye = right[0] * eye[0] + right[2] * eye[2];
  const float u_eye =
      up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2];
  const float f_eye =
      fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2];
  struct Plane { float n[3]; float d; };
  const Plane planes[6] = {
      // left bound: keeps dot(right, p-eye) >= -th*depth
      {{right[0] + th * fwd[0], th * fwd[1], right[2] + th * fwd[2]},
       -r_eye - th * f_eye},
      // right bound: keeps dot(right, p-eye) <= th*depth
      {{-right[0] + th * fwd[0], -th * fwd[1], -right[2] + th * fwd[2]},
       r_eye - th * f_eye},
      // bottom: keeps dot(up, p-eye) >= -th*depth
      {{up[0] + th * fwd[0], up[1] + th * fwd[1], up[2] + th * fwd[2]},
       -u_eye - th * f_eye},
      // top: keeps dot(up, p-eye) <= th*depth
      {{-up[0] + th * fwd[0], -up[1] + th * fwd[1], -up[2] + th * fwd[2]},
       u_eye - th * f_eye},
      // near: keeps dot(fwd, p-eye) >= 0.1
      {{fwd[0], fwd[1], fwd[2]}, -f_eye - 0.1f},
      // far: keeps dot(fwd, p-eye) <= 150
      {{-fwd[0], -fwd[1], -fwd[2]}, f_eye + 150.0f},
  };
  for (int p = 0; p < 6; ++p) {
    pushf[4 + p * 4 + 0] = planes[p].n[0];
    pushf[4 + p * 4 + 1] = planes[p].n[1];
    pushf[4 + p * 4 + 2] = planes[p].n[2];
    pushf[4 + p * 4 + 3] = planes[p].d;
  }
  pushf[28] = th;
  pushf[29] = static_cast<float>(height);
  pushu[30] = 1U;        // single LOD level
  pushf[31] = 0.0f;      // threshold (unused at count 1)
  (void)pushu;
}

//! Build the composed full-lighting stack: IBL bake from the analytic sky,
//! shadow-map resources (depth image, render pass, UBO, descriptor set),
//! the composed pbr_full pipelines (static + skinned), and the depth-only
//! shadow pipelines. Everything degrades gracefully: on failure the viewport
//! falls back to the basic pbr_scene path (no IBL/shadow), which keeps the
//! cubes-only mode and CI environments without the new shaders working.
// ============================================================================
// RT mode (OMNICPP_RT_MODE=1): hard ray-query shadows against a scene TLAS.
// ============================================================================

std::uint64_t rt_device_address(const ViewportApp& app, VkBuffer buffer) {
  VkBufferDeviceAddressInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  info.buffer = buffer;
  return static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(app.context.device(), &info));
}

//! Extract bind-pose triangle positions (9 floats per triangle, BLAS build
//! input layout) from an indexed glTF import (positions only, bind pose).
std::vector<float> rt_import_triangles(
    const omnicpp::asset::GltfMeshImport& import) {
  std::vector<float> triangles;
  triangles.reserve(import.indices.size() * 3U);
  for (const std::uint32_t idx : import.indices) {
    const float* v = &import.vertices[static_cast<std::size_t>(idx) * 3U];
    triangles.push_back(v[0]);
    triangles.push_back(v[1]);
    triangles.push_back(v[2]);
  }
  return triangles;
}

//! Build one BLAS from bind-pose triangle positions; the geometry buffer is
//! retained in `out` so the build input stays valid for the recorded build.
bool rt_build_blas(ViewportApp& app, const std::vector<float>& triangles,
                   ViewportApp::RtBlas& out) {
  if (triangles.empty()) return false;
  auto geom = app.allocator.create_buffer(
      triangles.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!geom.is_ok()) return false;
  std::memcpy(geom.value().mapped, triangles.data(),
              triangles.size() * sizeof(float));

  omnicpp::render::BlasBuildInput input{};
  input.vertex_buffer_address = rt_device_address(app, geom.value().buffer);
  input.triangle_count = triangles.size() / 9U;
  input.max_vertex = static_cast<std::uint32_t>(triangles.size() / 3U - 1U);
  if (input.vertex_buffer_address == 0U) return false;

  const auto sizes = omnicpp::render::VulkanAccelerationStructureBuilder::
      query_blas_sizes(app.context.device(), input);
  auto blas = app.rt_builder.create_blas(app.context.device(), app.allocator,
                                         input);
  if (!blas.is_ok()) return false;
  out.as = blas.value();
  out.geometry = geom.value();
  out.input = input;
  app.rt_blas_scratch_bytes =
      std::max(app.rt_blas_scratch_bytes, sizes.buildScratchSize);
  return true;
}

bool setup_rt_shadows(ViewportApp& app) {
  VkDevice dev = app.context.device();
  if (!app.context.has_ray_tracing()) {
    std::fprintf(stderr, "viewport: RT mode requested but device lacks "
                         "VK_KHR_acceleration_structure/ray_query\n");
    return false;
  }
  const char* shader_dir_env = std::getenv("OMNICPP_SHADER_DIR");
  const std::string shader_dir =
      shader_dir_env != nullptr ? shader_dir_env : "assets/shaders";

  // --- 1. Composed RT pipelines (same 6-set layout shape as the PCF
  //        family; set 4 is the TLAS layout). ----------------------------
  const std::vector<omnicpp::render::ReflectedBinding> rt_bindings = {
      {4U, 0U, 1U, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto rt_layout = app.descriptors.create_layout(rt_bindings, 1U);
  if (!rt_layout.is_ok()) {
    std::fprintf(stderr, "viewport: RT: create_layout failed\n");
    return false;
  }
  app.rt_layout = rt_layout.value();

  const VkDescriptorSetLayout rt_layouts[6] = {
      app.mesh_layout, app.textures_layout, app.material_layout,
      app.bone_layout, app.rt_layout, app.ibl5_layout};
  const VkPushConstantRange push_range{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};
  if (!app.rt_full_pipeline
           .load_shader_stage_file(dev, shader_dir + "/pbr_scene.vert.spv",
                                   "vertex")
           .is_ok() ||
      !app.rt_full_pipeline
           .load_shader_stage_file(dev, shader_dir + "/pbr_rt_full.frag.spv",
                                   "fragment")
           .is_ok() ||
      !app.rt_full_pipeline
           .create_pipeline_layout(dev, rt_layouts, 6U, &push_range)
           .is_ok() ||
      !app.rt_full_pipeline
           .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                     app.swapchain.image_format(),
                                     app.rt_full_pipeline.pipeline_layout(),
                                     /*depth_test=*/true, /*depth_write=*/true,
                                     /*cull=*/true)
           .is_ok()) {
    std::fprintf(stderr, "viewport: RT composed pipeline failed\n");
    return false;
  }
  if (app.has_mannequin &&
      (!app.rt_full_skinned_pipeline
               .load_shader_stage_file(dev,
                                       shader_dir + "/skinned_scene.vert.spv",
                                       "vertex")
               .is_ok() ||
       !app.rt_full_skinned_pipeline
               .load_shader_stage_file(dev,
                                       shader_dir + "/pbr_rt_full.frag.spv",
                                       "fragment")
               .is_ok() ||
       !app.rt_full_skinned_pipeline
               .create_pipeline_layout(dev, rt_layouts, 6U, &push_range)
               .is_ok() ||
       !app.rt_full_skinned_pipeline
               .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                         app.swapchain.image_format(),
                                         app.rt_full_skinned_pipeline
                                             .pipeline_layout(),
                                         /*depth_test=*/true,
                                         /*depth_write=*/true, /*cull=*/true)
               .is_ok())) {
    std::fprintf(stderr, "viewport: RT composed skinned pipeline failed\n");
    return false;
  }

  // --- 1b. Many-light RT variants (OMNICPP_SCENE=city + OMNICPP_RT_MODE).
  // Built here because app.rt_layout exists only after RT setup; same
  // 7-set shape as the PCF-ML family with set 4 = TLAS.
  if (app.city_scene) {
    const VkDescriptorSetLayout ml_rt_layouts[7] = {
        app.mesh_layout, app.textures_layout, app.material_layout,
        app.bone_layout, app.rt_layout, app.ibl5_layout,
        app.city_lights_layout};
    if (!app.rt_full_ml_pipeline
             .load_shader_stage_file(dev, shader_dir + "/pbr_scene.vert.spv",
                                     "vertex")
             .is_ok() ||
        !app.rt_full_ml_pipeline
             .load_shader_stage_file(
                 dev, shader_dir + "/pbr_rt_full_ml.frag.spv", "fragment")
             .is_ok() ||
        !app.rt_full_ml_pipeline
             .create_pipeline_layout(dev, ml_rt_layouts, 7U, &push_range)
             .is_ok() ||
        !app.rt_full_ml_pipeline
             .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                       app.swapchain.image_format(),
                                       app.rt_full_ml_pipeline.pipeline_layout(),
                                       true, true, true)
             .is_ok()) {
      std::fprintf(stderr, "viewport: RT many-light pipeline failed\n");
      return false;
    }
    if (app.has_mannequin &&
        (!app.rt_full_ml_skinned_pipeline
                 .load_shader_stage_file(
                     dev, shader_dir + "/skinned_scene.vert.spv", "vertex")
                 .is_ok() ||
         !app.rt_full_ml_skinned_pipeline
                 .load_shader_stage_file(
                     dev, shader_dir + "/pbr_rt_full_ml.frag.spv", "fragment")
                 .is_ok() ||
         !app.rt_full_ml_skinned_pipeline
                 .create_pipeline_layout(dev, ml_rt_layouts, 7U, &push_range)
                 .is_ok() ||
         !app.rt_full_ml_skinned_pipeline
                 .create_graphics_pipeline(
                     dev, app.render_pass.render_pass(),
                     app.swapchain.image_format(),
                     app.rt_full_ml_skinned_pipeline.pipeline_layout(), true,
                     true, true)
                 .is_ok())) {
      std::fprintf(stderr,
                   "viewport: RT many-light skinned pipeline failed\n");
      return false;
    }
  }

  // --- 2. TLAS descriptor (set 4 for both RT pipelines). ----------------
  auto rt_set = app.descriptors.allocate_set(app.rt_layout);
  if (!rt_set.is_ok()) {
    std::fprintf(stderr, "viewport: RT: allocate_set failed\n");
    return false;
  }
  app.rt_set = rt_set.value();

  // --- 3. BLASes from bind-pose geometry. -------------------------------
  // Cube + ground share the canonical quad-indexed unit cube (12 tris);
  // mannequin parts are indexed glTF meshes (positions only, bind pose).
  auto build_cube_blas = [&](std::vector<ViewportApp::RtBlas>& out) {
    std::vector<float> tris;
    tris.reserve(12U * 9U);
    for (const std::uint32_t idx : kRtCubeIndices) {
      for (int c = 0; c < 3; ++c) {
        tris.push_back(kRtCubePositions[idx * 3U + c]);
      }
    }
    ViewportApp::RtBlas b{};
    return rt_build_blas(app, tris, b) && (out.push_back(b), true);
  };
  if (!build_cube_blas(app.rt_blas_cube)) {
    std::fprintf(stderr, "viewport: RT: cube BLAS failed\n");
    return false;
  }
  if (!build_cube_blas(app.rt_blas_ground)) {
    std::fprintf(stderr, "viewport: RT: ground BLAS failed\n");
    return false;
  }

  // E5: record each part's dominant joint (single-joint binding: the
  // first vertex's highest-weight joint — the generator binds each part
  // to exactly one joint).
  for (const auto& binding : app.mannequin.skin_bindings) {
    float best_w = -1.0f;
    std::size_t best_j = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      const float w = binding.weights[k];
      if (w > best_w) {
        best_w = w;
        best_j = binding.joints[k];
      }
    }
    app.rt_part_joint.push_back(best_j);
  }
  for (const auto& import : app.mannequin.meshes) {
    ViewportApp::RtBlas b{};
    if (!rt_build_blas(app, rt_import_triangles(import), b)) {
      std::fprintf(stderr, "viewport: RT: mannequin BLAS failed\n");
      return false;
    }
    app.rt_blas_mannequin.push_back(b);
  }
  // City statics: exact-geometry BLASes (buildings, poles, heads, ground),
  // so TLAS shadows match what the depth map rasterizes. World-space
  // triangles were precomputed at part creation (CityPart::rt_triangles).
  for (const auto& part : app.city_parts) {
    ViewportApp::RtBlas b{};
    if (!rt_build_blas(app, part.rt_triangles, b)) {
      std::fprintf(stderr, "viewport: RT: city BLAS failed\n");
      return false;
    }
    app.rt_blas_city.push_back(b);
  }
  // Sponza landmark: one merged world-space BLAS (triangles baked in
  // setup_sponza); identity TLAS instance per frame.
  if (app.sponza_enabled && !app.rt_sponza_triangles.empty()) {
    ViewportApp::RtBlas b{};
    if (!rt_build_blas(app, app.rt_sponza_triangles, b)) {
      std::fprintf(stderr, "viewport: RT: sponza BLAS failed\n");
      return false;
    }
    app.rt_blas_sponza.push_back(b);
  }

  // --- 4. TLAS storage (capacity = worst-case instance count). ----------
  // City scene: 25 buildings + 32 poles/heads + 3 ground/static + actors.
  const std::uint32_t tlas_capacity =
      2U + app.mannequin_meshes.size() +
      static_cast<std::uint32_t>(app.city_parts.size()) +
      app.city_actor_count * app.mannequin_meshes.size() +
      static_cast<std::uint32_t>(app.rt_blas_sponza.size()) + 8U;
  auto tlas = app.rt_builder.create_tlas(dev, app.allocator, tlas_capacity);
  if (!tlas.is_ok()) {
    std::fprintf(stderr, "viewport: RT: create_tlas failed\n");
    return false;
  }
  app.rt_tlas = tlas.value();
  const auto tlas_sizes = omnicpp::render::VulkanAccelerationStructureBuilder::
      query_tlas_sizes(dev, tlas_capacity);
  app.rt_tlas_scratch_bytes = tlas_sizes.buildScratchSize;
  app.rt_instances.resize(tlas_capacity);

  // --- 5. Scratch pool (largest single build) + one-time BLAS builds. ---
  auto scratch = app.rt_scratch.acquire(app.allocator, dev,
                                        std::max(app.rt_blas_scratch_bytes,
                                                 app.rt_tlas_scratch_bytes));
  if (!scratch.is_ok()) {
    std::fprintf(stderr, "viewport: RT: scratch acquire failed\n");
    return false;
  }
  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      dev, static_cast<std::uint32_t>(
               app.context.queue_families().graphics_family));
  if (!pool.is_ok()) {
    std::fprintf(stderr, "viewport: RT: command pool failed\n");
    return false;
  }
  auto cb = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      dev, pool.value());
  if (!cb.is_ok()) {
    vkDestroyCommandPool(dev, pool.value(), nullptr);
    std::fprintf(stderr, "viewport: RT: command buffer failed\n");
    return false;
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb.value(), &bi);
  // One-time BLAS builds (order: build -> AS-write barrier -> next build).
  bool build_failed = false;
  auto build_one = [&](ViewportApp::RtBlas& b) {
    if (!app.rt_builder
             .cmd_build_blas(cb.value(), dev, b.as, b.input, scratch.value())
             .is_ok()) {
      return false;
    }
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                       VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(
        cb.value(), VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0,
        nullptr, 0, nullptr);
    return true;
  };
  for (auto& b : app.rt_blas_cube) {
    if (!build_one(b)) { build_failed = true; break; }
  }
  if (!build_failed) {
    for (auto& b : app.rt_blas_ground) {
      if (!build_one(b)) { build_failed = true; break; }
    }
  }
  if (!build_failed) {
    for (auto& b : app.rt_blas_mannequin) {
      if (!build_one(b)) { build_failed = true; break; }
    }
  }
  if (!build_failed) {
    for (auto& b : app.rt_blas_city) {
      if (!build_one(b)) { build_failed = true; break; }
    }
  }
  if (build_failed) {
    vkEndCommandBuffer(cb.value());
    vkDestroyCommandPool(dev, pool.value(), nullptr);
    std::fprintf(stderr, "viewport: RT: BLAS build record failed\n");
    return false;
  }
  vkEndCommandBuffer(cb.value());
  {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &cb.value();
    vkQueueSubmit(app.context.graphics_queue(), 1U, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(app.context.graphics_queue());
  }
  vkDestroyCommandPool(dev, pool.value(), nullptr);

  // --- 6. Bind the TLAS into set 4. --------------------------------------
  if (!app.descriptors
           .write_acceleration_structure(
               app.rt_set, 0U, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
               app.rt_tlas.handle, 0U)
           .is_ok()) {
    std::fprintf(stderr, "viewport: RT: write AS descriptor failed\n");
    return false;
  }

  // --- 7. Initial empty TLAS build so frame 0 ray queries hit a valid
  //        structure (the first frame's pre-pass fills real instances;
  //        frame 0 shades fully lit, one-frame latency like the shadow
  //        map). ---------------------------------------------------------
  auto pool0 = omnicpp::render::VulkanRenderer::create_command_pool(
      dev, static_cast<std::uint32_t>(
               app.context.queue_families().graphics_family));
  if (!pool0.is_ok()) {
    std::fprintf(stderr, "viewport: RT: pool0 failed\n");
    return false;
  }
  auto cb0 = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      dev, pool0.value());
  if (!cb0.is_ok()) {
    vkDestroyCommandPool(dev, pool0.value(), nullptr);
    std::fprintf(stderr, "viewport: RT: cb0 failed\n");
    return false;
  }
  VkCommandBufferBeginInfo bi0{};
  bi0.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi0.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb0.value(), &bi0);
  // Seed the TLAS with the ground slab (static model) so frame 0 ray
  // queries traverse a valid structure; the first frame's pre-pass replaces
  // it with the real instance set (one-frame latency, like the shadow map).
  omnicpp::render::TlasInstance seed{};
  {
    const SceneMatrix ground_model =
        multiply(translation_matrix(0.0f, -0.05f, 0.0f),
                 scale_matrix(8.0f, 0.1f, 8.0f));
    // Same column-major -> row-major conversion as build_rt_frame_tlas.
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        seed.transform[r * 4 + c] = ground_model[c * 4 + r];
      }
      seed.transform[r * 4 + 3] = ground_model[12 + r];
    }
    seed.instance_custom_index = 0U;
    seed.mask = 0xFFu;
    seed.sbt_offset = 0U;
    seed.flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    seed.blas_device_address = app.rt_blas_ground[0].as.device_address;
  }
  if (!app.rt_builder
           .cmd_build_tlas(cb0.value(), dev, app.rt_tlas, &seed, 1U,
                           scratch.value())
           .is_ok()) {
    vkEndCommandBuffer(cb0.value());
    vkDestroyCommandPool(dev, pool0.value(), nullptr);
    std::fprintf(stderr, "viewport: RT: empty TLAS build failed\n");
    return false;
  }
  vkEndCommandBuffer(cb0.value());
  {
    VkSubmitInfo submit0{};
    submit0.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit0.commandBufferCount = 1U;
    submit0.pCommandBuffers = &cb0.value();
    vkQueueSubmit(app.context.graphics_queue(), 1U, &submit0, VK_NULL_HANDLE);
    vkQueueWaitIdle(app.context.graphics_queue());
  }
  vkDestroyCommandPool(dev, pool0.value(), nullptr);
  return true;
}

//! Per-frame TLAS rebuild: instance transforms are the scene objects' model
//! matrices (extracted to 3x4 rows), written to the host instance buffer and
//! built on the frame's command buffer in the pre-pass hook (before the main
//! render pass). Rebuild (not refit) is preferred at our scale.
void build_rt_frame_tlas(ViewportApp& app, VkCommandBuffer command_buffer) {
  if (!app.rt_mode || app.rt_tlas.handle == nullptr) return;
  // Column-major SceneMatrix -> Vulkan row-major 3x4 instance transform:
  // element [r][c] = M(r,c) = m[c*4+r] for c in {0,1,2}, and the translation
  // column M(r,3) = m[12+r]. (Writing m[r*4+c] instead transposes the basis
  // and drops the translation — every instance would collapse to the
  // origin, shadowing everything the ray can reach.)
  const auto model_to_rows = [](const SceneMatrix& m, float* rows) {
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        rows[r * 4 + c] = m[c * 4 + r];
      }
      rows[r * 4 + 3] = m[12 + r];
    }
  };
  std::uint32_t count = 0;
  auto push_instance = [&](const ViewportApp::RtBlas& blas,
                           const SceneMatrix& model) {
    if (count >= app.rt_instances.size()) return;
    auto& inst = app.rt_instances[count++];
    model_to_rows(model, inst.transform);
    inst.instance_custom_index = count - 1U;
    inst.mask = 0xFFu;
    inst.sbt_offset = 0U;
    inst.flags =
        VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;  // hard-shadow occluders
    inst.blas_device_address = blas.as.device_address;
  };
  bool sponza_pushed = false;
  for (const auto& object : app.scene.objects) {
    if (object.mesh == nullptr || !object.mesh->is_drawable()) continue;
    if (object.mesh == &app.ground.mesh) {
      push_instance(app.rt_blas_ground[0], object.model);
    } else if (object.mesh == &app.cube.mesh) {
      push_instance(app.rt_blas_cube[0], object.model);
    } else {
      // Skinned parts (E5 animated TLAS): authored skinned vertices are
      // already in world bind pose, and the bone matrix B_j(t) =
      // global_j(t) * IB_j maps them directly to the posed world position.
      // Single-joint binding => the part's world transform IS
      // object_model * bones[j]: static BLASes + animated instance
      // transforms, no per-frame BLAS rebuild. Multi-actor bone arena:
      // the object's joint_base routes to its actor's slice.
      bool matched = false;
      if (!app.rt_blas_mannequin.empty()) {
        for (std::size_t i = 0; i < app.mannequin_meshes.size(); ++i) {
          if (object.mesh == &app.mannequin_meshes[i].mesh) {
            const auto* bones =
                static_cast<const SceneMatrix*>(app.bone_allocation.mapped);
            const std::size_t j = app.rt_part_joint[i];
            const std::size_t slot = object.joint_base + j;
            if (!app.rt_part_joint.empty() &&
                app.bone_allocation.mapped != nullptr &&
                slot * 64U < app.bone_allocation.size) {
              push_instance(app.rt_blas_mannequin[i],
                            multiply(object.model, bones[slot]));
            } else {
              push_instance(app.rt_blas_mannequin[i], object.model);
            }
            matched = true;
            break;
          }
        }
      }
      if (!matched) {
        // City statics: exact-geometry per-part BLASes whose triangles are
        // already WORLD-space (baked at part creation), so the instance
        // transform must be identity — applying object.model again would
        // double-transform every building.
        for (std::size_t c = 0; c < app.city_parts.size(); ++c) {
          if (object.mesh == &app.city_parts[c].buffers.mesh) {
            if (c < app.rt_blas_city.size()) {
              push_instance(app.rt_blas_city[c],
                            omnicpp::render::scene_identity_matrix());
            }
            matched = true;
            break;
          }
        }
      }
      if (!matched) {
        // Sponza landmark: ONE merged world-space BLAS covering all 103
        // primitives, identity transform. Pushed exactly once per frame —
        // per-object pushes would duplicate 103x and overflow the TLAS
        // instance capacity (verified: instances truncated at 96).
        if (!sponza_pushed && !app.rt_blas_sponza.empty()) {
          push_instance(app.rt_blas_sponza[0],
                        omnicpp::render::scene_identity_matrix());
          sponza_pushed = true;
        }
      }
    }
  }
  app.rt_last_instance_count = count;
  if (count == 0U) return;
  auto scratch = app.rt_scratch.acquire(app.allocator, app.context.device(),
                                        app.rt_tlas_scratch_bytes);
  if (!scratch.is_ok()) return;
  if (!app.rt_builder
           .cmd_build_tlas(command_buffer, app.context.device(), app.rt_tlas,
                           app.rt_instances.data(), count, scratch.value())
           .is_ok()) {
    return;
  }
  // AS-write -> fragment-stage ray-query read barrier (the main render pass
  // follows on the same command buffer).
  VkMemoryBarrier mb{};
  mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  vkCmdPipelineBarrier(command_buffer,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0,
                       nullptr, 0, nullptr);
}

void destroy_rt_shadows(ViewportApp& app) {
  VkDevice dev = app.context.device();
  if (dev == VK_NULL_HANDLE) return;
  auto destroy_list = [&](std::vector<ViewportApp::RtBlas>& list) {
    for (auto& b : list) {
      app.rt_builder.destroy_blas(dev, app.allocator, b.as);
    }
    list.clear();
  };
  destroy_list(app.rt_blas_cube);
  destroy_list(app.rt_blas_ground);
  destroy_list(app.rt_blas_mannequin);
  destroy_list(app.rt_blas_city);
  destroy_list(app.rt_blas_sponza);
  app.rt_builder.destroy_tlas(dev, app.allocator, app.rt_tlas);
  app.rt_scratch.cleanup(app.allocator);
  // City scene GPU resources (no-op when the city scene never ran).
  // Layouts (city_lights_layout) are owned by the descriptor manager and
  // released by its cleanup.
  if (app.city_lights_allocation.is_valid()) {
    app.allocator.destroy_allocation(app.city_lights_allocation);
    app.city_lights_allocation = {};
    app.city_lights_buffer = VK_NULL_HANDLE;
  }
  app.city_lights_layout = VK_NULL_HANDLE;
}

bool setup_lighting(ViewportApp& app) {
  // A/B: the legacy flag leaves the composed path unbuilt (lighting_ready
  // stays false so record_scene_into selects the legacy pipelines and never
  // registers the shadow pre-pass).
  if (app.legacy_lighting) return false;  // A/B selection, not a failure
  VkDevice dev = app.context.device();
  const std::uint32_t queue_family =
      static_cast<std::uint32_t>(app.context.queue_families().graphics_family);
  const char* shader_dir_env = std::getenv("OMNICPP_SHADER_DIR");
  const std::string shader_dir =
      shader_dir_env != nullptr ? shader_dir_env : "assets/shaders";
  std::string bake_error;

  // --- 1. IBL bake from the analytic sky. --------------------------------
  if (!app.ibl_baker.initialize(dev, app.context.graphics_queue(),
                                queue_family, app.allocator, app.descriptors,
                                shader_dir, bake_error)) {
    std::fprintf(stderr, "viewport: IBL bake unavailable (%s); legacy path\n",
                 bake_error.c_str());
    std::fprintf(stderr, "viewport: setup_lighting failed at line 925\n"); return false;
  }
  omnicpp::render::IblBakeParams bake_params{};
  bake_params.sun_direction = app.sun_direction;
  if (!app.ibl_baker.bake(bake_params)) {
    std::fprintf(stderr, "viewport: IBL bake failed; legacy path\n");
    std::fprintf(stderr, "viewport: setup_lighting failed at line 931\n"); return false;
  }

  // Set 5 IBL layout (bindings at 5.x for pbr_full.frag) written from the
  // baker's baked images.
  const std::vector<omnicpp::render::ReflectedBinding> ibl5_bindings = {
      {5U, 0U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {5U, 1U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {5U, 2U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto ibl5_layout = app.descriptors.create_layout(ibl5_bindings, 1U);
  if (!ibl5_layout.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 944\n"); return false; }
  app.ibl5_layout = ibl5_layout.value();
  auto ibl5_set = app.descriptors.allocate_set(app.ibl5_layout);
  if (!ibl5_set.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 947\n"); return false; }
  app.ibl5_set = ibl5_set.value();
  if (!app.descriptors
           .write_image(app.ibl5_set, 0U,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        app.ibl_baker.cube_sampler(),
                        app.ibl_baker.prefiltered_cube_view(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
      !app.descriptors
           .write_image(app.ibl5_set, 1U,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        app.ibl_baker.flat_sampler(),
                        app.ibl_baker.irradiance_cube_view(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok() ||
      !app.descriptors
           .write_image(app.ibl5_set, 2U,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        app.ibl_baker.flat_sampler(),
                        app.ibl_baker.brdf_lut_view(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    std::fprintf(stderr, "viewport: setup_lighting failed at line 970\n"); return false;
  }

  // --- 2. Shadow-map resources. ------------------------------------------
  VkImageCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  si.imageType = VK_IMAGE_TYPE_2D;
  si.format = VK_FORMAT_D32_SFLOAT;
  si.extent = {ViewportApp::kShadowRes, ViewportApp::kShadowRes, 1U};
  si.mipLevels = 1U;
  si.arrayLayers = 1U;
  si.samples = VK_SAMPLE_COUNT_1_BIT;
  si.tiling = VK_IMAGE_TILING_OPTIMAL;
  si.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (vkCreateImage(dev, &si, nullptr, &app.shadow_image) != VK_SUCCESS) {
    std::fprintf(stderr, "viewport: setup_lighting failed at line 986\n"); return false;
  }
  auto shadow_mem = app.allocator.bind_image(
      app.shadow_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!shadow_mem.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 990\n"); return false; }
  app.shadow_memory = shadow_mem.value();
  VkImageViewCreateInfo dvi{};
  dvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  dvi.image = app.shadow_image;
  dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  dvi.format = VK_FORMAT_D32_SFLOAT;
  dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U, 1U};
  if (vkCreateImageView(dev, &dvi, nullptr, &app.shadow_depth_view) !=
          VK_SUCCESS ||
      vkCreateImageView(dev, &dvi, nullptr, &app.shadow_sample_view) !=
          VK_SUCCESS) {
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1002\n"); return false;
  }
  // Compare-enabled sampler (the fragment stage declares sampler2DShadow in
  // pbr_shadow-style PCF paths; manual comparisons are not used here).
  VkSamplerCreateInfo sp{};
  sp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sp.magFilter = VK_FILTER_LINEAR;
  sp.minFilter = VK_FILTER_LINEAR;
  sp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  sp.compareEnable = VK_TRUE;
  sp.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  if (vkCreateSampler(dev, &sp, nullptr, &app.shadow_sampler) != VK_SUCCESS) {
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1018\n"); return false;
  }

  // Depth-only render pass (clear -> store, final DEPTH_READ for sampling).
  VkAttachmentDescription ad{};
  ad.format = VK_FORMAT_D32_SFLOAT;
  ad.samples = VK_SAMPLE_COUNT_1_BIT;
  ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ad.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  VkAttachmentReference dr{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sd{};
  sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sd.colorAttachmentCount = 0;
  sd.pDepthStencilAttachment = &dr;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &ad;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sd;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  if (vkCreateRenderPass(dev, &rpci, nullptr, &app.shadow_render_pass) !=
      VK_SUCCESS) {
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1053\n"); return false;
  }
  VkFramebufferCreateInfo fbi{};
  fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbi.renderPass = app.shadow_render_pass;
  fbi.attachmentCount = 1;
  fbi.pAttachments = &app.shadow_depth_view;
  fbi.width = ViewportApp::kShadowRes;
  fbi.height = ViewportApp::kShadowRes;
  fbi.layers = 1U;
  if (vkCreateFramebuffer(dev, &fbi, nullptr, &app.shadow_framebuffer) !=
      VK_SUCCESS) {
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1065\n"); return false;
  }

  // Shadow UBO (light VP) + set-4 descriptor (UBO + depth sampler).
  auto ubo = app.allocator.create_buffer(
      64U, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!ubo.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 1073\n"); return false; }
  app.shadow_ubo_allocation = ubo.value();
  const std::vector<omnicpp::render::ReflectedBinding> shadow_bindings = {
      {4U, 0U, 1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT},
      {4U, 1U, 1U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto shadow_layout = app.descriptors.create_layout(shadow_bindings, 2U);
  if (!shadow_layout.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 1081\n"); return false; }
  app.shadow_layout = shadow_layout.value();
  auto shadow_set = app.descriptors.allocate_set(app.shadow_layout);
  if (!shadow_set.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 1084\n"); return false; }
  app.shadow_set = shadow_set.value();
  if (!app.descriptors
           .write_buffer(app.shadow_set, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         app.shadow_ubo_allocation.buffer, 0U, 64U)
           .is_ok() ||
      !app.descriptors
           .write_image(app.shadow_set, 1U,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        app.shadow_sampler, app.shadow_sample_view,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0U)
           .is_ok()) {
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1096\n"); return false;
  }

  // --- 2b. Neutral shadow set (OMNICPP_NO_SHADOW). -----------------------
  // pbr_full.frag statically uses set 4, so it must remain bound even when
  // shadows are disabled — an unbound-but-used set is undefined behaviour.
  // A 1x1 depth image cleared to 1.0 makes every PCF comparison pass.
  {
    VkImageCreateInfo ni = si;
    ni.extent = {1U, 1U, 1U};
    if (vkCreateImage(dev, &ni, nullptr, &app.neutral_shadow_image) !=
        VK_SUCCESS) {
      std::fprintf(stderr, "viewport: setup_lighting failed at line 1108\n"); return false;
    }
    auto nmem = app.allocator.bind_image(
        app.neutral_shadow_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!nmem.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 1112\n"); return false; }
    app.neutral_shadow_memory = nmem.value();
    VkImageViewCreateInfo ndvi = dvi;
    ndvi.image = app.neutral_shadow_image;
    if (vkCreateImageView(dev, &ndvi, nullptr,
                          &app.neutral_shadow_sample_view) != VK_SUCCESS) {
      std::fprintf(stderr, "viewport: setup_lighting failed at line 1118\n"); return false;
    }
    auto neutral_set = app.descriptors.allocate_set(app.shadow_layout);
    if (!neutral_set.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 1121\n"); return false; }
    app.neutral_shadow_set = neutral_set.value();
    if (!app.descriptors
             .write_buffer(app.neutral_shadow_set, 0U,
                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           app.shadow_ubo_allocation.buffer, 0U, 64U)
             .is_ok() ||
        !app.descriptors
             .write_image(app.neutral_shadow_set, 1U,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                          app.shadow_sampler, app.neutral_shadow_sample_view,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 0U)
             .is_ok()) {
      std::fprintf(stderr, "viewport: setup_lighting failed at line 1134\n"); return false;
    }
    // Clear the 1x1 map to depth 1.0 (fully far = fully lit) once, on the
    // graphics queue, then leave it in the sampled layout permanently.
    auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
        dev, queue_family);
    if (!pool.is_ok()) { std::fprintf(stderr, "viewport: setup_lighting failed at line 1140\n"); return false; }
    auto cb = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        dev, pool.value());
    if (!cb.is_ok()) {
      vkDestroyCommandPool(dev, pool.value(), nullptr);
      std::fprintf(stderr, "viewport: setup_lighting failed at line 1145\n"); return false;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb.value(), &bi);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = app.neutral_shadow_image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U, 1U};
    vkCmdPipelineBarrier(cb.value(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkClearDepthStencilValue cd{1.0f, 0U};
    VkImageSubresourceRange range = {
        VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U, 1U};
    vkCmdClearDepthStencilImage(cb.value(), app.neutral_shadow_image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cd, 1,
                                &range);
    VkImageMemoryBarrier to_sample = barrier;
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb.value(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_sample);
    vkEndCommandBuffer(cb.value());
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &cb.value();
    vkQueueSubmit(app.context.graphics_queue(), 1U, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(app.context.graphics_queue());
    vkDestroyCommandPool(dev, pool.value(), nullptr);
  }

  // --- 3. Composed pipelines (pbr_full.frag). ----------------------------
  // pbr_full.frag declares sets 0,1,2,4,5; the skinned vertex stage adds
  // set 3. Both variants share one 6-entry layout array: the static one
  // simply never touches the bone layout declared at position 3 (an unused
  // declared set is legal; an undeclared gap is not).
  const VkDescriptorSetLayout full_layouts[6] = {
      app.mesh_layout, app.textures_layout, app.material_layout,
      app.bone_layout, app.shadow_layout, app.ibl5_layout};
  const VkPushConstantRange push_range{
      static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_VERTEX_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT),
      0U, 160U};

  if (!app.full_pipeline
           .load_shader_stage_file(dev, shader_dir + "/pbr_scene.vert.spv",
                                   "vertex")
           .is_ok() ||
      !app.full_pipeline
           .load_shader_stage_file(dev, shader_dir + "/pbr_full.frag.spv",
                                   "fragment")
           .is_ok() ||
      !app.full_pipeline
           .create_pipeline_layout(dev, full_layouts, 6U, &push_range)
           .is_ok() ||
      !app.full_pipeline
           .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                     app.swapchain.image_format(),
                                     app.full_pipeline.pipeline_layout(),
                                     /*depth_test=*/true, /*depth_write=*/true,
                                     /*cull=*/true)
           .is_ok()) {
    std::fprintf(stderr, "viewport: composed static pipeline failed\n");
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1219\n"); return false;
  }
  if (!app.full_skinned_pipeline
           .load_shader_stage_file(dev, shader_dir + "/skinned_scene.vert.spv",
                                   "vertex")
           .is_ok() ||
      !app.full_skinned_pipeline
           .load_shader_stage_file(dev, shader_dir + "/pbr_full.frag.spv",
                                   "fragment")
           .is_ok() ||
      !app.full_skinned_pipeline
           .create_pipeline_layout(dev, full_layouts, 6U, &push_range)
           .is_ok() ||
      !app.full_skinned_pipeline
           .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                     app.swapchain.image_format(),
                                     app.full_skinned_pipeline.pipeline_layout(),
                                     /*depth_test=*/true, /*depth_write=*/true,
                                     /*cull=*/true)
           .is_ok()) {
    std::fprintf(stderr, "viewport: composed skinned pipeline failed\n");
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1240\n"); return false;
  }

  // --- 4. Shadow-casting pipelines (depth-only). -------------------------
  const VkDescriptorSetLayout solo_mesh[1] = {app.mesh_layout};
  const VkPushConstantRange shadow_push{VK_SHADER_STAGE_VERTEX_BIT, 0U, 144U};
  if (!app.shadow_pipeline_static
           .load_shader_stage_file(dev, shader_dir + "/shadow.vert.spv",
                                   "vertex")
           .is_ok() ||
      !app.shadow_pipeline_static
           .load_shader_stage_file(dev, shader_dir + "/shadow.frag.spv",
                                   "fragment")
           .is_ok() ||
      !app.shadow_pipeline_static
           .create_pipeline_layout(dev, solo_mesh, 1U, &shadow_push)
           .is_ok() ||
      !app.shadow_pipeline_static
           .create_graphics_pipeline(dev, app.shadow_render_pass,
                                     VK_FORMAT_D32_SFLOAT,
                                     app.shadow_pipeline_static.pipeline_layout(),
                                     /*depth_test=*/true,
                                     /*depth_write=*/true, /*cull=*/false)
           .is_ok()) {
    std::fprintf(stderr, "viewport: static shadow pipeline failed\n");
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1265\n"); return false;
  }
  // --- 3b. Many-light city variant (OMNICPP_SCENE=city). ----------------
  // pbr_full_ml.frag adds the point-lights SSBO at set 6; the skinned
  // vertex stages only declare sets 0-3, so one 7-entry layout serves both
  // static and skinned draws. Same geometry/push contract as the composed
  // family; the record path picks the family, and the RT variants keep
  // set 4 = TLAS when rt_mode is on.
  if (app.city_scene) {
    const VkDescriptorSetLayout ml_layouts[7] = {
        app.mesh_layout, app.textures_layout, app.material_layout,
        app.bone_layout, app.shadow_layout, app.ibl5_layout,
        app.city_lights_layout};
    if (!app.full_ml_pipeline
             .load_shader_stage_file(dev, shader_dir + "/pbr_scene.vert.spv",
                                     "vertex")
             .is_ok() ||
        !app.full_ml_pipeline
             .load_shader_stage_file(dev,
                                     shader_dir + "/pbr_full_ml.frag.spv",
                                     "fragment")
             .is_ok() ||
        !app.full_ml_pipeline
             .create_pipeline_layout(dev, ml_layouts, 7U, &push_range)
             .is_ok() ||
        !app.full_ml_pipeline
             .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                       app.swapchain.image_format(),
                                       app.full_ml_pipeline.pipeline_layout(),
                                       true, true, true)
             .is_ok()) {
      std::fprintf(stderr, "viewport: many-light pipeline failed\n");
      return false;
    }
    if (app.has_mannequin &&
        (!app.full_ml_skinned_pipeline
                 .load_shader_stage_file(dev,
                                         shader_dir + "/skinned_scene.vert.spv",
                                         "vertex")
                 .is_ok() ||
         !app.full_ml_skinned_pipeline
                 .load_shader_stage_file(dev,
                                         shader_dir + "/pbr_full_ml.frag.spv",
                                         "fragment")
                 .is_ok() ||
         !app.full_ml_skinned_pipeline
                 .create_pipeline_layout(dev, ml_layouts, 7U, &push_range)
                 .is_ok() ||
         !app.full_ml_skinned_pipeline
                 .create_graphics_pipeline(
                     dev, app.render_pass.render_pass(),
                     app.swapchain.image_format(),
                     app.full_ml_skinned_pipeline.pipeline_layout(), true,
                     true, true)
                 .is_ok())) {
      std::fprintf(stderr, "viewport: many-light skinned pipeline failed\n");
      return false;
    }
    // NOTE: the RT-ML variants are built in setup_rt_shadows (they need
    // app.rt_layout, which exists only after the TLAS setup runs).
  }

  if (app.has_mannequin) {
    // shadow_skinned.vert declares bones at set 3, so the layout mirrors
    // the skinned_scene pipeline's first four slots (sets 1/2 unused).
    const VkDescriptorSetLayout skinned_solo[4] = {
        app.mesh_layout, app.textures_layout, app.material_layout,
        app.bone_layout};
    if (!app.shadow_pipeline_skinned
             .load_shader_stage_file(dev,
                                     shader_dir + "/shadow_skinned.vert.spv",
                                     "vertex")
             .is_ok() ||
        !app.shadow_pipeline_skinned
             .load_shader_stage_file(dev, shader_dir + "/shadow.frag.spv",
                                     "fragment")
             .is_ok() ||
        !app.shadow_pipeline_skinned
             .create_pipeline_layout(dev, skinned_solo, 4U, &shadow_push)
             .is_ok() ||
        !app.shadow_pipeline_skinned
             .create_graphics_pipeline(dev, app.shadow_render_pass,
                                       VK_FORMAT_D32_SFLOAT,
                                       app.shadow_pipeline_skinned.pipeline_layout(),
                                       /*depth_test=*/true,
                                       /*depth_write=*/true, /*cull=*/false)
             .is_ok()) {
      std::fprintf(stderr, "viewport: skinned shadow pipeline failed\n");
      std::fprintf(stderr, "viewport: setup_lighting failed at line 1293\n"); return false;
    }
  }
  return true;
}

// ============================================================================
// City scene (OMNICPP_SCENE=city): procedural street + multiple animated
// actors + many dynamic point lights. Replaces the single-actor scene's
// object list and camera framing when active.
// ============================================================================

// ============================================================================
// Sponza landmark (OMNICPP_SPONZA=1, city scene): whole-scene glTF import of
// the CC0 Sponza atrium (assets/models/sponza). One shared vertex/index
// buffer pair; one draw per glTF primitive as a SceneMesh index slice over
// the single mesh SSBO descriptor set (SceneMesh::index_offset/count already
// express per-primitive draws). Each primitive's material lands at
// kSponzaMaterialBase + primitive index in the shared material SSBO; albedo
// textures (69 external PNG/JPEG files, decoded by the importer) upload into
// the bindless array from kSponzaTextureBase. One merged world-space BLAS
// feeds ray-query shadows (identity TLAS instance).
// ============================================================================

constexpr std::uint32_t kSponzaMaterialBase = 64U;
constexpr std::uint32_t kSponzaTextureBase = 8U;

bool setup_sponza(ViewportApp& app) {
  // Resolve assets/models/sponza/Sponza.gltf (+ .bin + 69 external images
  // relative to the document dir), mirroring the mannequin loader.
  const char* asset_dir_env = std::getenv("OMNICPP_ASSET_DIR");
  std::vector<std::string> candidates;
  if (asset_dir_env != nullptr) candidates.emplace_back(asset_dir_env);
  candidates.insert(candidates.end(), {"assets/models", "../assets/models",
                                       "../../assets/models"});
  std::string json;
  std::vector<char> json_bytes;
  std::vector<char> bin_bytes;
  std::string model_dir;
  bool loaded = false;
  for (const auto& dir : candidates) {
    const std::string doc_path = dir + "/sponza/Sponza.gltf";
    if (!read_file_bytes(doc_path, json, json_bytes)) continue;
    model_dir = doc_path.substr(0, doc_path.find_last_of('/'));
    const std::string buffer_uri = find_buffer0_uri(json);
    if (!buffer_uri.empty() &&
        buffer_uri.compare(0, 5, "data:") != 0) {
      std::string bin_text;
      if (!read_file_bytes(model_dir + "/" + buffer_uri, bin_text,
                           bin_bytes)) {
        continue;
      }
    } else {
      bin_bytes.clear();
    }
    loaded = true;
    break;
  }
  if (!loaded) {
    std::fprintf(stderr, "viewport: sponza asset not found\n");
    return false;
  }

  // External image files resolve relative to the document dir.
  const omnicpp::asset::ExternalFileLoader loader =
      [&model_dir](const std::string& uri, std::string& load_error,
                   std::vector<std::uint8_t>& out_bytes) {
        std::vector<char> bytes;
        std::string unused;
        if (!read_file_bytes(model_dir + "/" + uri, unused, bytes)) {
          load_error = "cannot open " + uri;
          return false;
        }
        out_bytes.assign(bytes.begin(), bytes.end());
        return true;
      };

  std::string import_error;
  auto imported = omnicpp::asset::import_gltf_scene(
      json_bytes.data(), json_bytes.size(),
      bin_bytes.empty()
          ? nullptr
          : reinterpret_cast<const std::uint8_t*>(bin_bytes.data()),
      bin_bytes.size(), 0U, &import_error, &loader);
  if (!imported.is_ok()) {
    std::fprintf(stderr, "viewport: sponza import failed: %s\n",
                 import_error.c_str());
    return false;
  }
  auto& scene_import = imported.value();
  if (scene_import.meshes.empty() || scene_import.nodes.empty()) {
    std::fprintf(stderr, "viewport: sponza import empty\n");
    return false;
  }
  const auto& mesh = scene_import.meshes[0];

  auto* mats = static_cast<omnicpp::render::PbrMaterialData*>(
      app.material_allocation.mapped);

  // ---- Bindless albedo textures (one sampler; glTF baseColor is sRGB) ----
  VkDevice dev = app.context.device();
  const std::uint32_t queue_family = static_cast<std::uint32_t>(
      app.context.queue_families().graphics_family);

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  if (vkCreateSampler(dev, &sampler_info, nullptr, &app.sponza_sampler) !=
      VK_SUCCESS) {
    std::fprintf(stderr, "viewport: sponza sampler failed\n");
    return false;
  }

  app.sponza_textures.resize(mesh.images.size());
  {
    omnicpp::render::VulkanFrameUploadArena arena;
    if (!arena.initialize(dev, app.context.physical_device(), queue_family,
                          /*frame_count=*/1U,
                          /*bytes_per_frame=*/96U << 20U)
             .is_ok()) {
      std::fprintf(stderr, "viewport: sponza upload arena failed\n");
      return false;
    }
    if (!arena.begin_frame(0U).is_ok()) {
      arena.cleanup();
      return false;
    }
    bool upload_failed = false;
    for (std::size_t i = 0; i < mesh.images.size() && !upload_failed; ++i) {
      const auto& image = mesh.images[i];
      auto& out = app.sponza_textures[i];
      VkImageCreateInfo ii{};
      ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ii.imageType = VK_IMAGE_TYPE_2D;
      ii.format = VK_FORMAT_R8G8B8A8_SRGB;  // baseColor: hardware linearises
      ii.extent = {image.width, image.height, 1U};
      ii.mipLevels = 1U;
      ii.arrayLayers = 1U;
      ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = VK_IMAGE_TILING_OPTIMAL;
      ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;
      ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (vkCreateImage(dev, &ii, nullptr, &out.image) != VK_SUCCESS) {
        upload_failed = true;
        break;
      }
      auto memory = app.allocator.bind_image(
          out.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!memory.is_ok()) {
        upload_failed = true;
        break;
      }
      out.allocation = memory.value();
      const VkDeviceSize byte_count =
          static_cast<VkDeviceSize>(image.width) * image.height * 4U;
      auto span = arena.acquire(byte_count);
      if (!span.is_ok()) {
        upload_failed = true;
        break;
      }
      std::memcpy(span.value().host_data, image.rgba.data(),
                  static_cast<std::size_t>(byte_count));
      arena.record_copy_image_rgba8(span.value(), out.image, image.width,
                                    image.height);

      VkImageViewCreateInfo vi{};
      vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vi.image = out.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = VK_FORMAT_R8G8B8A8_SRGB;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
      if (vkCreateImageView(dev, &vi, nullptr, &out.scene.view) !=
          VK_SUCCESS) {
        upload_failed = true;
        break;
      }
      out.scene.sampler = app.sponza_sampler;
      out.scene.bindless_index =
          kSponzaTextureBase + static_cast<std::uint32_t>(i);
      if (!app.descriptors
               .write_image(app.textures_set, 0U,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                            app.sponza_sampler, out.scene.view,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            out.scene.bindless_index)
               .is_ok()) {
        upload_failed = true;
      }
    }
    if (upload_failed) {
      arena.wait_idle();
      arena.cleanup();
      std::fprintf(stderr, "viewport: sponza texture upload failed\n");
      return false;
    }
    if (!arena.submit(app.context.graphics_queue()).is_ok()) {
      arena.cleanup();
      std::fprintf(stderr, "viewport: sponza texture submit failed\n");
      return false;
    }
    arena.wait_idle();
    arena.cleanup();
  }

  // ---- Shared geometry buffers + one mesh SSBO descriptor set. ----------
  // The city ML pipeline's vertex stage (skinned_scene.vert) reads an
  // unconditional 19-float combined stream [11 geometry][8 skin], so static
  // geometry must carry the identity skin payload (joints=0 -> bone slot 0,
  // weights 1,0,0,0) — the same trick as the procedural city statics.
  std::vector<float> combined_vertices;
  {
    const std::size_t vertex_count = mesh.vertex_count();
    combined_vertices.reserve(mesh.vertices.size() + vertex_count * 8U);
    combined_vertices.insert(combined_vertices.end(), mesh.vertices.begin(),
                             mesh.vertices.end());
    for (std::size_t v = 0; v < vertex_count; ++v) {
      combined_vertices.insert(combined_vertices.end(),
                               {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                0.0f});
    }
  }
  const VkDeviceSize vertex_bytes =
      static_cast<VkDeviceSize>(combined_vertices.size()) * sizeof(float);
  const VkDeviceSize index_bytes =
      static_cast<VkDeviceSize>(mesh.indices.size()) * sizeof(std::uint32_t);
  auto vb = app.allocator.create_buffer(
      vertex_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto ib = app.allocator.create_buffer(
      index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!vb.is_ok() || !ib.is_ok()) return false;
  app.sponza.vertex_allocation = vb.value();
  app.sponza.index_allocation = ib.value();
  std::memcpy(app.sponza.vertex_allocation.mapped, combined_vertices.data(),
              static_cast<std::size_t>(vertex_bytes));
  std::memcpy(app.sponza.index_allocation.mapped, mesh.indices.data(),
              static_cast<std::size_t>(index_bytes));

  auto mesh_set = app.descriptors.allocate_set(app.mesh_layout);
  if (!mesh_set.is_ok()) return false;
  app.sponza.mesh_set = mesh_set.value();
  if (!app.descriptors
           .write_buffer(app.sponza.mesh_set, 0U,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         app.sponza.vertex_allocation.buffer, 0U,
                         VK_WHOLE_SIZE)
           .is_ok()) {
    return false;
  }

  // ---- Per-primitive SceneMesh slices + materials + BLAS triangles. ----- 
  app.sponza.parts.resize(mesh.primitives.size());
  app.sponza.part_models.resize(mesh.primitives.size());
  app.sponza.material_base = kSponzaMaterialBase;
  // glTF has no metallic/roughness factors wired through the importer; the
  // atrium is dielectric stone.
  for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
    const auto& prim = mesh.primitives[p];
    auto& part = app.sponza.parts[p];
    part.vertex_buffer = app.sponza.vertex_allocation.buffer;
    part.index_buffer = app.sponza.index_allocation.buffer;
    part.descriptor_set = app.sponza.mesh_set;
    part.index_offset =
        static_cast<VkDeviceSize>(prim.index_offset) * sizeof(std::uint32_t);
    part.index_count = static_cast<std::uint32_t>(prim.index_count);

    auto& mat = mats[kSponzaMaterialBase + static_cast<std::uint32_t>(p)];
    mat = {};
    mat.base_color_factor = {prim.base_color[0], prim.base_color[1],
                             prim.base_color[2], prim.base_color[3]};
    mat.metallic_factor = 0.0f;
    mat.roughness_factor = 0.9f;
    if (prim.albedo.present &&
        prim.albedo.image_index < mesh.images.size()) {
      mat.flags = static_cast<std::uint32_t>(
          omnicpp::render::PbrMaterialFlags::kHasAlbedo);
      mat.albedo_index = app.sponza_textures[prim.albedo.image_index]
                             .scene.bindless_index;
    }
  }

  // Node model (column-major, scale 0.008 baked) shared by every primitive
  // (one mesh-bearing node in the document); vertices stay LOCAL so the
  // model belongs on both the draw and the RT bake (unlike the procedural
  // city parts, whose triangles are pre-baked world-space).
  if (scene_import.nodes.empty()) {
    std::fprintf(stderr, "viewport: sponza import has no mesh node\n");
    return false;
  }
  const auto& nm = scene_import.nodes[0].model;
  app.rt_sponza_triangles.reserve(
      static_cast<std::size_t>(mesh.indices.size()) * 3U * 3U);
  for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
    app.sponza.part_models[p] =
        [&] {
          SceneMatrix m{};
          for (int i = 0; i < 16; ++i) m[i] = nm[i];
          return m;
        }();
    const auto& prim = mesh.primitives[p];
    for (std::size_t i = prim.index_offset;
         i + 2U < prim.index_offset + prim.index_count; i += 3U) {
      for (int k = 0; k < 3; ++k) {
        // Stride 19: the combined stream is [11 geometry][8 skin] per vertex
        // after the identity-payload append; positions lead each record.
        const std::size_t b =
            static_cast<std::size_t>(mesh.indices[i + k]) * 19U;
        const float x = combined_vertices[b + 0];
        const float y = combined_vertices[b + 1];
        const float z = combined_vertices[b + 2];
        app.rt_sponza_triangles.push_back(nm[0] * x + nm[4] * y +
                                          nm[8] * z + nm[12]);
        app.rt_sponza_triangles.push_back(nm[1] * x + nm[5] * y +
                                          nm[9] * z + nm[13]);
        app.rt_sponza_triangles.push_back(nm[2] * x + nm[6] * y +
                                          nm[10] * z + nm[14]);
      }
    }
  }
  app.sponza_triangle_count = static_cast<std::uint32_t>(
      app.rt_sponza_triangles.size() / 9U);
  return true;
}

bool setup_city_scene(ViewportApp& app) {
  if (!app.has_mannequin || app.mannequin_meshes.empty()) {
    std::fprintf(stderr, "viewport: city scene requires the mannequin asset\n");
    return false;
  }
  if (app.mannequin.skins.empty()) return false;
  VkDevice dev = app.context.device();
  const char* shader_dir_env = std::getenv("OMNICPP_SHADER_DIR");
  const std::string shader_dir =
      shader_dir_env != nullptr ? shader_dir_env : "assets/shaders";

  // ------------------------------------------------------------------
  // Lights SSBO (set 6): 32 warm point lights along both streets.
  // ------------------------------------------------------------------
  app.city_lights.clear();
  app.city_lights.reserve(32U);
  for (int s = 0; s < 2; ++s) {
    for (int i = 0; i < 16; ++i) {
      ViewportApp::CityLight light{};
      light.pos[0] = -42.0f + 5.6f * static_cast<float>(i);
      light.pos[1] = 6.2f;
      light.pos[2] = s == 0 ? -6.0f : 6.0f;
      light.radius = 11.0f;
      // Alternating sodium/cool tints, deterministic.
      light.color[0] = (i % 2 == 0) ? 1.0f : 0.75f;
      light.color[1] = (i % 2 == 0) ? 0.72f : 0.85f;
      light.color[2] = (i % 2 == 0) ? 0.42f : 1.0f;
      light.intensity = 6.0f;
      app.city_lights.push_back(light);
    }
  }
  constexpr std::size_t kLightWords = 4U + 32U * 8U;  // header + 32x2 vec4
  auto lights_buffer = app.allocator.create_buffer(
      kLightWords * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!lights_buffer.is_ok()) return false;
  app.city_lights_allocation = lights_buffer.value();
  app.city_lights_buffer = app.city_lights_allocation.buffer;
  auto* light_words = static_cast<std::uint32_t*>(
      app.city_lights_allocation.mapped);
  light_words[0] = static_cast<std::uint32_t>(app.city_lights.size());
  light_words[1] = 0U;
  light_words[2] = 0U;
  light_words[3] = 0U;
  float* light_floats = reinterpret_cast<float*>(light_words + 4U);
  for (std::size_t i = 0; i < app.city_lights.size(); ++i) {
    const auto& l = app.city_lights[i];
    light_floats[i * 8U + 0] = l.pos[0];
    light_floats[i * 8U + 1] = l.pos[1];
    light_floats[i * 8U + 2] = l.pos[2];
    light_floats[i * 8U + 3] = l.radius;
    light_floats[i * 8U + 4] = l.color[0];
    light_floats[i * 8U + 5] = l.color[1];
    light_floats[i * 8U + 6] = l.color[2];
    light_floats[i * 8U + 7] = l.intensity;
  }

  // Lights descriptor set (layout mirrors the engine's lights SSBO shape;
  // binding 0, storage buffer, fragment stage).
  const std::vector<omnicpp::render::ReflectedBinding> lights_bindings = {
      {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto lights_layout =
      app.descriptors.create_layout(lights_bindings, 4U);
  if (!lights_layout.is_ok()) return false;
  app.city_lights_layout = lights_layout.value();
  auto lights_set = app.descriptors.allocate_set(app.city_lights_layout);
  if (!lights_set.is_ok()) return false;
  app.city_lights_set = lights_set.value();
  if (!app.descriptors
           .write_buffer(app.city_lights_set, 0U,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         app.city_lights_buffer, 0U, VK_WHOLE_SIZE)
           .is_ok()) {
    return false;
  }

  // ------------------------------------------------------------------
  // Skinning arena: slot 0 = identity bone (static meshes), actors at
  // [1 + a * joints_per_actor). One bone SSBO serves all actors; per-object
  // joint_base routes each draw to its actor's joint slice.
  // ------------------------------------------------------------------
  app.joints_per_actor =
      static_cast<std::uint32_t>(app.mannequin.skins[0].joints.size());
  app.city_actor_count = 3U;
  const std::uint32_t bone_slots =
      1U + app.city_actor_count * app.joints_per_actor;
  // Grow the single-actor bone buffer to the multi-actor arena.
  if (app.bone_allocation.is_valid()) {
    app.allocator.destroy_allocation(app.bone_allocation);
    app.bone_allocation = {};
  }
  auto arena = app.allocator.create_buffer(
      static_cast<VkDeviceSize>(bone_slots) * 64U,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!arena.is_ok()) return false;
  app.bone_allocation = arena.value();
  auto bone_set = app.descriptors.allocate_set(app.bone_layout);
  if (!bone_set.is_ok()) return false;
  app.bone_set = bone_set.value();
  if (!app.descriptors
           .write_buffer(app.bone_set, 0U,
                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         app.bone_allocation.buffer, 0U, VK_WHOLE_SIZE)
           .is_ok()) {
    return false;
  }
  auto* bones = static_cast<SceneMatrix*>(app.bone_allocation.mapped);
  bones[0] = omnicpp::render::scene_identity_matrix();  // static slot

  // ------------------------------------------------------------------
  // Static geometry (identity skin payload: joints=0, weights=1,0,0,0,
  // joint_base=0 -> bone slot 0 = identity).
  // ------------------------------------------------------------------
  std::mt19937 rng(20260911U);
  auto ground_mesh = [&](float w, float d) {
    std::vector<float> verts;
    std::vector<std::uint32_t> idx;
    const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const float corners[4][3] = {{-w, 0, -d}, {w, 0, -d}, {w, 0, d}, {-w, 0, d}};
    const std::uint32_t quad[6] = {0, 2, 1, 0, 3, 2};
    for (int i = 0; i < 4; ++i) {
      verts.insert(verts.end(),
                   {corners[i][0], corners[i][1], corners[i][2],
                    1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, uvs[i][0], uvs[i][1]});
    }
    for (std::uint32_t i : quad) idx.push_back(i);
    return std::make_pair(std::move(verts), std::move(idx));
  };
  auto box_mesh = [&](float sx, float sy, float sz) {
    std::vector<float> verts;
    std::vector<std::uint32_t> idx;
    struct Face {
      float n[3];
      float c[4][3];
    };
    const Face faces[6] = {
        {{0, 0, 1}, {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}},
        {{0, 0, -1}, {{-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}, {1, -1, -1}}},
        {{1, 0, 0}, {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}}},
        {{-1, 0, 0}, {{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}}},
        {{0, 1, 0}, {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}},
        {{0, -1, 0}, {{-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}}},
    };
    const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const std::uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
    for (const Face& f : faces) {
      const std::uint32_t base =
          static_cast<std::uint32_t>(verts.size() / 11U);
      for (int i = 0; i < 4; ++i) {
        verts.insert(verts.end(),
                     {f.c[i][0] * sx, f.c[i][1] * sy, f.c[i][2] * sz,
                      1.0f, 1.0f, 1.0f, f.n[0], f.n[1], f.n[2],
                      uvs[i][0], uvs[i][1]});
      }
      for (std::uint32_t i : quad) idx.push_back(base + i);
    }
    return std::make_pair(std::move(verts), std::move(idx));
  };
  auto append_skin_identity = [](std::vector<float>& verts) {
    const std::size_t vc = verts.size() / 11U;
    for (std::size_t v = 0; v < vc; ++v) {
      verts.push_back(0.0f);  // joint 0 (identity bone slot)
      verts.push_back(0.0f);
      verts.push_back(0.0f);
      verts.push_back(0.0f);
      verts.push_back(1.0f);  // weight fully on joint 0
      verts.push_back(0.0f);
      verts.push_back(0.0f);
      verts.push_back(0.0f);
    }
  };

  auto make_city_part = [&](std::vector<float> verts,
                            const std::vector<std::uint32_t>& indices,
                            std::uint32_t material, const SceneMatrix& model,
                            ViewportApp::CityPart& out) {
    append_skin_identity(verts);
    if (!make_mesh(app, verts, indices, out.buffers)) return false;
    out.material_index = material;
    out.model = model;
    return true;
  };
  // World-space triangles for the exact-geometry BLAS (append after model
  // composition): the local triangle list transformed by the part model.
  auto fill_rt_triangles = [&](const std::vector<float>& local_verts,
                               const std::vector<std::uint32_t>& indices,
                               const SceneMatrix& model,
                               ViewportApp::CityPart& out) {
    out.rt_triangles.reserve(indices.size() / 3U * 9U);
    for (std::size_t i = 0; i + 2U < indices.size(); i += 3U) {
      for (int k = 0; k < 3; ++k) {
        const std::size_t b = static_cast<std::size_t>(indices[i + k]) * 11U;
        const float x = local_verts[b + 0], y = local_verts[b + 1],
                    z = local_verts[b + 2];
        // SceneMatrix is column-major: world = model * (x,y,z,1).
        out.rt_triangles.push_back(model[0] * x + model[4] * y +
                                   model[8] * z + model[12]);
        out.rt_triangles.push_back(model[1] * x + model[5] * y +
                                   model[9] * z + model[13]);
        out.rt_triangles.push_back(model[2] * x + model[6] * y +
                                   model[10] * z + model[14]);
      }
    }
  };

  auto* mats = static_cast<omnicpp::render::PbrMaterialData*>(
      app.material_allocation.mapped);

  // Ground: one 100x40 slab covering the full street grid.
  {
    auto g = ground_mesh(50.0f, 20.0f);
    ViewportApp::CityPart part{};
    if (!make_city_part(g.first, g.second, 2U,
                        omnicpp::render::scene_identity_matrix(), part)) {
      return false;
    }
    fill_rt_triangles(g.first, g.second,
                      omnicpp::render::scene_identity_matrix(), part);
    app.city_parts.push_back(std::move(part));
  }

  // Buildings: 2 rows of 8, subdivided street grid (seeded jitter).
  std::uniform_real_distribution<float> hue(0.0f, 1.0f);
  for (int row = 0; row < 2; ++row) {
    for (int i = 0; i < 8; ++i) {
      const float bx = -42.0f + 12.0f * static_cast<float>(i);
      const float bz = row == 0 ? -14.0f : 14.0f;
      const float w = 4.5f + hue(rng) * 1.5f;
      const float h = 6.0f + hue(rng) * 14.0f;
      const float d = 4.5f + hue(rng) * 1.5f;
      auto b = box_mesh(w, h, d);
      ViewportApp::CityPart part{};
      const SceneMatrix model =
          multiply(translation_matrix(bx, h * 0.5f, bz),
                   omnicpp::render::scene_identity_matrix());
      if (!make_city_part(b.first, b.second,
                          10U + static_cast<std::uint32_t>(row * 8 + i),
                          model, part)) {
        return false;
      }
      fill_rt_triangles(b.first, b.second, model, part);
      app.city_parts.push_back(std::move(part));
      // Building material: grey concrete with seeded hue variation.
      mats[10U + static_cast<std::uint32_t>(row * 8 + i)] = {};
      mats[10U + row * 8 + i].base_color_factor = {
          0.42f + 0.18f * hue(rng), 0.44f + 0.14f * hue(rng),
          0.48f + 0.16f * hue(rng), 1.0f};
      mats[10U + row * 8 + i].metallic_factor = 0.0f;
      mats[10U + row * 8 + i].roughness_factor = 0.85f;
      // Streetlight pole + head under each building street face.
      auto pole = box_mesh(0.12f, 5.2f, 0.12f);
      ViewportApp::CityPart pole_part{};
      const float px = bx + 5.0f;
      const float pz = row == 0 ? -6.8f : 6.8f;
      const SceneMatrix pole_model =
          multiply(translation_matrix(px, 2.6f, pz),
                   omnicpp::render::scene_identity_matrix());
      if (!make_city_part(pole.first, pole.second, 26U, pole_model,
                          pole_part)) {
        return false;
      }
      fill_rt_triangles(pole.first, pole.second, pole_model, pole_part);
      app.city_parts.push_back(std::move(pole_part));
      auto head = box_mesh(0.7f, 0.22f, 0.35f);
      ViewportApp::CityPart head_part{};
      const SceneMatrix head_model =
          multiply(translation_matrix(px, 5.35f, pz),
                   omnicpp::render::scene_identity_matrix());
      if (!make_city_part(head.first, head.second, 27U, head_model,
                          head_part)) {
        return false;
      }
      fill_rt_triangles(head.first, head.second, head_model, head_part);
      app.city_parts.push_back(std::move(head_part));
    }
  }
  // Shared static materials: pole (26) dark steel, head (27) emissive.
  // Slots 10..25 belong to the 16 buildings — the old 11/12 assignment
  // clobbered building materials 1 and 2 (slot-collision bug).
  mats[26] = {};
  mats[26].base_color_factor = {0.16f, 0.17f, 0.19f, 1.0f};
  mats[26].metallic_factor = 0.85f;
  mats[26].roughness_factor = 0.45f;
  mats[27] = {};
  mats[27].base_color_factor = {1.0f, 0.87f, 0.6f, 1.0f};
  mats[27].emissive_factor = {6.0f, 5.0f, 3.2f};
  mats[27].roughness_factor = 0.6f;

  // Actors: 3 walking mannequins (shared meshes, per-actor joint slices in
  // the shared bone SSBO; parts expand per actor in record_scene_into).
  for (std::uint32_t a = 0; a < app.city_actor_count; ++a) {
    const float ax = -6.0f + 6.0f * static_cast<float>(a);
    const float az = a == 1 ? 2.5f : (a == 0 ? -2.5f : 0.0f);
    app.city_actors.push_back({translation_matrix(ax, 0.05f, az),
                               static_cast<float>(a) * 0.37f});
  }

  // Sponza landmark (OMNICPP_SPONZA=1): CC0 atrium centred at the origin,
  // actors walking its courtyard. Local-space vertices + per-draw model;
  // world-space BLAS triangles were baked inside setup_sponza.
  if (app.sponza_enabled && !setup_sponza(app)) {
    std::fprintf(stderr,
                 "viewport: sponza landmark setup failed; continuing without\n");
    app.sponza_enabled = false;
  }
  return true;
}

//! Renderer pre-pass hook: renders the shadow map (static + skinned objects
//! in one depth-only pass) before the main render pass. The main pass then
//! samples it via scene.shadow_set with the SAME light VP the pre-pass used
//! (stashed in scene.shadow_light_vp inside record_scene_into, which runs
//! first within the frame).
bool shadow_pre_pass_cb(VkCommandBuffer command_buffer, std::uint32_t width,
                        std::uint32_t                        height, void* user_data) {
  auto& app = *static_cast<ViewportApp*>(user_data);

  // GPU-driven cull/LOD pass: compute (outside any render pass) writes this
  // frame's indirect draw commands from the payload slot for the in-flight
  // frame. The main pass consumes them with ONE vkCmdDrawIndexedIndirect.
  if (app.gpu_driven) {
    const std::uint32_t gd_slot = app.renderer.current_frame();
    write_gpu_driven_payload(app, gd_slot, app.time, height);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      app.gd_cull_pipeline.pipeline());
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            app.gd_cull_pipeline.pipeline_layout(), 0U, 1U,
                            &app.gd_cull_sets[gd_slot], 0U, nullptr);
    vkCmdPushConstants(command_buffer, app.gd_cull_pipeline.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                       app.gd_cull_push_staging.size(),
                       app.gd_cull_push_staging.data());
    vkCmdDispatch(command_buffer, (app.gd_instance_count + 63U) / 64U, 1U, 1U);
    VkBufferMemoryBarrier bb{};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                       VK_ACCESS_SHADER_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = app.gd_indirect_buffer;
    bb.offset = 0U;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0U, 0U, nullptr, 1U, &bb, 0U, nullptr);
  }

  if (!app.lighting_ready) return true;  // nothing to pre-render
  (void)width;
  (void)height;

  // RT mode: rebuild the frame's TLAS (instance transforms = the scene
  // objects' models recorded last frame — one frame of latency, exactly
  // like the shadow map) and skip the shadow-map render pass entirely. AS
  // builds must record outside a render pass, so this pre-pass hook is the
  // sanctioned spot. Not combined with GPU-driven (the driven fragment is
  // the PCF variant); gpu_driven takes precedence.
  if (app.rt_mode && !app.gpu_driven) {
    build_rt_frame_tlas(app, command_buffer);
    return true;
  }

  VkClearValue shadow_clear{};
  shadow_clear.depthStencil = {1.0f, 0U};
  VkRenderPassBeginInfo rpb{};
  rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpb.renderPass = app.shadow_render_pass;
  rpb.framebuffer = app.shadow_framebuffer;
  rpb.renderArea.extent = {ViewportApp::kShadowRes, ViewportApp::kShadowRes};
  rpb.clearValueCount = 1;
  rpb.pClearValues = &shadow_clear;
  vkCmdBeginRenderPass(command_buffer, &rpb, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.width = static_cast<float>(ViewportApp::kShadowRes);
  viewport.height = static_cast<float>(ViewportApp::kShadowRes);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = {ViewportApp::kShadowRes, ViewportApp::kShadowRes};
  vkCmdSetScissor(command_buffer, 0, 1, &scissor);

  struct ShadowPush {
    omnicpp::render::SceneMatrix light_vp;
    omnicpp::render::SceneMatrix model;
    std::uint32_t joint_base;
    std::uint32_t pad[3];
  } push{};
  push.light_vp = app.scene.shadow_light_vp;

  for (const auto& object : app.scene.objects) {
    if (object.mesh == nullptr || !object.mesh->is_drawable()) continue;
    const bool skinned = app.has_mannequin &&
                         object.mesh != &app.ground.mesh;
    const omnicpp::render::VulkanPipeline& pipe =
        skinned ? app.shadow_pipeline_skinned : app.shadow_pipeline_static;
    push.model = object.model;
    push.joint_base = object.joint_base;
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipe.pipeline());
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipe.pipeline_layout(), 0, 1,
                            &object.mesh->descriptor_set, 0, nullptr);
    if (skinned) {
      vkCmdBindDescriptorSets(command_buffer,
                              VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipe.pipeline_layout(), 3, 1, &app.bone_set, 0,
                              nullptr);
    }
    vkCmdPushConstants(command_buffer, pipe.pipeline_layout(),
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(command_buffer, object.mesh->index_buffer,
                         object.mesh->index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(command_buffer, object.mesh->index_count, 1, 0, 0, 0);
  }
  vkCmdEndRenderPass(command_buffer);
  return true;
}

//! Build the scene description for sim time `t` / walk phase `walk_t` and
//! record it into a begun render pass. Pure: mutates nothing on `app`, so
//! the window path and the capture path can record the identical scene.
bool record_scene_into(VkCommandBuffer command_buffer, ViewportApp& app,
                       float t, float walk_t, std::uint32_t width,
                       std::uint32_t height) {
  const float orbit_base = std::isnan(app.run_config.camera_radius)
                               ? (app.has_mannequin ? 3.2f : 6.5f)
                               : app.run_config.camera_radius;
  const float height_default = std::isnan(app.run_config.camera_height)
                                   ? (app.has_mannequin ? 1.6f : 3.2f)
                                   : app.run_config.camera_height;
  // Scripted/human input adjusts the camera: orbit_left/right add angular
  // bias, zoom_in/out change radius at 1.5 units/s. Pure function of state
  // so the capture path sees the same camera.
  const float orbit_angle =
      t * 0.25f + app.camera_orbit_bias * t;
  const float orbit_radius =
      std::min(std::max(orbit_base + app.camera_radius_bias, 1.2f), 12.0f);
  const float eye[3] = {orbit_radius * std::cos(orbit_angle), height_default,
                        orbit_radius * std::sin(orbit_angle)};
  const float target[3] = {0.0f, app.has_mannequin ? 0.9f : 0.8f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  app.scene.camera.view_projection = omnicpp::render::scene_camera_view_projection(
      eye, target, up, 1.05f,
      static_cast<float>(width) / static_cast<float>(height), 0.1f, 100.0f);
  // Camera position in view conventions: the look_at eye.
  app.scene.camera_position = {eye[0], eye[1], eye[2], 1.0f};

  // Light VP for the shadow pre-pass (the hook records with the same matrix
  // stashed here) and for the fragment stage's shadow lookup. Ortho box
  // covers the ~16-unit scene; the light eye sits 8 units along the sun
  // direction looking at the origin.
  const float light_eye[3] = {app.sun_direction[0] * 8.0f,
                              app.sun_direction[1] * 8.0f,
                              app.sun_direction[2] * 8.0f};
  const float light_target[3] = {0.0f, 0.0f, 0.0f};
  app.scene.shadow_light_vp = multiply(
      make_ortho(-10.0f, 10.0f, -10.0f, 10.0f, -20.0f, 20.0f),
      omnicpp::render::scene_camera_look_at(light_eye, light_target, up));
  if (app.shadow_ubo_allocation.mapped != nullptr) {
    std::memcpy(app.shadow_ubo_allocation.mapped,
                app.scene.shadow_light_vp.data(), 64U);
  }

  app.scene.objects.clear();

  // ------------------------------------------------------------------------
  // City scene: street grid + 3 walking actors + 32 dynamic point lights.
  // Own camera framing (high overview) and light box; the ML pipeline family
  // binds the lights SSBO at set 6 (RT variant: TLAS at set 4, no shadow map).
  // ------------------------------------------------------------------------
  if (app.city_scene) {
    // Camera: slow orbiting overview. With the Sponza landmark the orbit
    // rises ABOVE the atrium (walls reach ~11m and span |x|<=16m, so the
    // 16m-radius street orbit would put the lens inside the walls).
    const float orbit_radius = app.sponza_enabled ? 34.0f : 16.0f;
    const float orbit_height = app.sponza_enabled ? 22.0f : 14.0f;
    const float cam_angle = t * 0.05f;
    // Control-channel override (set_camera command) replaces the orbit.
    float eye[3];
    float target[3];
    if (app.camera_override_) {
      eye[0] = app.camera_eye_[0];
      eye[1] = app.camera_eye_[1];
      eye[2] = app.camera_eye_[2];
      target[0] = app.camera_target_[0];
      target[1] = app.camera_target_[1];
      target[2] = app.camera_target_[2];
    } else {
      eye[0] = orbit_radius * std::cos(cam_angle);
      eye[1] = orbit_height;
      eye[2] = orbit_radius * std::sin(cam_angle);
      target[0] = 0.0f;
      target[1] = 1.0f;
      target[2] = 0.0f;
    }
    const float up[3] = {0.0f, 1.0f, 0.0f};
    app.scene.camera.view_projection =
        omnicpp::render::scene_camera_view_projection(
            eye, target, up, 1.05f,
            static_cast<float>(width) / static_cast<float>(height), 0.5f,
            300.0f);
    app.scene.camera_position = {eye[0], eye[1], eye[2], 1.0f};
    // Sun ortho box covering the street grid.
    const float light_eye[3] = {app.sun_direction[0] * 30.0f,
                                app.sun_direction[1] * 30.0f,
                                app.sun_direction[2] * 30.0f};
    app.scene.shadow_light_vp = multiply(
        make_ortho(-55.0f, 55.0f, -55.0f, 55.0f, -60.0f, 60.0f),
        omnicpp::render::scene_camera_look_at(light_eye, target, up));
    if (app.shadow_ubo_allocation.mapped != nullptr) {
      std::memcpy(app.shadow_ubo_allocation.mapped,
                  app.scene.shadow_light_vp.data(), 64U);
    }

    // Per-actor pose update into the shared bone arena (slot 0 = identity;
    // actor a's joints at 1 + a * joints_per_actor). Phase offsets de-sync
    // the walk cycles; update_mannequin_pose samples the clip at
    // (walk_t + phase) and writes that actor's slice.
    for (std::uint32_t a = 0; a < app.city_actor_count; ++a) {
      update_mannequin_pose(
          app, walk_t + app.city_actors[a].walk_phase,
          1U + a * app.joints_per_actor);
    }

    // Static city parts: ground, buildings, poles, heads (identity payload,
    // joint_base 0).
    for (const auto& part : app.city_parts) {
      omnicpp::render::ScenePbrObject obj;
      obj.mesh = &part.buffers.mesh;
      obj.model = part.model;
      obj.material_index = part.material_index;
      obj.joint_base = 0U;
      app.scene.objects.push_back(obj);
    }
    // Sponza landmark: one draw per primitive (index-offset slices over the
    // shared buffers), local vertices + the node model, materials at
    // kSponzaMaterialBase + p.
    if (app.sponza_enabled) {
      for (std::size_t p = 0; p < app.sponza.parts.size(); ++p) {
        const auto& part = app.sponza.parts[p];
        if (!part.is_drawable()) continue;
        omnicpp::render::ScenePbrObject obj;
        obj.mesh = &part;
        obj.model = app.sponza.part_models[p];
        obj.material_index = kSponzaMaterialBase + static_cast<std::uint32_t>(p);
        obj.joint_base = 0U;  // identity bone slot (static)
        app.scene.objects.push_back(obj);
      }
    }
    // Actors: shared per-part meshes, per-actor model + joint slice.
    for (std::uint32_t a = 0; a < app.city_actor_count; ++a) {
      for (std::size_t i = 0; i < app.mannequin_meshes.size(); ++i) {
        omnicpp::render::ScenePbrObject part;
        part.mesh = &app.mannequin_meshes[i].mesh;
        part.model = app.city_actors[a].model;
        part.material_index = 3U;
        part.joint_base = 1U + a * app.joints_per_actor;
        app.scene.objects.push_back(part);
      }
    }

    // Many-light pipeline family (skinned vertex stages for everything;
    // static meshes carry the identity payload). RT variant swaps the
    // fragment for the ray-query one and drops the shadow map.
    const bool use_rt = app.rt_mode &&
                        app.rt_full_ml_pipeline.pipeline() != VK_NULL_HANDLE;
    app.scene.pipeline =
        use_rt ? app.rt_full_ml_skinned_pipeline.pipeline()
               : app.full_ml_skinned_pipeline.pipeline();
    app.scene.pipeline_layout =
        use_rt ? app.rt_full_ml_skinned_pipeline.pipeline_layout()
               : app.full_ml_skinned_pipeline.pipeline_layout();
    app.scene.bone_set = app.bone_set;
    app.scene.ibl_set = app.ibl5_set;
    app.scene.ibl_set_slot = 5U;
    app.scene.lights_set = app.city_lights_set;
    app.scene.lights_set_slot = 6U;
    if (use_rt) {
      app.scene.rt_set = app.rt_set;
      app.scene.rt_set_slot = 4U;
      app.scene.shadow_pipeline = VK_NULL_HANDLE;
      app.scene.shadow_pipeline_layout = VK_NULL_HANDLE;
      app.scene.shadow_set = VK_NULL_HANDLE;
    } else {
      app.scene.rt_set = VK_NULL_HANDLE;
      app.scene.shadow_set = app.no_shadow ? app.neutral_shadow_set
                                           : app.shadow_set;
      app.scene.shadow_set_slot = 4U;
      app.scene.shadow_pipeline = app.shadow_pipeline_static.pipeline();
      app.scene.shadow_pipeline_layout =
          app.shadow_pipeline_static.pipeline_layout();
    }
    // M11: document cubes mirror into the city branch too (it returns
    // below, so the shared tail append cannot serve it).
    mirror_document_objects(app);
    return omnicpp::render::VulkanRenderer{}
        .record_pbr_scene(command_buffer, app.scene, width, height)
        .is_ok();
  }

  // Ground slab (shared backdrop for both scene variants).
  omnicpp::render::ScenePbrObject ground;
  ground.mesh = &app.ground.mesh;
  ground.model = multiply(translation_matrix(0.0f, -0.05f, 0.0f),
                          scale_matrix(8.0f, 0.1f, 8.0f));
  ground.material_index = 2U;
  app.scene.objects.push_back(ground);

  if (app.has_mannequin) {
    // Walking mannequin at the origin: skinned pipeline, pose driven by the
    // deterministic animation state machine (ticked once per frame in the
    // window callback; the capture path re-records without ticking, so both
    // see the identical pose).
    const float idle_weight =
        app.machine != nullptr ? app.machine->blended_weight() : 0.0f;
    app.last_idle_weight = idle_weight;
    {
      const bool idle_dominates = idle_weight > 0.5f;
      const float prev_walk = app.walk_time;
      update_mannequin_pose(app, walk_t);
      if (idle_weight > 0.0f) {
        // Blend the idle clip over the walk pose by the machine's weight.
        std::vector<omnicpp::asset::GltfSkinNode> pose = app.mannequin.nodes;
        omnicpp::asset::sample_clip_blended(
            app.mannequin, app.mannequin.animations[1], walk_t,
            idle_weight, pose);
        app.mannequin.nodes = pose;
      }
      if (idle_dominates) app.walk_time = prev_walk;  // pause walk clock
    }
    for (auto& buffers : app.mannequin_meshes) {
      omnicpp::render::ScenePbrObject part;
      part.mesh = &buffers.mesh;
      // Bind vertices are authored around the skeleton origin, feet at the
      // ground plane; shift so the figure stands on the slab.
      part.model = translation_matrix(0.0f, 0.05f, 0.0f);
      part.material_index = 3U;
      app.scene.objects.push_back(part);
    }
    if (app.lighting_ready) {
      app.scene.pipeline = app.full_skinned_pipeline.pipeline();
      app.scene.pipeline_layout =
          app.full_skinned_pipeline.pipeline_layout();
      app.scene.ibl_set = app.ibl5_set;
      app.scene.ibl_set_slot = 5U;
    } else {
      app.scene.pipeline = app.skinned_pipeline.pipeline();
      app.scene.pipeline_layout = app.skinned_pipeline.pipeline_layout();
      app.scene.ibl_set = VK_NULL_HANDLE;
      app.scene.ibl_set_slot = 0U;
    }
    app.scene.bone_set = app.bone_set;
  } else {
    // Spinning metal cube at the origin.
    omnicpp::render::ScenePbrObject spinner;
    spinner.mesh = &app.cube.mesh;
    spinner.model = multiply(translation_matrix(0.0f, 1.4f, 0.0f),
                             rotation_y_matrix(t * 0.8f));
    spinner.material_index = 0U;
    app.scene.objects.push_back(spinner);

    // Rough blue cube, counter-rotating beside it.
    omnicpp::render::ScenePbrObject rough_cube;
    rough_cube.mesh = &app.cube.mesh;
    rough_cube.model =
        multiply(translation_matrix(-2.4f, 1.0f, 0.6f),
                 multiply(rotation_y_matrix(-t * 0.5f),
                          scale_matrix(0.7f, 0.7f, 0.7f)));
    rough_cube.material_index = 1U;
    app.scene.objects.push_back(rough_cube);

    if (app.lighting_ready) {
      app.scene.pipeline = app.full_pipeline.pipeline();
      app.scene.pipeline_layout = app.full_pipeline.pipeline_layout();
      app.scene.ibl_set = app.ibl5_set;
      app.scene.ibl_set_slot = 5U;
    } else {
      app.scene.pipeline = app.pbr_pipeline.pipeline();
      app.scene.pipeline_layout = app.pbr_pipeline.pipeline_layout();
      app.scene.ibl_set = VK_NULL_HANDLE;
      app.scene.ibl_set_slot = 0U;
    }
    app.scene.bone_set = VK_NULL_HANDLE;
  }
  // Shadow set shared by both branches (null keeps the legacy path).
  // OMNICPP_NO_SHADOW keeps the composed pipelines (their shaders statically
  // use set 4) but binds the neutral 1x1 cleared map: every PCF comparison
  // passes, so the image equals composed shading with no shadow term.
  // M11: document cubes mirror after the built-in scene lists.
  mirror_document_objects(app);
  const bool shadow_active = app.lighting_ready && !app.no_shadow;
  app.scene.shadow_set =
      app.lighting_ready
          ? (shadow_active ? app.shadow_set : app.neutral_shadow_set)
          : VK_NULL_HANDLE;
  app.scene.shadow_set_slot = 4U;
  app.scene.shadow_pipeline =
      shadow_active
          ? app.shadow_pipeline_static.pipeline()
          : VK_NULL_HANDLE;
  app.scene.shadow_pipeline_layout =
      shadow_active
          ? app.shadow_pipeline_static.pipeline_layout()
          : VK_NULL_HANDLE;

  // RT mode (OMNICPP_RT_MODE=1): swap the composed pipelines for the
  // pbr_rt_full family, bind the TLAS at set 4, and skip the shadow-map
  // pre-pass (the fragment stage traces occlusion rays against the TLAS
  // instead of PCF-sampling a depth map).
  if (app.rt_mode && app.rt_set != VK_NULL_HANDLE) {
    const bool skinned = app.has_mannequin;
    app.scene.pipeline =
        skinned ? app.rt_full_skinned_pipeline.pipeline()
                : app.rt_full_pipeline.pipeline();
    app.scene.pipeline_layout =
        skinned ? app.rt_full_skinned_pipeline.pipeline_layout()
                : app.rt_full_pipeline.pipeline_layout();
    app.scene.rt_set = app.rt_set;
    app.scene.rt_set_slot = 4U;
    // No shadow-map pre-pass: the TLAS replaces the depth map.
    app.scene.shadow_pipeline = VK_NULL_HANDLE;
    app.scene.shadow_pipeline_layout = VK_NULL_HANDLE;
    app.scene.shadow_set = VK_NULL_HANDLE;
  } else {
    app.scene.rt_set = VK_NULL_HANDLE;
  }

  // GPU-driven mode: the scene description above only feeds the shadow
  // pre-pass and telemetry. The lit draw itself is ONE indirect command over
  // the mesh table — the cull/LOD compute pass (recorded in the pre-pass
  // hook) already wrote every draw command and the visibility counter, so
  // the CPU never computes visibility, LOD, or per-draw submission.
  if (app.gpu_driven) {
    // The shadow pre-pass hook leaves its 2048^2 dynamic viewport/scissor
    // behind; the driven pipeline uses dynamic viewport state, so re-set the
    // window's before drawing.
    VkViewport vp{0.0f, 0.0f, static_cast<float>(width),
                  static_cast<float>(height), 0.0f, 1.0f};
    vkCmdSetViewport(command_buffer, 0U, 1U, &vp);
    VkRect2D sc{{0, 0}, {width, height}};
    vkCmdSetScissor(command_buffer, 0U, 1U, &sc);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      app.gd_draw_pipeline.pipeline());
    // Two binds: sets must bind to consecutive slots, and the bone slot (3)
    // is unused by the driven path (cubes scene, no skinning).
    const VkDescriptorSet draw_sets[3] = {
        app.gd_draw_sets[app.renderer.current_frame()], app.scene.texture_set,
        app.scene.material_set};
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            app.gd_draw_pipeline_layout, 0U, 3U, draw_sets,
                            0U, nullptr);
    const VkDescriptorSet light_sets[2] = {app.scene.shadow_set,
                                           app.scene.ibl_set};
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            app.gd_draw_pipeline_layout, 4U, 2U, light_sets,
                            0U, nullptr);
    const GdPush push{app.scene.camera.view_projection,
                      {}, app.scene.camera_position, {}};
    vkCmdPushConstants(command_buffer, app.gd_draw_pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0U, sizeof(push), &push);
    vkCmdBindIndexBuffer(command_buffer, app.gd_shared_index_buffer, 0U,
                         VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(command_buffer, app.gd_indirect_buffer, 0U,
                             app.gd_instance_count,
                             sizeof(VkDrawIndexedIndirectCommand));
    return true;
  }

  return omnicpp::render::VulkanRenderer{}
      .record_pbr_scene(command_buffer, app.scene, width, height)
      .is_ok();
}

//! M11: mirror every document-authoritative cube into the render scene.
//! The document (registry type "cube": position/rotation/scale/color) is the
//! single source of truth — the record path consumes it read-only each frame
//! after scene.objects.clear(), so undo/redo/save/load/queries apply to
//! exactly what renders. Rotation uses the document's Euler XYZ degrees
//! (yaw about Y first, matching the city's actor convention).
void mirror_document_objects(ViewportApp& app) {
  const auto& doc_objects = app.editor.document().objects;
  for (const auto& obj : doc_objects) {
    if (obj.type_id != kDocumentCubeTypeId) continue;
    const auto pos_it = obj.properties.find("position");
    const auto rot_it = obj.properties.find("rotation");
    const auto scale_it = obj.properties.find("scale");
    const auto color_it = obj.properties.find("color");
    if (pos_it == obj.properties.end() || rot_it == obj.properties.end() ||
        scale_it == obj.properties.end()) {
      continue;
    }
    const auto& p = pos_it->second.vec;
    const auto& r = rot_it->second.vec;
    const auto& s = scale_it->second.vec;
    const float kDegToRad = 3.14159265358979f / 180.0f;
    SceneMatrix model =
        multiply(translation_matrix(static_cast<float>(p[0]),
                                    static_cast<float>(p[1]),
                                    static_cast<float>(p[2])),
                 multiply(rotation_y_matrix(static_cast<float>(r[1]) *
                                            kDegToRad),
                          scale_matrix(static_cast<float>(s[0]),
                                       static_cast<float>(s[1]),
                                       static_cast<float>(s[2]))));
    omnicpp::render::ScenePbrObject render_obj;
    render_obj.mesh = &app.cube.mesh;
    render_obj.model = model;
    render_obj.material_index = 2U;
    render_obj.joint_base = 0U;
    (void)color_it;  // material table is fixed for now (M12: per-object)
    app.scene.objects.push_back(render_obj);
  }
}

//! Renderer hook: advance the clock (the one sanctioned mutation) and record
//! the window's scene for this frame.
// ============================================================================
// Node editor overlay (OMNICPP_NODE_EDITOR=1): a live graph canvas rendered
// over the scene through the engine UI paint path.
// ============================================================================

//! Builds the demo graph (const -> add), the widget tree, and the UI
//! renderer. False (non-fatal for the scene; overlay just stays off) when
//! the ui_quad shaders are unavailable.
bool setup_node_editor(ViewportApp& app) {
  // The session document owns the graph + type registry (M7); the embedded
  // EditorSession constructor already registered the builtin node types —
  // do NOT register again (duplicate registration is a contract violation).
  // The demo const -> add graph is authored through the SAME undoable
  // commands the protocol uses, so the first undo steps are the demo's
  // construction.
  omnicpp::editor::SceneDocument& doc = app.session_document();
  {
    auto add_cmd = std::make_unique<omnicpp::editor::AddNodeCommand>(
        "const_number", 60.0, 60.0);
    std::string err;
    if (!add_cmd->apply(doc, err)) {
      std::fprintf(stderr, "viewport: demo node failed: %s\n", err.c_str());
    } else {
      const auto cn = add_cmd->node_id();
      (void)cn;
      omnicpp::editor::SetNodeParamCommand param(cn, "value",
                                                 omnicpp::editor::PropValue::make_number(2.0));
      if (!param.apply(doc, err)) {
        std::fprintf(stderr, "viewport: demo param failed: %s\n",
                     err.c_str());
      }
    }
    (void)add_cmd.release();  // demo edits bypass the stack (no undo seed)
  }
  {
    auto add_cmd =
        std::make_unique<omnicpp::editor::AddNodeCommand>("add", 320.0, 60.0);
    std::string err;
    if (!add_cmd->apply(doc, err)) {
      std::fprintf(stderr, "viewport: demo node failed: %s\n", err.c_str());
    } else {
      const auto add_id = add_cmd->node_id();
      const auto cn_id = add_id - 1U;
      omnicpp::editor::LinkNodesCommand link(cn_id, "value", add_id, "a");
      if (!link.apply(doc, err)) {
        std::fprintf(stderr, "viewport: node link failed: %s\n",
                     err.c_str());
      }
    }
    (void)add_cmd.release();
  }
  // M10: the document owns the graph; app.node_graph is a borrowed view
  // (session commands mutate it in place — no copy-back needed).
  app.node_graph = &doc.node_graph;

  app.node_view = std::make_unique<omnicpp::editor::NodeEditorView>(
      *app.node_graph);
  const auto canvas = app.ui_tree.add(warploom::ui::Widget{},
                                      app.ui_tree.root());
  app.node_canvas = canvas;
  app.node_view->rebuild(app.ui_tree, canvas);
  app.node_view->sync_widgets();

  // Toolbar: one add-button per registered type + undo/redo.
  {
    const auto toolbar = app.ui_tree.add(warploom::ui::Widget{},
                                         app.ui_tree.root());
    app.node_toolbar = toolbar;
    app.node_toolbar_buttons = omnicpp::editor::build_node_toolbar(
        app.ui_tree, toolbar, *app.node_graph);
  }

  // M13 fusion: when enabled, seed a pulse node mapped to "graph_move" so
  // the mannequin's walk<->idle machine is graph-driven out of the box.
  if (app.graph_anim_enabled) {
    auto add_cmd = std::make_unique<omnicpp::editor::AddNodeCommand>(
        "pulse", 480.0, 60.0);
    std::string err;
    if (add_cmd->apply(doc, err)) {
      const auto pid = add_cmd->node_id();
      (void)add_cmd.release();
      omnicpp::editor::SetNodeParamCommand freq(
          pid, "frequency", omnicpp::editor::PropValue::make_number(0.25));
      omnicpp::editor::SetNodeParamCommand thresh(
          pid, "threshold", omnicpp::editor::PropValue::make_number(0.5));
      (void)freq.apply(doc, err);
      (void)thresh.apply(doc, err);
    } else {
      (void)add_cmd.release();
      std::fprintf(stderr, "viewport: graph-anim pulse node failed: %s\n",
                   err.c_str());
    }
    app.graph_anim = std::make_unique<omnicpp::editor::GraphSignalAdapter>();
    // Bind to whatever id the pulse got (last added node).
    const auto pulse_id = doc.node_graph.nodes().back().id;
    app.graph_anim->set_signal({"graph_move", pulse_id, "on", 0.5});
    std::printf("viewport: graph-anim fusion on (pulse node %llu -> move)\n",
                static_cast<unsigned long long>(pulse_id));
  }

  // M11: inspector/outliner panel on the right edge.
  {
    const auto insp_canvas = app.ui_tree.add(warploom::ui::Widget{},
                                             app.ui_tree.root());
    app.inspector_canvas = insp_canvas;
    app.inspector.rebuild(app.ui_tree, insp_canvas, doc,
                          app.editor.registry(), app.editor.selected_id(),
                          app.editor.bindings());
    app.inspector_root = app.inspector.panel_handle();
  }

  const char* shader_dir_env = std::getenv("OMNICPP_SHADER_DIR");
  const std::string shader_dir =
      shader_dir_env != nullptr ? shader_dir_env : "assets/shaders";
  return app.ui_renderer
      .initialize(app.context.device(), app.context.physical_device(),
                  app.render_pass.render_pass(), app.allocator, shader_dir)
      .is_ok();
}

//! One graph evaluation per frame keeps the card readouts live. Editor
//! interaction flags (set by the X event thread) are consumed here on the
//! frame thread: undo/redo through the session's own stack and full view
//! rebuild when the graph structure changed.
void tick_node_editor(ViewportApp& app) {
  if (app.node_view == nullptr) return;

  // Undo/redo toolbar requests: the session stack is the authority. These
  // run here because stack_ is bound to the session's document and the
  // event thread must not mutate document state.
  // The viewport drives the document through direct command application
  // (event thread) and stack ops (here); rebuild flags cover both.
  // M10: drain the event-thread edit queue through the session — the
  // single mutation authority. Every queued command executes with
  // protocol-identical semantics (validation + undoable stack push) on the
  // frame thread.
  {
    std::vector<ViewportApp::QueuedEdit> drained;
    {
      std::lock_guard<std::mutex> lock(app.edit_queue_mutex);
      drained.swap(app.edit_queue);
    }
    for (auto& queued : drained) {
      const auto reply = app.editor.on_control(queued.command);
      if (!reply.ok) {
        std::fprintf(stderr, "viewport: edit rejected: %s\n",
                     reply.error.c_str());
      }
    }
  }
  // Toolbar undo/redo: real stack operations (M8 placeholder removed).
  if (app.undo_requested) {
    app.undo_requested = false;
    omnicpp::core::ControlCommand cmd;
    cmd.kind = omnicpp::core::ControlCommand::Kind::Undo;
    const auto reply = app.editor.on_control(cmd);
    if (!reply.ok) {
      std::fprintf(stderr, "viewport: undo: %s\n", reply.error.c_str());
    }
  }
  if (app.redo_requested) {
    app.redo_requested = false;
    omnicpp::core::ControlCommand cmd;
    cmd.kind = omnicpp::core::ControlCommand::Kind::Redo;
    const auto reply = app.editor.on_control(cmd);
    if (!reply.ok) {
      std::fprintf(stderr, "viewport: redo: %s\n", reply.error.c_str());
    }
  }
  if (app.node_dirty) {
    app.node_dirty = false;
    // Structural change: rebuild the widget cards; layout + paint below.
    app.node_view->rebuild(app.ui_tree, app.node_canvas);
    // The inspector mirrors document objects too (spawn/destroy/undo all
    // mark the dirty flag) — full re-derive keeps it a pure projection.
    app.inspector.rebuild(app.ui_tree, app.inspector_canvas,
                          app.editor.document(), app.editor.registry(),
                          app.editor.selected_id(), app.editor.bindings());
    app.inspector_root = app.inspector.panel_handle();
  }

  if (app.inspector_rebuild_requested) {
    app.inspector_rebuild_requested = false;
    app.inspector.rebuild(app.ui_tree, app.inspector_canvas,
                          app.editor.document(), app.editor.registry(),
                          app.editor.selected_id(), app.editor.bindings());
    app.inspector_root = app.inspector.panel_handle();
  }

  std::string error;
  // M13: context-driven nodes (time/oscillators/noise) use the sim clock +
  // frame counter — deterministic, host-driven, never wall clocks.
  omnicpp::editor::GraphContext ctx;
  ctx.time = static_cast<double>(app.time);
  ctx.tick = static_cast<std::uint64_t>(app.frame_index);
  (void)app.node_graph->evaluate_with(ctx, error);
  // M10: graph->scene bridge — bindings write their pin values into object
  // properties every tick (insertion order, deterministic; skipped bindings
  // are non-fatal).
  std::string sync_error;
  (void)app.editor.sync_graph(sync_error);
  app.node_view->sync_widgets();
  warploom::ui::compute_layout(app.ui_tree, static_cast<float>(kWidth),
                              static_cast<float>(kHeight));
  app.ui_paint.clear();
  warploom::ui::paint(app.ui_tree, app.ui_paint);
  app.node_view->append_wires(app.ui_paint);
  // M11: binding wires from output pins to the inspector chips. Anchors are
  // recomputed every frame (cheap: layout rects are fresh) so wires track
  // the panel when rows move.
  {
    std::vector<omnicpp::editor::NodeEditorView::BindingWire> wires;
    const auto& bindings = app.editor.bindings();
    wires.reserve(bindings.size());
    for (std::size_t i = 0; i < bindings.size(); ++i) {
      float ax = 0.0F;
      float ay = 0.0F;
      if (app.inspector.binding_anchor(app.ui_tree, i, ax, ay)) {
        wires.push_back({bindings[i].node_id, bindings[i].out_pin, ax, ay});
      }
    }
    app.node_view->set_binding_wires(std::move(wires));
    app.node_view->append_binding_wires(app.ui_paint);
    app.node_view->append_param_editor(app.ui_paint);
  }
}

//! Pre-pass chain when the node editor is active: the UI atlas one-time
//! layout barrier MUST be recorded before the main render pass (layout
//! transitions are illegal inside a pass), then the lighting pre-pass runs
//! its own passes. Defined after shadow_pre_pass_cb (declared above it).
bool node_editor_pre_pass_cb(VkCommandBuffer command_buffer,
                             std::uint32_t width, std::uint32_t height,
                             void* user_data) {
  auto& app = *static_cast<ViewportApp*>(user_data);
  app.ui_renderer.ensure_layout(command_buffer);
  if (app.lighting_ready) {
    return shadow_pre_pass_cb(command_buffer, width, height, user_data);
  }
  return true;
}

bool record_scene_cb(VkCommandBuffer command_buffer, std::uint32_t width,
                     std::uint32_t height, void* user_data) {
  auto& app = *static_cast<ViewportApp*>(user_data);
  const float recorded_time = app.time;
  const float recorded_walk = app.walk_time;
  const auto record_start = std::chrono::steady_clock::now();
  const bool ok = record_scene_into(command_buffer, app, recorded_time,
                                    recorded_walk, width, height);
  app.last_record_us =
      std::chrono::duration<double, std::micro>(
          std::chrono::steady_clock::now() - record_start)
          .count();
  // Node editor overlay: same render pass, scene depth test off (the scene
  // path disabled depth write for this pass), quads over the 3-D image.
  if (ok && app.node_editor && app.node_view != nullptr) {
    auto quads = app.ui_renderer.upload_paint_list(
        app.ui_paint, static_cast<float>(width), static_cast<float>(height));
    if (quads.is_ok() && quads.value() > 0U) {
      app.ui_renderer.record(command_buffer, width, height, quads.value());
    }
  }
  // Stash the recorded times for the capture path, then advance (the window
  // owns the animation clock; the walk phase only exists with a mannequin).
  app.last_recorded_time = recorded_time;
  app.last_recorded_walk = recorded_walk;
  if (!app.mannequin.animations.empty()) {
    app.walk_time += app.run_config.fixed_dt;
    const float duration = app.mannequin.animations[0].duration;
    if (duration > 0.0f && app.walk_time > duration) {
      app.walk_time -= duration;
    }
  }
  return ok;
}

}  // namespace

// ============================================================================
// Lifecycle
// ============================================================================

bool ViewportApp::initialize() {
  run_config = viewport::RunConfig::from_environment();
  // M13 fusion: enable graph->animation signal projection.
  graph_anim_enabled = std::getenv("OMNICPP_GRAPH_ANIM") != nullptr;
  // M11: resolve the document cube type id for the render mirror.
  if (const auto* cube_type =
          omnicpp::editor::default_registry().find_by_name(
              std::string(omnicpp::editor::kTypeCube));
      cube_type != nullptr) {
    kDocumentCubeTypeId = cube_type->id;
  }
  // A/B selection must be known BEFORE setup_lighting() picks the pipeline
  // family; the flag read later in this function only adds telemetry.
  legacy_lighting = std::getenv("OMNICPP_LEGACY_LIGHTING") != nullptr;
  no_shadow = std::getenv("OMNICPP_NO_SHADOW") != nullptr;
  // RT mode: hard ray-query shadows (requires composed lighting; setup
  // happens after setup_lighting builds the IBL stack it composes on).
  rt_mode = std::getenv("OMNICPP_RT_MODE") != nullptr;
  // City scene selection (must precede setup_scene: setup_mannequin loads
  // the actor asset, setup_city_scene builds on it, and setup_lighting
  // needs to know which fragment family to build).
  city_scene = std::getenv("OMNICPP_SCENE") != nullptr &&
               std::string_view(std::getenv("OMNICPP_SCENE")) == "city";
  // Sponza landmark inside the city scene (CC0 asset vendored under
  // assets/models/sponza).
  sponza_enabled = std::getenv("OMNICPP_SPONZA") != nullptr;
  if (const char* ds = std::getenv("OMNICPP_DUMP_SHADOW")) {
    dump_shadow = true;
    dump_shadow_frame = static_cast<std::uint32_t>(std::atoi(ds));
  }
  // Sun direction override — scenario control for shadow proofs (two suns
  // must move the shadow region). Normalized on read; kept above the horizon.
  if (const char* sun = std::getenv("OMNICPP_SUN_DIRECTION")) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (std::sscanf(sun, "%f,%f,%f", &x, &y, &z) == 3 && x > 0.0f &&
        y > 0.05f && z > 0.0f) {
      const float len = std::sqrt(x * x + y * y + z * z);
      sun_direction = {x / len, y / len, z / len};
    }
  }
  if (!setup_window(*this)) return false;
  setup_wm_delete_protocol(*this);

  if (!context.initialize("OmniCppViewport", false).is_ok()) {
    std::fprintf(stderr, "viewport: Vulkan context initialization failed\n");
    return false;
  }

  omnicpp::render::SurfaceCreateInfo surface_info;
  surface_info.display = connection;
  surface_info.window = reinterpret_cast<void*>(static_cast<std::uintptr_t>(window));
  surface_info.width = kWidth;
  surface_info.height = kHeight;
  if (!omnicpp::render::create_platform_surface(context.instance(),
                                                surface_info, surface)
           .is_ok()) {
    std::fprintf(stderr, "viewport: Vulkan surface creation failed\n");
    return false;
  }

  omnicpp::render::SwapchainConfig swap_config;
  swap_config.width = kWidth;
  swap_config.height = kHeight;
  swap_config.vsync = true;
  if (!swapchain.create(context.device(), context.physical_device(), surface,
                        swap_config)
           .is_ok() ||
      !swapchain.create_image_views(context.device()).is_ok()) {
    std::fprintf(stderr, "viewport: swapchain creation failed\n");
    return false;
  }

  const VkFormat depth_format =
      omnicpp::render::VulkanRenderPass::find_supported_depth_format(
          context.physical_device());
  if (!render_pass
           .create(context.device(), swapchain.image_format(), depth_format)
           .is_ok() ||
      !render_pass
           .create_depth_resources(context.device(), context.physical_device(),
                                   depth_format, swapchain.extent_width(),
                                   swapchain.extent_height())
           .is_ok() ||
      !render_pass
           .create_framebuffers(context.device(), swapchain.image_views(),
                                swapchain.extent_width(),
                                swapchain.extent_height())
           .is_ok()) {
    std::fprintf(stderr, "viewport: render pass creation failed\n");
    return false;
  }

  omnicpp::render::RendererConfig renderer_config;
  // GPU timestamp queries: per-frame device-side duration in telemetry.
  renderer_config.enable_gpu_timing = true;
  if (!renderer.initialize(context, swapchain, render_pass, renderer_config)
           .is_ok()) {
    std::fprintf(stderr, "viewport: renderer initialization failed\n");
    return false;
  }
  if (renderer.gpu_timing().available) {
    std::printf("viewport: gpu timing enabled (period %.1f ns/tick)\n",
                renderer.gpu_timing().timestamp_period_ns);
  }
  renderer.set_synchronization2(context.has_synchronization2());

  if (!setup_scene(*this)) {
    std::fprintf(stderr, "viewport: scene setup failed\n");
    return false;
  }
  // Composed full lighting (IBL + shadow map + composed PBR). Non-fatal:
  // failure falls back to the basic pbr_scene path.
  lighting_ready = setup_lighting(*this);
  if (lighting_ready && rt_mode) {
    // RT shadows replace the shadow map: same composed pipeline family,
    // set 4 becomes the TLAS, the PCF fragment swaps for pbr_rt_full.
    if (!setup_rt_shadows(*this)) {
      std::fprintf(stderr, "viewport: RT shadows unavailable; PCF path\n");
      rt_mode = false;
    } else {
      // Log telemetry AFTER the run starts (initialize runs pre-run); the
      // record path reads rt_mode directly each frame.
      std::printf("viewport: RT shadow mode active (TLAS ray queries)\n");
    }
  }
  if (lighting_ready) {
    renderer.set_frame_pre_pass_callback(shadow_pre_pass_cb, this);
    // GPU-driven draw path (cubes scene only): cull/LOD on the GPU, one
    // indirect draw per frame. Requires composed lighting (the driven
    // fragment shader statically uses the shadow + IBL sets).
    const char* gd_env = std::getenv("OMNICPP_GPU_DRIVEN");
    if (gd_env != nullptr && gd_env[0] == '1' && !has_mannequin &&
        !rt_mode &&  // GPU-driven fragment is the PCF variant; exclusive
        setup_gpu_driven(*this)) {
      gpu_driven = true;
      // Instance-count override: grows the payload past the static trio
      // (benchmarks; requires OMNICPP_GPU_DRIVEN=1).
      if (const char* count_env = std::getenv("OMNICPP_INSTANCE_COUNT")) {
        const long parsed = std::strtol(count_env, nullptr, 10);
        if (parsed >= 1L &&
            parsed <= static_cast<long>(kGdMaxInstances) - 1L) {
          gd_instance_count = static_cast<std::uint32_t>(parsed);
        }
      }
      // Physics-driven scene: N falling/rolling cubes integrated on the CPU
      // (deterministic PhysicsWorld), streamed into the payload, drawn by
      // the same one-indirect-draw path. instanceCount = bodies + ground.
      if (std::getenv("OMNICPP_PHYSICS") != nullptr) {
        const std::uint32_t body_count =
            gd_instance_count > 1U ? gd_instance_count - 1U : 3U;
        physics_bodies.reserve(body_count);
        for (std::uint32_t i = 0; i < body_count; ++i) {
          omnicpp::physics::PhysicsBody b;
          const float f = static_cast<float>(i);
          b.position[0] = -6.0f + std::fmod(f * 0.37f, 12.0f);
          b.position[1] = 2.0f + std::fmod(f * 0.11f, 8.0f);
          b.position[2] = -3.0f + std::fmod(f * 0.53f, 6.0f);
          b.radius = 0.25f + 0.2f * std::fmod(f * 0.017f, 1.0f);
          b.restitution = 0.35f;
          physics_bodies.push_back(b);
          physics_world.add_body(b);
        }
        gd_instance_count = body_count + 1U;
      }
    }
  }
  renderer.set_scene_record_callback(record_scene_cb, this);

  // Node editor overlay (OMNICPP_NODE_EDITOR=1): purely additive — setup
  // failure leaves the plain scene (logged, not fatal). The pre-pass hook
  // is REPLACED by the chained wrapper so the atlas barrier is recorded
  // before the main render pass (and the lighting pre-pass still runs).
  if (std::getenv("OMNICPP_NODE_EDITOR") != nullptr) {
    // M8: OMNICPP_DOC=<path> loads a saved document BEFORE the demo graph
    // is authored, so a persisted scene replaces the demo (missing file is
    // a warning, not fatal — the demo still comes up).
    doc_path = [] {
      const char* p = std::getenv("OMNICPP_DOC");
      return p != nullptr ? std::string(p) : std::string();
    }();
    if (!doc_path.empty()) {
      omnicpp::editor::SceneDocument loaded;
      omnicpp::editor::register_builtin_node_types(loaded.node_graph);
      std::string err;
      if (omnicpp::editor::SceneDocument::load_from_file(doc_path, loaded,
                                                         err)) {
        editor.reset_from(std::move(loaded));
        // Re-borrow the graph (same document address, but stay explicit).
        node_graph = &session_document().node_graph;
        std::printf("viewport: loaded document %s (%zu nodes)\n",
                    doc_path.c_str(), editor.document().node_graph.node_count());
      } else {
        std::fprintf(stderr, "viewport: OMNICPP_DOC load failed: %s\n",
                     err.c_str());
      }
    }
    if (setup_node_editor(*this)) {
      node_editor = true;
      renderer.set_frame_pre_pass_callback(node_editor_pre_pass_cb, this);
      std::printf("viewport: node editor overlay active (drag cards, drag "
                  "pin-to-pin to link, right-click a wire to unlink, "
                  "Delete removes the selection)\n");
    } else {
      std::fprintf(stderr, "viewport: node editor unavailable "
                           "(missing ui_quad shaders?)\n");
    }
  }

  // ------------------------------------------------------------------------
  // Observability: telemetry log + GPU-side frame capture.
  // ------------------------------------------------------------------------
  if (!run_config.telemetry_dir.empty()) {
    const std::size_t joints = has_mannequin
                                   ? mannequin.skins[0].joints.size()
                                   : 0U;
    telemetry_enabled = telemetry.open(
        run_config.telemetry_dir, run_config,
        context.device_properties().name, kWidth, kHeight,
        scene.objects.capacity(), joints, has_mannequin);
    if (!telemetry_enabled) {
      std::fprintf(stderr, "viewport: cannot open telemetry dir %s\n",
                   run_config.telemetry_dir.c_str());
    } else {
      telemetry.log_event("init", context.device_properties().name);
      // Self-describing runs: the analyzer and A/B tooling must never have
      // to guess which draw path or scene variant produced this file.
      telemetry.log_event("draw_path", gpu_driven ? "gpu_driven" : "per_draw");
      telemetry.log_event("scene_variant",
                          city_scene ? "city"
                                     : (has_mannequin ? "mannequin" : "cubes"));
      if (city_scene) {
        telemetry.log_event("city_lights", std::to_string(city_lights.size()));
        telemetry.log_event("city_actors",
                            std::to_string(city_actor_count));
        telemetry.log_event("city_parts", std::to_string(city_parts.size()));
        if (sponza_enabled) {
          telemetry.log_event("sponza_parts",
                              std::to_string(sponza.parts.size()));
          telemetry.log_event("sponza_textures",
                              std::to_string(sponza_textures.size()));
          telemetry.log_event("sponza_rt_triangles",
                              std::to_string(sponza_triangle_count));
        }
      }
      if (gpu_driven) {
        telemetry.log_event(
            "gd_instances", std::to_string(gd_instance_count));
      }
      if (!physics_bodies.empty()) {
        telemetry.log_event("physics_bodies",
                            std::to_string(physics_bodies.size()));
      }
    }
#if defined(__linux__)
    // Optional gamepad: an absent device is normal (the driver no-ops and
    // is dropped). Opened regardless of telemetry so real devices work in
    // plain interactive use.
    constexpr int kJsIndex = 0;
    {
      auto pad =
          std::make_unique<omnicpp::core::LinuxJoystickDriver>(kJsIndex);
      if (pad->is_open()) {
        if (telemetry_enabled) {
          telemetry.log_event("gamepad", "linux_joystick js0");
        }
        gamepad = std::move(pad);
      }
    }
#endif

    // ----------------------------------------------------------------------
    // Static scene manifest: readable structure of everything on screen.
    // ----------------------------------------------------------------------
    if (telemetry_enabled) {
      std::vector<std::tuple<std::string, std::size_t, std::size_t>> objects;
      objects.emplace_back("ground", 1U, 2U);
      if (has_mannequin) {
        for (std::size_t i = 0; i < mannequin_meshes.size(); ++i) {
          objects.emplace_back("figure_part_" + std::to_string(i),
                               2U + i, 3U);
        }
      } else {
        objects.emplace_back("spinner_cube", 0U, 0U);
        objects.emplace_back("rough_cube", 0U, 1U);
      }
      telemetry.log_scene_objects(objects);

      if (has_mannequin) {
        const auto& skin = mannequin.skins[0];
        std::vector<std::string> joint_names;
        joint_names.reserve(skin.joints.size());
        for (const std::size_t joint : skin.joints) {
          joint_names.push_back(mannequin.nodes[joint].name);
        }
        telemetry.log_scene_skeleton(skin.joints.size(), joint_names);

        std::vector<std::tuple<std::string, float, std::size_t>> clips;
        clips.reserve(mannequin.animations.size());
        for (const auto& clip : mannequin.animations) {
          clips.emplace_back(clip.name, clip.duration, clip.channels.size());
        }
        telemetry.log_scene_clips(clips);
      }
    }
  }
  if (legacy_lighting) {
    if (telemetry_enabled) {
      telemetry.log_event("legacy_lighting", "forced via OMNICPP_LEGACY_LIGHTING");
    }
  } else if (telemetry_enabled) {
    telemetry.log_event("lighting_mode",
                        lighting_ready ? "composed (ibl+shadow+pbr_full)"
                                       : "legacy fallback");
    char sun_buf[64];
    std::snprintf(sun_buf, sizeof(sun_buf), "%.4f,%.4f,%.4f",
                  sun_direction[0], sun_direction[1], sun_direction[2]);
    telemetry.log_event("sun_direction", sun_buf);
    if (no_shadow) {
      telemetry.log_event("shadow_mode", "disabled (OMNICPP_NO_SHADOW)");
    }
    if (rt_mode) {
      telemetry.log_event(
          "shadow_mode",
          "ray_query_tlas (OMNICPP_RT_MODE; pbr_rt_full, hard shadows; "
          "animated TLAS: skinned parts follow bones_j(t))");
    }
  }
  if (const char* script = std::getenv("OMNICPP_INPUT_SCRIPT")) {
    std::string script_error;
    if (virtual_input.load_script(script, script_error)) {
      input_scripted = true;
      if (telemetry_enabled) {
        telemetry.log_event("input_script", script);
      }
    } else {
      std::fprintf(stderr, "viewport: input script rejected: %s\n",
                   script_error.c_str());
      return false;
    }
  }
  // Capture targets exist when cadence captures are enabled OR the control
  // channel will run (a capture command must be servable without cadence
  // config). The server starts later, in run(), so probe the env here.
  const bool control_requested =
      std::getenv("OMNICPP_CONTROL_SOCKET") != nullptr;
  if (run_config.capture_every != 0U || control_requested) {
    const VkFormat depth_format =
        omnicpp::render::VulkanRenderPass::find_supported_depth_format(
            context.physical_device());
    if (!capture.initialize(
            context.device(), allocator, render_pass.render_pass(),
            swapchain.image_format(), depth_format, kWidth, kHeight,
            context.queue_families().graphics_family)) {
      std::fprintf(stderr, "viewport: frame capture initialization failed\n");
      return false;
    }
  }
  return true;
}

void ViewportApp::run() {
  std::printf(
      "viewport: %s — ESC or window close to quit\n",
      context.device_properties().name.c_str());
  time = run_config.start_time;

  // M0 control channel: OMNICPP_CONTROL_SOCKET=<path> hosts the JSONL
  // control server (pause/step/camera/sun/cube/capture) polled once per
  // frame. Purely additive — unset leaves the loop unchanged.
  if (const char* sock = std::getenv("OMNICPP_CONTROL_SOCKET"); sock != nullptr && *sock != '\0') {
    control_host = std::make_unique<ViewportControlHost>(*this);
    control_server = std::make_unique<omnicpp::core::ControlServer>();
    std::string error;
    if (control_server->start(sock, error)) {
      std::fprintf(stderr, "viewport: control server on %s\n", sock);
      if (telemetry_enabled) telemetry.log_event("control_socket", sock);
    } else {
      std::fprintf(stderr, "viewport: control server failed: %s\n",
                   error.c_str());
      control_server.reset();
      control_host.reset();
    }
  }

  while (poll_events(*this)) {
    if (g_shutdown_requested.load(std::memory_order_acquire)) {
      telemetry.log_event("exit", "signal");
      break;
    }
    // Control channel first: accept/read/dispatch so pause/step/camera
    // commands apply to THIS frame's simulation and render.
    if (control_server != nullptr) {
      (void)control_server->poll(*control_host);
    }
    // Pause/step gate: paused with no pending steps skips the whole frame
    // (no render, no clock advance); each requested step releases exactly
    // one frame so fixed-dt semantics stay intact.
    if (control_paused()) {
      if (control_steps_requested_ == 0U) {
        continue;
      }
      --control_steps_requested_;
    }
    const auto frame_start = std::chrono::steady_clock::now();
    auto image = renderer.begin_frame();
    if (!image.is_ok()) continue;
    if (!renderer
             .record_commands(image.value(),
                              render_pass.framebuffer(image.value()),
                              swapchain.extent_width(),
                              swapchain.extent_height())
             .is_ok()) {
      std::fprintf(stderr, "viewport: frame recording failed\n");
      break;
    }
    if (!renderer.end_frame().is_ok()) {
      std::fprintf(stderr, "viewport: frame presentation failed\n");
      break;
    }

    // M6 node editor: evaluate + rebuild the UI paint list for this frame
    // (cheap CPU work; the draw happens inside the scene record callback).
    if (node_editor) {
      tick_node_editor(*this);
    }
    // Input tick: poll drivers (virtual script when present), let the
    // camera respond, and log consumed events for auditability.
    input.begin_tick();
    // Driver priority: a script, when present, stands in for all real
    // devices (deterministic runs must be isolated from human input);
    // otherwise keyboard/mouse and gamepad compose.
    if (input_scripted) {
      virtual_input.poll(input);
    } else {
      kb_mouse.apply(input);
#if defined(__linux__)
      if (gamepad && gamepad->is_open()) gamepad->poll(input);
#endif
    }
    input.clamp_axes();
    input.commit_tick();
    // D2: step the physics world once per window frame (fixed dt), keeping
    // the body vector in sync for the payload writer.
    if (!physics_bodies.empty()) {
      physics_world.step(run_config.fixed_dt);
      for (std::uint32_t i = 0; i < physics_bodies.size(); ++i) {
        physics_bodies[i] = physics_world.body(i);
      }
    }
    // D1: tick the animation state machine exactly once per window frame,
    // after input commit (so it sees this tick's actions) and before the
    // next frame's scene record (so the capture path sees the settled pose).
    // Snapshot selection: scripted/real input wins; otherwise the time-driven
    // demo cycle synthesizes the toggle action at each half-period boundary
    // so ONE machine configuration serves both modes.
    if (machine) {
      // M13 fusion: graph signal actions (OMNICPP_GRAPH_ANIM=1) overlay the
      // input snapshot — node outputs hold the same "fade_toggle" action
      // the demo cycle synthesizes, so graphs control the mannequin.
      omnicpp::core::InputSnapshot snapshot = input.current();
      if (graph_anim_enabled && graph_anim != nullptr &&
          node_graph != nullptr) {
        std::string gerr;
        omnicpp::editor::GraphContext ctx;
        ctx.time = static_cast<double>(time);
        ctx.tick = static_cast<std::uint64_t>(frame_index);
        (void)node_graph->evaluate_with(ctx, gerr);
        const auto graph_snap = graph_anim->build(*node_graph);
        for (const auto& [name, held] : graph_snap.actions) {
          snapshot.actions[name] = held;
        }
        // A graph signal mapped to the machine's action name drives the
        // mannequin; map it onto fade_toggle here so the standard machine
        // configuration is graph-controllable without new transitions.
        if (snapshot.actions["graph_move"]) {
          snapshot.actions["fade_toggle"] = true;
        }
      }
      if (input_scripted) {
        machine->tick(input.current(), run_config.fixed_dt);
      } else if (run_config.crossfade_period > 0.0f) {
        omnicpp::core::InputSnapshot demo = snapshot;
        const float period = run_config.crossfade_period;
        demo.actions["fade_toggle"] = std::fmod(time, 2.0f * period) <
                                       run_config.fixed_dt;
        machine->tick(demo, run_config.fixed_dt);
      } else {
        machine->tick(snapshot, run_config.fixed_dt);
      }
      if (telemetry_enabled && machine->state() != machine_last_state) {
        machine_last_state = machine->state();
        telemetry.log_event("anim_state", machine->state());
      }
    }
    // Response: orbit_left/right (actions) and zoom_in/out (actions) move
    // the camera; move_x/move_y axes are logged for downstream consumers.
    if (input.action("orbit_left")) camera_orbit_bias -= 0.02f;
    if (input.action("orbit_right")) camera_orbit_bias += 0.02f;
    if (input.action("zoom_in")) camera_radius_bias -= 1.5f * run_config.fixed_dt;
    if (input.action("zoom_out")) camera_radius_bias += 1.5f * run_config.fixed_dt;
    if (input.action_pressed("fade_toggle")) {
      if (machine) {
        // D1: the machine consumed the edge in its tick this frame (the
        // press scans its own min-time guards). Log the resulting state as
        // the outcome value: idle = 1.0, walk = 0.0 (the scenario contract).
        if (telemetry_enabled) {
          telemetry.log_input(frame_index, "virtual", "fade_toggle",
                              machine->state() == "idle" ? 1.0f : 0.0f);
        }
      } else {
        fade_target = fade_target > 0.5f ? 0.0f : 1.0f;
        if (telemetry_enabled) {
          telemetry.log_input(frame_index, "virtual", "fade_toggle",
                              fade_target);
        }
      }
    }
    camera_radius_bias =
        std::min(std::max(camera_radius_bias, -3.0f), 4.0f);
    if (telemetry_enabled) {
      for (const char* action : {"orbit_left", "orbit_right", "zoom_in",
                                 "zoom_out"}) {
        if (input.action(action)) {
          telemetry.log_input(frame_index, "virtual", action, 1.0f);
        }
      }
      if (std::abs(input.axis("move_x")) > 1e-4f ||
          std::abs(input.axis("move_y")) > 1e-4f) {
        telemetry.log_input(frame_index, "virtual", "move",
                            input.axis("move_x"));
      }
    }

    // GPU-side capture: re-record the exact scene just displayed into the
    // capture targets and pull color+depth to the host. Control-channel
    // captures (capture command) bypass the cadence but need telemetry.
    std::string capture_name;
    const bool control_capture = control_capture_requested_ && telemetry_enabled;
    control_capture_requested_ = false;
    if ((run_config.capture_every != 0U &&
         captures_done < run_config.capture_limit &&
         (frame_index + 1U) % run_config.capture_every == 0U &&
         telemetry_enabled) ||
        control_capture) {
      const bool captured = capture.capture(
          context.graphics_queue(), [&](VkCommandBuffer cmd) {
            return record_scene_into(cmd, *this, last_recorded_time,
                                     last_recorded_walk, kWidth, kHeight);
          });
      if (captured) {
        if (capture.save(frame_index + 1U, run_config.telemetry_dir,
                         capture_name)) {
          ++captures_done;
        } else {
          std::fprintf(stderr, "viewport: capture save failed\n");
        }
      } else {
        std::fprintf(stderr, "viewport: capture recording failed\n");
      }
    }

    // Telemetry for the frame just presented.
    const auto frame_end = std::chrono::steady_clock::now();
    const double total_us =
        std::chrono::duration<double, std::micro>(frame_end - frame_start)
            .count();
    const double instantaneous_fps = total_us > 0.0 ? 1e6 / total_us : 0.0;
    fps_smoothed = fps_smoothed == 0.0
                       ? instantaneous_fps
                       : 0.9 * fps_smoothed + 0.1 * instantaneous_fps;
    if (telemetry_enabled) {
      std::size_t drawn = 0;
      for (const auto& object : scene.objects) {
        if (object.mesh != nullptr && object.mesh->is_drawable()) ++drawn;
      }
      const bool skinned = scene.bone_set != VK_NULL_HANDLE;
      telemetry.log_frame(
          frame_index + 1U, time, last_recorded_walk,
          scene.camera_position[0], scene.camera_position[1],
          scene.camera_position[2], scene.objects.size(), drawn, skinned,
          last_record_us, total_us, static_cast<float>(fps_smoothed),
          capture_name, last_idle_weight, renderer.gpu_timing().last_total_ns);

      // Pose summary + engine memory stats (bounded size, every frame).
      if (has_mannequin) {
        const auto& root_node = mannequin.nodes[0];
        const auto stats_now = allocator.stats();
        telemetry.log_pose(
            root_node.translation[0], root_node.translation[1],
            root_node.translation[2], last_swing_joint, last_swing_deg,
            stats_now.used_bytes, stats_now.reserved_bytes,
            stats_now.allocation_count, last_recorded_walk,
            last_idle_weight);
      }
      if (run_config.max_frames != 0U &&
          frame_index + 1U >= run_config.max_frames) {
        telemetry.log_event("exit", "max_frames reached");
        telemetry.flush();
        break;
      }
      telemetry.flush();
    }

    time += run_config.fixed_dt;  // deterministic animation clock
    // Shadow-map dump: copy the depth-only pre-pass output to the host so
    // the pre-pass is directly observable (occupied texels < 1.0).
    if (dump_shadow && shadow_image != VK_NULL_HANDLE &&
        frame_index + 1U == dump_shadow_frame &&
        dump_shadow_buffer == VK_NULL_HANDLE) {
      constexpr VkDeviceSize kShadowBytes = static_cast<VkDeviceSize>(
          ViewportApp::kShadowRes * ViewportApp::kShadowRes * sizeof(float));
      auto dump_buf = allocator.create_buffer(
          kShadowBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (dump_buf.is_ok()) {
        dump_shadow_allocation = dump_buf.value();
        dump_shadow_buffer = dump_shadow_allocation.buffer;
        auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
            context.device(),
            static_cast<std::uint32_t>(
                context.queue_families().graphics_family));
        if (pool.is_ok()) {
          auto cb = omnicpp::render::VulkanRenderer::allocate_command_buffer(
              context.device(), pool.value());
          if (cb.is_ok()) {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cb.value(), &bi);
            VkImageMemoryBarrier to_src{
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            to_src.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.oldLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.image = shadow_image;
            to_src.subresourceRange = {
                VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U, 1U};
            vkCmdPipelineBarrier(
                cb.value(), VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                &to_src);
            VkBufferImageCopy region{};
            region.imageSubresource = {
                VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 0U, 1U};
            region.imageExtent = {ViewportApp::kShadowRes,
                                  ViewportApp::kShadowRes, 1U};
            vkCmdCopyImageToBuffer(
                cb.value(), shadow_image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_shadow_buffer, 1,
                &region);
            VkImageMemoryBarrier back = to_src;
            back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            back.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back.newLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(
                cb.value(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0,
                nullptr, 1, &back);
            vkEndCommandBuffer(cb.value());
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers = &cb.value();
            vkQueueSubmit(context.graphics_queue(), 1U, &submit,
                          VK_NULL_HANDLE);
            vkQueueWaitIdle(context.graphics_queue());
            vkDestroyCommandPool(context.device(), pool.value(), nullptr);
            std::FILE* f =
                std::fopen("/tmp/shadowmap.f32", "wb");
            if (f != nullptr) {
              std::fwrite(dump_shadow_allocation.mapped, 1,
                          static_cast<std::size_t>(kShadowBytes), f);
              std::fclose(f);
              std::fprintf(stderr, "viewport: shadow map dumped\n");
            }
          } else {
            vkDestroyCommandPool(context.device(), pool.value(), nullptr);
          }
        }
      }
    }

    ++frame_index;
  }
  renderer.wait_idle();
}

void ViewportApp::shutdown() {
  renderer.wait_idle();
  capture.cleanup(context.device(), &allocator);
  telemetry.flush();
  if (node_editor) {
    ui_renderer.cleanup(context.device());
    node_view.reset();
  }
  // RT-shadow resources (AS buffers/instances/scratch) before teardown.
  destroy_rt_shadows(*this);
  renderer.cleanup(context.device());
  gd_cull_pipeline.cleanup(context.device());
  gd_draw_pipeline.cleanup(context.device());
  rt_full_skinned_pipeline.cleanup(context.device());
  rt_full_pipeline.cleanup(context.device());
  // Many-light city variants (only created when OMNICPP_SCENE=city).
  rt_full_ml_skinned_pipeline.cleanup(context.device());
  rt_full_ml_pipeline.cleanup(context.device());
  full_ml_skinned_pipeline.cleanup(context.device());
  full_ml_pipeline.cleanup(context.device());
  full_skinned_pipeline.cleanup(context.device());
  full_pipeline.cleanup(context.device());
  shadow_pipeline_skinned.cleanup(context.device());
  shadow_pipeline_static.cleanup(context.device());
  ibl_baker.cleanup(context.device());
  skinned_pipeline.cleanup(context.device());
  pbr_pipeline.cleanup(context.device());
  render_pass.cleanup(context.device());
  swapchain.cleanup(context.device());
  // Full-lighting resources (shadow map, views, sampler, pass/fb).
  if (shadow_framebuffer != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(context.device(), shadow_framebuffer, nullptr);
    shadow_framebuffer = VK_NULL_HANDLE;
  }
  if (shadow_render_pass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(context.device(), shadow_render_pass, nullptr);
    shadow_render_pass = VK_NULL_HANDLE;
  }
  if (shadow_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(context.device(), shadow_sampler, nullptr);
    shadow_sampler = VK_NULL_HANDLE;
  }
  for (VkImageView* view : {&shadow_depth_view, &shadow_sample_view}) {
    if (*view != VK_NULL_HANDLE) {
      vkDestroyImageView(context.device(), *view, nullptr);
      *view = VK_NULL_HANDLE;
    }
  }
  if (shadow_memory.is_valid()) {
    allocator.destroy_allocation(shadow_memory);
  }
  if (shadow_image != VK_NULL_HANDLE) {
    vkDestroyImage(context.device(), shadow_image, nullptr);
    shadow_image = VK_NULL_HANDLE;
  }
  if (shadow_ubo_allocation.is_valid()) {
    allocator.destroy_allocation(shadow_ubo_allocation);
  }
  // Neutral shadow resources (OMNICPP_NO_SHADOW diagnostic path).
  if (neutral_shadow_sample_view != VK_NULL_HANDLE) {
    vkDestroyImageView(context.device(), neutral_shadow_sample_view, nullptr);
    neutral_shadow_sample_view = VK_NULL_HANDLE;
  }
  if (neutral_shadow_memory.is_valid()) {
    allocator.destroy_allocation(neutral_shadow_memory);
  }
  if (neutral_shadow_image != VK_NULL_HANDLE) {
    vkDestroyImage(context.device(), neutral_shadow_image, nullptr);
    neutral_shadow_image = VK_NULL_HANDLE;
  }
  if (material_allocation.is_valid()) {
    allocator.destroy_allocation(material_allocation);
  }
  allocator.destroy_allocation(cube.vertex_allocation);
  allocator.destroy_allocation(cube.index_allocation);
  allocator.destroy_allocation(ground.vertex_allocation);
  allocator.destroy_allocation(ground.index_allocation);
  for (auto& buffers : mannequin_meshes) {
    allocator.destroy_allocation(buffers.vertex_allocation);
    allocator.destroy_allocation(buffers.index_allocation);
  }
  if (bone_allocation.is_valid()) {
    allocator.destroy_allocation(bone_allocation);
  }
  // Sponza landmark resources (only when OMNICPP_SPONZA=1).
  for (auto& t : sponza_textures) {
    if (t.scene.view != VK_NULL_HANDLE) {
      vkDestroyImageView(context.device(), t.scene.view, nullptr);
    }
    if (t.allocation.is_valid()) {
      allocator.destroy_allocation(t.allocation);
    }
    if (t.image != VK_NULL_HANDLE) {
      vkDestroyImage(context.device(), t.image, nullptr);
    }
  }
  sponza_textures.clear();
  if (sponza_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(context.device(), sponza_sampler, nullptr);
    sponza_sampler = VK_NULL_HANDLE;
  }
  if (sponza.vertex_allocation.is_valid()) {
    allocator.destroy_allocation(sponza.vertex_allocation);
  }
  if (sponza.index_allocation.is_valid()) {
    allocator.destroy_allocation(sponza.index_allocation);
  }
  descriptors.cleanup();
  allocator.cleanup();
  context.destroy_surface(surface);
  context.cleanup();
  if (connection) {
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
  }
}

int main() {
  std::signal(SIGTERM, handle_shutdown_signal);
  std::signal(SIGINT, handle_shutdown_signal);
  ViewportApp app;
  if (!app.initialize()) {
    app.shutdown();
    return 1;
  }
  app.run();
  app.shutdown();
  return 0;
}
