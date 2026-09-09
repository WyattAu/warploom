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
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#include "engine/asset/gltf_animation.hpp"
#include "engine/asset/gltf_importer.hpp"
#include "engine/core/input_state.hpp"
#include "engine/core/input_translators.hpp"
#include "engine/render/vulkan_context.hpp"
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
#include "telemetry.hpp"

using SceneMatrix = omnicpp::render::SceneMatrix;

namespace {

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
  std::chrono::steady_clock::time_point frame_started{};
  double fps_smoothed{0.0};

  [[nodiscard]] bool initialize();
  void run();
  void shutdown();
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
      // ESC (keycode 9 on most servers).
      if (key->detail == 9) {
        free(event);
        return false;
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
    } else if (type == XCB_BUTTON_PRESS) {
      const auto* button =
          reinterpret_cast<const xcb_button_press_event_t*>(event);
      app.kb_mouse.on_button(button->detail, true);
    } else if (type == XCB_BUTTON_RELEASE) {
      const auto* button =
          reinterpret_cast<const xcb_button_press_event_t*>(event);
      app.kb_mouse.on_button(button->detail, false);
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

//! Frames of payload copies (must match the renderer's frames in flight).
constexpr std::uint32_t kViewportMaxFramesInFlight = 2U;
//! GPU-driven scene: ground slab + two cubes (payload/cull/dispatch count).
constexpr std::uint32_t kGdObjectCount = 3U;
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
  return true;
}

//! Sample the walk cycle at `time` and upload joint matrices. Computes
//! joints = global_pose(j) * inverse_bind(j) directly (same math the GPU
//! test cross-checks).
void update_mannequin_pose(ViewportApp& app, float time) {
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

  // Joint matrices: global * inverse bind, in joint order.
  auto* bones = static_cast<SceneMatrix*>(app.bone_allocation.mapped);
  std::size_t swing_index = 0;
  float swing_max = -1.0f;
  for (std::size_t j = 0; j < skin.joints.size(); ++j) {
    const SceneMatrix& g = globals[skin.joints[j]];
    const SceneMatrix& ibm = skin.inverse_bind_matrices[j];
    bones[j] = multiply(g, ibm);
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
  // Pool sized for cubes + ground + the 9 mannequin meshes plus headroom.
  auto mesh_layout = app.descriptors.create_layout(mesh_bindings, 32U);
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

  // Set 2: material SSBO (4 slots).
  const std::vector<omnicpp::render::ReflectedBinding> material_bindings = {
      {2U, 0U, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
       VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto material_layout = app.descriptors.create_layout(material_bindings, 8U);
  if (!material_layout.is_ok()) return false;
  app.material_layout = material_layout.value();
  auto material_set = app.descriptors.allocate_set(app.material_layout);
  if (!material_set.is_ok()) return false;
  app.material_set = material_set.value();

  auto material_buffer = app.allocator.create_buffer(
      4U * sizeof(omnicpp::render::PbrMaterialData),
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
  // 0: brushed metal cube, 1: rough dielectric cube, 2: ground, 3: spare.
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
  // Host-side copies of the cube + ground geometry (same 11-float layout as
  // the per-draw path) go through the table builder, which rewrites indices
  // into one global vertex space. The ground slab's (8, 0.1, 8) scale is
  // baked into its vertices so the payload model stays identity.
  std::vector<float> cube_verts;
  std::vector<std::uint32_t> cube_idx;
  build_unit_cube(cube_verts, cube_idx);
  std::vector<float> ground_verts;
  ground_verts.reserve(cube_verts.size());
  for (std::size_t i = 0; i < cube_verts.size(); i += 11U) {
    ground_verts.insert(
        ground_verts.end(),
        {cube_verts[i] * 8.0f, cube_verts[i + 1] * 0.1f,
         cube_verts[i + 2] * 8.0f, cube_verts[i + 3], cube_verts[i + 4],
         cube_verts[i + 5], cube_verts[i + 6], cube_verts[i + 7],
         cube_verts[i + 8], cube_verts[i + 9], cube_verts[i + 10]});
  }
  std::vector<std::uint32_t> ground_idx(cube_idx);

  app.gd_slot_cube = app.gd_table_builder.add(
      omnicpp::render::SceneMesh{}, cube_verts, cube_idx);
  app.gd_slot_ground = app.gd_table_builder.add(
      omnicpp::render::SceneMesh{}, ground_verts, ground_idx);
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
  constexpr std::uint32_t kObjectCount = 3U;  // ground + spinner + rough
  constexpr VkDeviceSize kPayloadBytes = (2U + 24U * kObjectCount) * 4U;
  constexpr VkDeviceSize kDrawWords = 5U * kObjectCount + 1U;  // + counter
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
           .load_shader_stage_file(
               dev, shader_dir + "/cull_and_draw_lod.comp.spv", "compute")
           .is_ok() ||
      !app.gd_cull_pipeline
           .create_pipeline_layout(
               dev, &app.gd_set0_layout, 1U, &kGdCullPush)
           .is_ok() ||
      !app.gd_cull_pipeline.create_compute_pipeline(dev).is_ok()) {
    std::fprintf(stderr, "viewport: gd cull pipeline failed\n");
    return false;
  }
  // The driven fragment shader statically uses the shadow (set 4) and IBL
  // (set 5) slots, so the draw layout declares all five sets. gd mode is only
  // enabled with composed lighting (both sets are always bound).
  const VkDescriptorSetLayout draw_layouts[5] = {
      app.gd_set0_layout, app.textures_layout, app.material_layout,
      app.shadow_layout, app.ibl5_layout};
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
           .create_pipeline_layout(               dev, draw_layouts, 5U, &draw_push)
           .is_ok() ||
      !app.gd_draw_pipeline
           .create_graphics_pipeline(dev, app.render_pass.render_pass(),
                                     app.swapchain.image_format(),
                                     app.gd_draw_pipeline.pipeline_layout(),
                                     /*depth_test=*/true,
                                     /*depth_write=*/true, /*cull=*/false)
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
  constexpr std::uint32_t kObjectCount = 3U;
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

  // Same transforms as record_scene_into's cubes branch; the ground slab's
  // scale is baked into its mesh-table geometry, so its model is identity.
  write_obj(0U, omnicpp::render::scene_identity_matrix(), 2U,
            app.gd_slot_ground, {0.0f, -0.05f, 0.0f}, 5.66f);
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
                                     /*cull=*/false)
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
                                     /*cull=*/false)
           .is_ok()) {
    std::fprintf(stderr, "viewport: composed skinned pipeline failed\n");
    std::fprintf(stderr, "viewport: setup_lighting failed at line 1240\n"); return false;
  }

  // --- 4. Shadow-casting pipelines (depth-only). -------------------------
  const VkDescriptorSetLayout solo_mesh[1] = {app.mesh_layout};
  const VkPushConstantRange shadow_push{VK_SHADER_STAGE_VERTEX_BIT, 0U, 128U};
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
    vkCmdDispatch(command_buffer, (kGdObjectCount + 63U) / 64U, 1U, 1U);
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
  } push{};
  push.light_vp = app.scene.shadow_light_vp;

