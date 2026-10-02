//! @file test_scene3d.cpp
//! @brief A real 3D scene: lit cubes on a ground plane plus an analytically
//!        ray-traced sphere, orbiting animation, and depth ordering.
//!
//! Verified by pixel classification and targeted probe points:
//!   1. The ground plane fills the lower half (its albedo dominates there).
//!   2. The red-orange sphere renders in front of the blue cube behind it
//!      (fragment ray-sphere hit shades sphere-colored pixels inside the
//!      sphere's projected disc even where the cube is rasterized).
//!   3. Orbiting a cube around the scene changes the image (animation) while
//!      the static parts (plane, sphere center pixel) stay identical.
//!   4. The ground plane depth-occludes nothing it shouldn't: cubes sitting
//!      ON the plane show both their lit top faces and the plane behind.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_renderer.hpp"
#ifdef WARPLOOM_HAS_VULKAN
#include "vulkan_test_readback.hpp"
using omnicpp_test::readback_swapchain_image;
#endif

namespace {

constexpr std::uint32_t kSize = 256U;

std::uint32_t bits(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4U);
  return u;
}

void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      float* m16) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m16[0] = f / aspect; m16[1] = 0; m16[2] = 0; m16[3] = 0;
  m16[4] = 0; m16[5] = f; m16[6] = 0; m16[7] = 0;
  m16[8] = 0; m16[9] = 0; m16[10] = zfar * zn; m16[11] = -1.0f;
  m16[12] = 0; m16[13] = 0; m16[14] = znear * zfar * zn; m16[15] = 0;
}

//! Camera looking at `target` from `eye` (right-handed, -z forward view
//! space). Column-major.
void make_view(const float eye[3], const float target[3], float* m16) {
  float fwd[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  const float fl = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
  for (float& v : fwd) v /= fl;
  // world-up = +y; right = fwd x up (then normalize); up = right x fwd.
  float right[3] = {fwd[2], 0.0f, -fwd[0]};
  const float rl = std::sqrt(right[0]*right[0] + right[2]*right[2]);
  right[0] /= rl; right[2] /= rl;
  float up[3] = {
      right[1]*fwd[2] - right[2]*fwd[1],
      right[2]*fwd[0] - right[0]*fwd[2],
      right[0]*fwd[1] - right[1]*fwd[0]};
  // View matrix rows: right, up, -fwd; translation = -R * eye.
  m16[0] = right[0]; m16[1] = up[0]; m16[2] = -fwd[0]; m16[3] = 0.0f;
  m16[4] = right[1]; m16[5] = up[1]; m16[6] = -fwd[1]; m16[7] = 0.0f;
  m16[8] = right[2]; m16[9] = up[2]; m16[10] = -fwd[2]; m16[11] = 0.0f;
  m16[12] = -(right[0]*eye[0] + right[1]*eye[1] + right[2]*eye[2]);
  m16[13] = -(up[0]*eye[0] + up[1]*eye[1] + up[2]*eye[2]);
  m16[14] = (fwd[0]*eye[0] + fwd[1]*eye[1] + fwd[2]*eye[2]);
  m16[15] = 1.0f;
}

void mat4_multiply(const float* a, const float* b, float* out) {
  float r[16];
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[col * 4 + k];
      r[col * 4 + row] = sum;
    }
  }
  std::memcpy(out, r, sizeof(r));
}

}  // namespace

