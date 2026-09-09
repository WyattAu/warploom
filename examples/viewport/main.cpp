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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include <xcb/xcb.h>
#include <vulkan/vulkan.h>

#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_render_pass.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "engine/render/vulkan_scene.hpp"
#include "engine/render/scene_camera.hpp"
#include "engine/render/vulkan_surface.hpp"
#include "engine/render/vulkan_swapchain.hpp"

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
  VkDescriptorSetLayout mesh_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout textures_layout{VK_NULL_HANDLE};
  VkDescriptorSetLayout material_layout{VK_NULL_HANDLE};
  VkDescriptorSet textures_set{VK_NULL_HANDLE};
  VkDescriptorSet material_set{VK_NULL_HANDLE};
  omnicpp::render::Allocation material_allocation{};

  struct MeshBuffers {
    omnicpp::render::Allocation vertex_allocation{};
    omnicpp::render::Allocation index_allocation{};
    omnicpp::render::SceneMesh mesh{};
  };
  MeshBuffers cube{};
  MeshBuffers ground{};

  std::vector<omnicpp::render::ScenePbrObject> objects;
  omnicpp::render::VulkanPbrScene scene;

  // XCB window.
  xcb_connection_t* connection{nullptr};
  xcb_window_t window{0};
  xcb_atom_t wm_protocols{0};
  xcb_atom_t wm_delete_window{0};
  VkSurfaceKHR surface{VK_NULL_HANDLE};

  // Animation state.
  float time{0.0f};

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
  auto mesh_layout = app.descriptors.create_layout(mesh_bindings, 8U);
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
  return true;
}

//! Per-frame scene record callback installed on the renderer. Runs inside
//! the render pass; rebuilds object transforms from the animation clock and
//! the orbiting camera, then delegates to record_pbr_scene.
bool record_scene_cb(VkCommandBuffer command_buffer, std::uint32_t width,
                     std::uint32_t height, void* user_data) {
  auto& app = *static_cast<ViewportApp*>(user_data);

  const float t = app.time;
  // Orbiting camera: 6.5 units out, slowly circling, looking at the origin.
  const float eye[3] = {6.5f * std::cos(t * 0.25f), 3.2f,
                        6.5f * std::sin(t * 0.25f)};
  const float target[3] = {0.0f, 0.8f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  app.scene.camera.view_projection = omnicpp::render::scene_camera_view_projection(
      eye, target, up, 1.05f,
      static_cast<float>(width) / static_cast<float>(height), 0.1f, 100.0f);
  // Camera position in view conventions: the look_at eye.
  app.scene.camera_position = {eye[0], eye[1], eye[2], 1.0f};

  app.scene.objects.clear();

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

  // Ground slab.
  omnicpp::render::ScenePbrObject ground;
  ground.mesh = &app.ground.mesh;
  ground.model = multiply(translation_matrix(0.0f, -0.05f, 0.0f),
                          scale_matrix(8.0f, 0.1f, 8.0f));
  ground.material_index = 2U;
  app.scene.objects.push_back(ground);

  return omnicpp::render::VulkanRenderer{}
      .record_pbr_scene(command_buffer, app.scene, width, height)
      .is_ok();
}

}  // namespace

// ============================================================================
// Lifecycle
// ============================================================================

bool ViewportApp::initialize() {
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
  return true;
}

void ViewportApp::run() {
  std::printf(
      "viewport: %s — ESC or window close to quit\n",
      context.device_properties().name.c_str());
  while (poll_events(*this)) {
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
    time += 1.0f / 60.0f;  // vsync-paced animation clock
  }
  renderer.wait_idle();
}

void ViewportApp::shutdown() {
  renderer.cleanup(context.device());
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