  for (const auto& object : app.scene.objects) {
    if (object.mesh == nullptr || !object.mesh->is_drawable()) continue;
    const bool skinned = app.has_mannequin &&
                         object.mesh != &app.ground.mesh;
    const omnicpp::render::VulkanPipeline& pipe =
        skinned ? app.shadow_pipeline_skinned : app.shadow_pipeline_static;
    push.model = object.model;
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

  // Ground slab (shared backdrop for both scene variants).
  omnicpp::render::ScenePbrObject ground;
  ground.mesh = &app.ground.mesh;
  ground.model = multiply(translation_matrix(0.0f, -0.05f, 0.0f),
                          scale_matrix(8.0f, 0.1f, 8.0f));
  ground.material_index = 2U;
  app.scene.objects.push_back(ground);

  if (app.has_mannequin) {
    // Walking mannequin at the origin: skinned pipeline, pose sampled from
    // the walk cycle (optionally cross-faded to idle), bones uploaded before
    // recording.
    //
    // Blend selection: the scripted "fade_toggle" action takes priority once
    // engaged (target or current nonzero). Otherwise the time-driven
    // walk<->idle demo cycle runs when configured.
    const bool input_fade =
        app.fade_target != 0.0f || app.fade_current != 0.0f;
    if (input_fade) {
      // Ease the current weight toward the target (2.5/s -> ~0.4 s fade).
      const float step = 2.5f * app.run_config.fixed_dt;
      if (app.fade_current < app.fade_target) {
        app.fade_current =
            std::min(app.fade_current + step, app.fade_target);
      } else if (app.fade_current > app.fade_target) {
        app.fade_current =
            std::max(app.fade_current - step, app.fade_target);
      }
      app.last_idle_weight = app.fade_current;
      const bool idle_dominates = app.fade_current > 0.5f;
      const float prev_walk = app.walk_time;
      update_mannequin_pose(app, walk_t);
      if (app.fade_current > 0.0f) {
        // Blend the idle clip over the walk pose by the current weight.
        std::vector<omnicpp::asset::GltfSkinNode> pose = app.mannequin.nodes;
        omnicpp::asset::sample_clip_blended(
            app.mannequin, app.mannequin.animations[1], walk_t,
            app.fade_current, pose);
        app.mannequin.nodes = pose;
      }
      if (idle_dominates) app.walk_time = prev_walk;  // pause walk clock
    } else if (app.run_config.crossfade_period > 0.0f &&
               app.mannequin.animations.size() >= 2U) {
      // walk <-> idle cycle: fade out over the first half, back over the
      // second. The walk clock pauses while idle dominates (feet planted).
      const float period = app.run_config.crossfade_period;
      const float phase = std::fmod(t, 2.0f * period);
      float idle_weight;
      if (phase < period) {
        idle_weight = phase / period;  // walk -> idle
      } else {
        idle_weight = 2.0f - phase / period;  // idle -> walk
      }
      app.last_idle_weight = idle_weight;
      const bool idle_dominates = idle_weight > 0.5f;
      const float prev_walk = app.walk_time;
      update_mannequin_pose(app, walk_t);
      if (idle_dominates) {
        // Blend the idle clip over the walk pose by the excess weight.
        std::vector<omnicpp::asset::GltfSkinNode> pose = app.mannequin.nodes;
        omnicpp::asset::sample_clip_blended(
            app.mannequin, app.mannequin.animations[1], walk_t,
            idle_weight, pose);
        app.mannequin.nodes = pose;
      }
      if (idle_dominates) app.walk_time = prev_walk;  // pause walk clock
    } else {
      app.last_idle_weight = 0.0f;
      update_mannequin_pose(app, walk_t);
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

  // GPU-driven mode: the scene description above only feeds the shadow
  // pre-pass and telemetry. The lit draw itself is ONE indirect command over
  // the mesh table — the cull/LOD compute pass (recorded in the pre-pass
  // hook) already wrote every draw command and the visibility counter, so
  // the CPU never computes visibility, LOD, or per-draw submission.
  if (app.gpu_driven) {
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      app.gd_draw_pipeline.pipeline());
    const VkDescriptorSet draw_sets[5] = {
        app.gd_draw_sets[app.renderer.current_frame()], app.scene.texture_set,
        app.scene.material_set, app.scene.shadow_set, app.scene.ibl_set};
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            app.gd_draw_pipeline_layout, 0U, 5U, draw_sets,
                            0U, nullptr);
    const GdPush push{app.scene.camera.view_projection,
                      {}, app.scene.camera_position, {}};
    vkCmdPushConstants(command_buffer, app.gd_draw_pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0U, sizeof(push), &push);
    vkCmdBindIndexBuffer(command_buffer, app.gd_shared_index_buffer, 0U,
                         VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(command_buffer, app.gd_indirect_buffer, 0U,
                             kGdObjectCount,
                             sizeof(VkDrawIndexedIndirectCommand));
    return true;
  }

  return omnicpp::render::VulkanRenderer{}
      .record_pbr_scene(command_buffer, app.scene, width, height)
      .is_ok();
}

//! Renderer hook: advance the clock (the one sanctioned mutation) and record
//! the window's scene for this frame.
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
  // A/B selection must be known BEFORE setup_lighting() picks the pipeline
  // family; the flag read later in this function only adds telemetry.
  legacy_lighting = std::getenv("OMNICPP_LEGACY_LIGHTING") != nullptr;
  no_shadow = std::getenv("OMNICPP_NO_SHADOW") != nullptr;
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
  if (!renderer.initialize(context, swapchain, render_pass, renderer_config)
           .is_ok()) {
    std::fprintf(stderr, "viewport: renderer initialization failed\n");
    return false;
  }
  renderer.set_synchronization2(context.has_synchronization2());

  if (!setup_scene(*this)) {
    std::fprintf(stderr, "viewport: scene setup failed\n");
    return false;
  }
  // Composed full lighting (IBL + shadow map + composed PBR). Non-fatal:
  // failure falls back to the basic pbr_scene path.
  lighting_ready = setup_lighting(*this);
  if (lighting_ready) {
    renderer.set_frame_pre_pass_callback(shadow_pre_pass_cb, this);
    // GPU-driven draw path (cubes scene only): cull/LOD on the GPU, one
    // indirect draw per frame. Requires composed lighting (the driven
    // fragment shader statically uses the shadow + IBL sets).
    const char* gd_env = std::getenv("OMNICPP_GPU_DRIVEN");
    if (gd_env != nullptr && gd_env[0] == '1' && !has_mannequin &&
        setup_gpu_driven(*this)) {
      gpu_driven = true;
    }
  }
  renderer.set_scene_record_callback(record_scene_cb, this);

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
  if (run_config.capture_every != 0U) {
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
  while (poll_events(*this)) {
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
    // Response: orbit_left/right (actions) and zoom_in/out (actions) move
    // the camera; move_x/move_y axes are logged for downstream consumers.
    if (input.action("orbit_left")) camera_orbit_bias -= 0.02f;
    if (input.action("orbit_right")) camera_orbit_bias += 0.02f;
    if (input.action("zoom_in")) camera_radius_bias -= 1.5f * run_config.fixed_dt;
    if (input.action("zoom_out")) camera_radius_bias += 1.5f * run_config.fixed_dt;
    if (input.action_pressed("fade_toggle")) {
      fade_target = fade_target > 0.5f ? 0.0f : 1.0f;
      if (telemetry_enabled) {
        telemetry.log_input(frame_index, "virtual", "fade_toggle",
                            fade_target);
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
    // capture targets and pull color+depth to the host.
    std::string capture_name;
    if (run_config.capture_every != 0U &&
        captures_done < run_config.capture_limit &&
        (frame_index + 1U) % run_config.capture_every == 0U &&
        telemetry_enabled) {
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
          capture_name, last_idle_weight);

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
  renderer.cleanup(context.device());
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
  ViewportApp app;
  if (!app.initialize()) {
    app.shutdown();
    return 1;
  }
  app.run();
  app.shutdown();
  return 0;
}