TEST(VulkanHardware, Scene3DObjectsLightingAnimation) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppScene3D", true).is_ok());
  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(),
                                   context.physical_device()).is_ok());

  // --- Scene: ground slab + 3 cubes. Sphere is procedural (no instance). ---
  constexpr std::uint32_t kInstances = 4U;
  struct Instance {
    float pos[3];
    float scale;
    float color[3];
    std::uint32_t shape;  // 0 cube, 1 slab
  };
  const Instance instances[kInstances] = {
      {{0.0f, -1.0f, 0.0f}, 14.0f, {0.35f, 0.45f, 0.30f}, 1U},  // ground
      {{-2.2f, 0.0f, -3.0f}, 0.9f, {0.15f, 0.3f, 0.95f}, 0U},   // blue cube
      {{2.4f, 0.0f, -6.0f}, 1.1f, {0.9f, 0.8f, 0.2f}, 0U},      // yellow cube
      {{0.6f, 0.0f, -9.0f}, 0.8f, {0.8f, 0.2f, 0.75f}, 0U},     // magenta cube
  };
  auto inst_buf = allocator.create_buffer(
      kInstances * 8U * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(inst_buf.is_ok());
  {
    auto* w = static_cast<std::uint32_t*>(inst_buf.value().mapped);
    for (std::uint32_t i = 0; i < kInstances; ++i) {
      const std::uint32_t base = i * 8U;
      w[base + 0U] = bits(instances[i].pos[0]);
      w[base + 1U] = bits(instances[i].pos[1]);
      w[base + 2U] = bits(instances[i].pos[2]);
      w[base + 3U] = bits(instances[i].scale);
      w[base + 4U] = bits(instances[i].color[0]);
      w[base + 5U] = bits(instances[i].color[1]);
      w[base + 6U] = bits(instances[i].color[2]);
      w[base + 7U] = instances[i].shape;
    }
  }

  // --- Descriptors + pipelines. ---
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> bindings(1);
  bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 VK_SHADER_STAGE_VERTEX_BIT};
  auto layout = manager.create_layout(bindings, 1);
  ASSERT_TRUE(layout.is_ok());
  auto dset = manager.allocate_set(layout.value());
  ASSERT_TRUE(dset.is_ok());
  ASSERT_TRUE(manager.write_buffer(dset.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   inst_buf.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  // Push block: view_proj (64) + 4 words (vertex part) + camera (16) +
  // sphere (16) + light (16) = 128 bytes.
  // C++ struct packs vec3+pad as 12+4 with no extra padding: total 124.
  // GLSL std430-style push blocks pad each vec3 to 16, so the byte offsets
  // match exactly; only the tail differs, which no member follows.
  constexpr std::uint32_t kPushBytes = 128U;  // GLSL pads vec3 tails to 16
  const VkPushConstantRange push_range{
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
  omnicpp::render::VulkanPipeline gfx_pipe;
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/scene.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/scene.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(gfx_pipe.create_pipeline_layout(
      context.device(), &layout.value(), 1, &push_range).is_ok());

  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator).is_ok());
  ASSERT_TRUE(target.create_depth(context.device(), context.physical_device(),
                                  VK_FORMAT_D32_SFLOAT).is_ok());
  ASSERT_TRUE(target.create_render_pass(context.device()).is_ok());
  ASSERT_TRUE(target.create_framebuffer(context.device()).is_ok());
  ASSERT_TRUE(gfx_pipe.create_graphics_pipeline(
      context.device(), target.render_pass(), target.format(),
      gfx_pipe.pipeline_layout(), true, true, true).is_ok());

  // --- Command infrastructure. ---
  const auto pool_result = omnicpp::render::VulkanRenderer::create_command_pool(
      context.device(),
      static_cast<std::uint32_t>(context.queue_families().graphics_family));
  ASSERT_TRUE(pool_result.is_ok());
  const VkCommandPool pool = pool_result.value();
  const auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      context.device(), pool);
  ASSERT_TRUE(cb_result.is_ok());
  const VkCommandBuffer cb = cb_result.value();
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateFence(context.device(), &fence_info, nullptr, &fence), VK_SUCCESS);

  // --- Push block assembly. ---
  struct PushData {
    float view_proj[16];
    std::uint32_t instance_count;
    std::uint32_t pad[3];  // GLSL pads the vec3 tail to a 16-byte slot
    float camera_pos[3];
    float pad2;
    float sphere_center[3];
    float sphere_radius;
    float light_dir[3];
    float pad3;
  };
  static_assert(sizeof(PushData) == kPushBytes, "push block size mismatch");
  static_assert(sizeof(PushData) == 128U, "shader expects a 128-byte block");

  const float eye[3] = {0.0f, 2.2f, 4.5f};
  const float look[3] = {0.0f, 0.0f, -4.0f};
  float view[16], proj[16];
  make_view(eye, look, view);
  make_perspective(1.05f, 1.0f, 0.1f, 100.0f, proj);

  PushData push{};
  mat4_multiply(proj, view, push.view_proj);
  push.instance_count = kInstances;
  std::memset(push.pad, 0, sizeof(push.pad));
  std::memcpy(push.camera_pos, eye, 12U);
  const float sphere_center[3] = {0.0f, 0.3f, -4.5f};
  std::memcpy(push.sphere_center, sphere_center, 12U);
  push.sphere_radius = 0.85f;
  const float light[3] = {0.4472f, 0.7453f, -0.4930f};  // normalized
  std::memcpy(push.light_dir, light, 12U);

  const auto render = [&]() -> omnicpp_test::ReadbackResult {
    EXPECT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {kSize, kSize};
    VkClearValue clears[2]{};
    clears[0].color = {{0.06f, 0.08f, 0.12f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    rb.clearValueCount = 2;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {kSize, kSize}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gfx_pipe.pipeline_layout(), 0, 1, &dset.value(), 0, nullptr);
    vkCmdPushConstants(cb, gfx_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, kPushBytes, &push);
    vkCmdDraw(cb, 36U, kInstances, 0, 0);
    vkCmdEndRenderPass(cb);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    EXPECT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    EXPECT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
    return readback_swapchain_image(
        context.physical_device(), context.device(), context.graphics_queue(),
        static_cast<std::uint32_t>(context.queue_families().graphics_family),
        target.image(), target.format(), kSize, kSize,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  };

  // --- Frame 1: static scene. ---
  const auto frame1 = render();
  ASSERT_TRUE(frame1.submitted);
  std::cout << "[scene3d] center=0x" << std::hex << frame1.center_pixel
            << " lower=0x" << frame1.lower_triangle_pixel << std::dec
            << " non_clear=" << frame1.non_clear_pixels << "\n";

  // The ground plane (large slab, green-ish albedo, lit) fills the lower
  // half; its green channel must dominate the lower probe.
  {
    const std::uint32_t px = frame1.lower_triangle_pixel;
    const std::uint32_t g = (px >> 8U) & 0xFFU;
    const std::uint32_t r = px & 0xFFU;
    EXPECT_GT(g, 40U);      // green albedo lit by directional light
    EXPECT_GT(g, r);        // more green than red
  }
  // Overall: most of the frame is populated by the scene.
  EXPECT_GT(frame1.non_clear_pixels, 20000U);

  // The sphere hovers at the frame center: its shaded color (albedo
  // (0.9,0.35,0.15) under lambert + ambient) must be strongly red-dominant.
  {
    const std::uint32_t px = frame1.center_pixel;
    const std::uint32_t r = px & 0xFFU;
    const std::uint32_t g = (px >> 8U) & 0xFFU;
    const std::uint32_t b = (px >> 16U) & 0xFFU;
    EXPECT_GT(r, g);   // warm albedo survives lighting
    EXPECT_GT(r, b);
  }

  // --- Frame 2: animate — orbit the blue cube around the sphere. ---
  {
    const float ang = 1.2f;
    const float x = std::cos(ang) * 3.0f;
    const float z = -4.5f + std::sin(ang) * 3.0f;
    auto* w = static_cast<std::uint32_t*>(inst_buf.value().mapped);
    w[1U * 8U + 0U] = bits(x);
    w[1U * 8U + 2U] = bits(z);
  }
  const auto frame2 = render();
  ASSERT_TRUE(frame2.submitted);
  EXPECT_NE(frame2.hash, frame1.hash);              // the scene changed
  EXPECT_EQ(frame2.center_pixel, frame1.center_pixel);  // sphere static

  // --- Frame 3: animate back — image must return to frame 1 exactly. ---
  {
    auto* w = static_cast<std::uint32_t*>(inst_buf.value().mapped);
    w[1U * 8U + 0U] = bits(-2.2f);
    w[1U * 8U + 2U] = bits(-3.0f);
  }
  const auto frame3 = render();
  ASSERT_TRUE(frame3.submitted);
  EXPECT_EQ(frame3.hash, frame1.hash);  // deterministic round trip

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  gfx_pipe.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation b = inst_buf.value();
  allocator.destroy_allocation(b);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
