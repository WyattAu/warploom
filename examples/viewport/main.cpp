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
#include <string>
#include <vector>

#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#include "engine/asset/gltf_animation.hpp"
#include "engine/asset/gltf_importer.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_render_pass.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_scene.hpp"
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
                                   XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_create_window(app.connection, XCB_COPY_FROM_PARENT, app.window,
                    screen->root, 0, 0, kWidth, kHeight, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                    XCB_CW_EVENT_MASK, &event_mask);
  xcb_change_property(app.connection, XCB_PROP_MODE_REPLACE, app.window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 14,
                      "OmniCpp Viewport");
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
    const std::string model_path = dir + "/" + model + ".gltf";
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

//! Build the scene description for sim time `t` / walk phase `walk_t` and
//! record it into a begun render pass. Pure: mutates nothing on `app`, so
//! the window path and the capture path can record the identical scene.
bool record_scene_into(VkCommandBuffer command_buffer, ViewportApp& app,
                       float t, float walk_t, std::uint32_t width,
                       std::uint32_t height) {
  const float orbit_radius = std::isnan(app.run_config.camera_radius)
                                 ? (app.has_mannequin ? 3.2f : 6.5f)
                                 : app.run_config.camera_radius;
  const float height_default = std::isnan(app.run_config.camera_height)
                                   ? (app.has_mannequin ? 1.6f : 3.2f)
                                   : app.run_config.camera_height;
  const float eye[3] = {orbit_radius * std::cos(t * 0.25f), height_default,
                        orbit_radius * std::sin(t * 0.25f)};
  const float target[3] = {0.0f, app.has_mannequin ? 0.9f : 0.8f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  app.scene.camera.view_projection = omnicpp::render::scene_camera_view_projection(
      eye, target, up, 1.05f,
      static_cast<float>(width) / static_cast<float>(height), 0.1f, 100.0f);
  // Camera position in view conventions: the look_at eye.
  app.scene.camera_position = {eye[0], eye[1], eye[2], 1.0f};

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
    if (app.run_config.crossfade_period > 0.0f &&
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
    app.scene.pipeline = app.skinned_pipeline.pipeline();
    app.scene.pipeline_layout = app.skinned_pipeline.pipeline_layout();
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

    app.scene.pipeline = app.pbr_pipeline.pipeline();
    app.scene.pipeline_layout = app.pbr_pipeline.pipeline_layout();
    app.scene.bone_set = VK_NULL_HANDLE;
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
    ++frame_index;
  }
  renderer.wait_idle();
}

void ViewportApp::shutdown() {
  renderer.wait_idle();
  capture.cleanup(context.device(), &allocator);
  telemetry.flush();
  renderer.cleanup(context.device());
  skinned_pipeline.cleanup(context.device());
  pbr_pipeline.cleanup(context.device());
  render_pass.cleanup(context.device());
  swapchain.cleanup(context.device());
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
