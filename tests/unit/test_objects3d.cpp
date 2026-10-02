//! @file test_objects3d.cpp
//! @brief Actual 3D object rendering: solid cube meshes drawn with model
//!        matrices through a depth-tested pipeline. Verified by readback:
//!        (1) a cube fills pixels with its assigned face colors, (2) a near
//!        cube depth-occludes a far cube completely (both orderings), and
//!        (3) rotation animation changes the rendered image.

#include <gtest/gtest.h>

#include <algorithm>
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

constexpr float kFovY = 1.05f;
constexpr float kAspect = 1.0f;
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;

//! Perspective projection, Vulkan clip space (column-major, like GLSL mat4).
void make_perspective(float fov_y, float aspect, float znear, float zfar,
                      float* m16) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (znear - zfar);
  m16[0] = f / aspect; m16[1] = 0; m16[2] = 0; m16[3] = 0;
  m16[4] = 0; m16[5] = f; m16[6] = 0; m16[7] = 0;
  m16[8] = 0; m16[9] = 0; m16[10] = zfar * zn; m16[11] = -1.0f;
  m16[12] = 0; m16[13] = 0; m16[14] = znear * zfar * zn; m16[15] = 0;
}

void make_identity(float* m16) {
  for (int i = 0; i < 16; ++i) m16[i] = 0.0f;
  m16[0] = m16[5] = m16[10] = m16[15] = 1.0f;
}

void make_translation(float x, float y, float z, float* m16) {
  make_identity(m16);
  m16[12] = x; m16[13] = y; m16[14] = z;
}

//! Column-major rotation about Y.
void make_rotation_y(float angle, float* m16) {
  make_identity(m16);
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  m16[0] = c;  m16[2] = -s;
  m16[8] = s;  m16[10] = c;
}

//! Column-major rotation about X.
void make_rotation_x(float angle, float* m16) {
  make_identity(m16);
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  m16[5] = c;  m16[6] = s;
  m16[9] = -s; m16[10] = c;
}

//! out = a * b (column-major convention: transforms apply b first).
void mat4_multiply(const float* a, const float* b, float* out) {
  float r[16];
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      r[col * 4 + row] = sum;
    }
  }
  std::memcpy(out, r, sizeof(r));
}

//! 36-vertex cube (6 faces x 2 triangles), 8 floats per vertex: pos.xyz,
//! pad, color.rgb, pad. All faces share one color so a drawn cube reads
//! unambiguously as that color in pixel classification.
//!
//! Winding: faces are authored CW in view space (y-up) so that after the
//! projection's y-flip into Vulkan's y-down NDC/framebuffer space they are
//! CCW — matching the pipeline's VK_FRONT_FACE_COUNTER_CLOCKWISE with back
//! culling (same effective orientation as the bundled visible triangle).
void build_cube_mesh(float half, const float color[3], float* out288) {
  const float h = half;
  struct Face { float v0[3], v1[3], v2[3], v3[3]; };
  const Face faces[6] = {
      {{-h, -h,  h}, { h, -h,  h}, {-h,  h,  h}, { h,  h,  h}},  // +z front
      {{ h, -h, -h}, {-h, -h, -h}, { h,  h, -h}, {-h,  h, -h}},  // -z back
      {{ h, -h,  h}, { h, -h, -h}, { h,  h,  h}, { h,  h, -h}},  // +x right
      {{-h, -h, -h}, {-h, -h,  h}, {-h,  h, -h}, {-h,  h,  h}},  // -x left
      {{-h,  h,  h}, { h,  h,  h}, {-h,  h, -h}, { h,  h, -h}},  // +y top
      {{-h, -h, -h}, { h, -h, -h}, {-h, -h,  h}, { h, -h,  h}},  // -y bottom
  };
  std::uint32_t vi = 0;
  const auto emit = [&](const float* p) {
    float* v = out288 + vi * 8U;
    v[0] = p[0]; v[1] = p[1]; v[2] = p[2]; v[3] = 0.0f;
    v[4] = color[0]; v[5] = color[1]; v[6] = color[2]; v[7] = 0.0f;
    ++vi;
  };
  for (const auto& f : faces) {
    emit(f.v0);
    emit(f.v2);
    emit(f.v1);
    emit(f.v2);
    emit(f.v3);
    emit(f.v1);
  }
}

}  // namespace

TEST(VulkanHardware, CubeMeshDepthOcclusionAnimation) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }

  omnicpp::render::VulkanContext context;
  ASSERT_TRUE(context.initialize("OmniCppObjects3DTest", true).is_ok());

  omnicpp::render::VulkanMemoryAllocator allocator;
  ASSERT_TRUE(allocator.initialize(context.device(), context.physical_device()).is_ok());

  // --- Cube mesh buffers: cube A all red, cube B all green. Separate
  // meshes + descriptor sets make the pixel classification unambiguous.
  constexpr std::uint32_t kVerts = 36U;
  constexpr VkDeviceSize kMeshBytes = kVerts * 8U * sizeof(float);
  const float red[3] = {1.0f, 0.0f, 0.0f};
  const float green[3] = {0.0f, 1.0f, 0.0f};
  auto mesh_a = allocator.create_buffer(
      kMeshBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(mesh_a.is_ok());
  build_cube_mesh(1.0f, red, static_cast<float*>(mesh_a.value().mapped));
  auto mesh_b = allocator.create_buffer(
      kMeshBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(mesh_b.is_ok());
  build_cube_mesh(1.0f, green, static_cast<float*>(mesh_b.value().mapped));

  // --- Descriptors + pipeline. ---
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(context.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> bindings(1);
  bindings[0] = {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT};
  auto layout = manager.create_layout(bindings, 2);  // dset_a + dset_b
  ASSERT_TRUE(layout.is_ok());
  auto dset_a = manager.allocate_set(layout.value());
  ASSERT_TRUE(dset_a.is_ok());
  ASSERT_TRUE(manager.write_buffer(dset_a.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   mesh_a.value().buffer, 0, VK_WHOLE_SIZE).is_ok());
  auto dset_b = manager.allocate_set(layout.value());
  ASSERT_TRUE(dset_b.is_ok());
  ASSERT_TRUE(manager.write_buffer(dset_b.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                   mesh_b.value().buffer, 0, VK_WHOLE_SIZE).is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  constexpr VkDeviceSize kPushBytes = 128U;  // two mat4
  const VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, kPushBytes};
  omnicpp::render::VulkanPipeline gfx_pipe;
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/cube.vert.spv", "vertex").is_ok());
  ASSERT_TRUE(gfx_pipe.load_shader_stage_file(
      context.device(), shader_dir + "/cube.frag.spv", "fragment").is_ok());
  ASSERT_TRUE(gfx_pipe.create_pipeline_layout(
      context.device(), &layout.value(), 1, &push_range).is_ok());

  // --- Offscreen target with depth attachment. ---
  omnicpp::render::VulkanOffscreenTarget target;
  ASSERT_TRUE(target.create(context.device(), context.physical_device(),
                            VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &allocator).is_ok());
  if (!target.create_depth(context.device(), context.physical_device(),
                           VK_FORMAT_D32_SFLOAT).is_ok()) {
    ASSERT_TRUE(target.create_depth(context.device(), context.physical_device(),
                                    VK_FORMAT_D24_UNORM_S8_UINT).is_ok());
  }
  ASSERT_TRUE(target.has_depth());
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

  float view_proj[16];
  make_perspective(kFovY, kAspect, kNear, kFar, view_proj);

  struct FrameTransforms {
    float view_proj[16];
    float model[16];
  };

  // Render one frame with the given cube transforms; returns classification.
  const auto render_frame = [&](const float* model_a, const float* model_b)
      -> omnicpp_test::ReadbackResult {
    FrameTransforms xa{}, xb{};
    std::memcpy(xa.view_proj, view_proj, 64U);
    std::memcpy(xa.model, model_a, 64U);
    std::memcpy(xb.view_proj, view_proj, 64U);
    std::memcpy(xb.model, model_b, 64U);

    EXPECT_EQ(vkResetCommandBuffer(cb, 0), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &begin), VK_SUCCESS);

    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {256, 256};
    VkClearValue clears[2]{};
    clears[0].color = {{0.02f, 0.02f, 0.05f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    rb.clearValueCount = 2;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, 256, 256, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {256, 256}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline());
    const VkDescriptorSet ds_a = dset_a.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline_layout(),
                            0, 1, &ds_a, 0, nullptr);
    vkCmdPushConstants(cb, gfx_pipe.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT,
                       0, kPushBytes, &xa);
    vkCmdDraw(cb, kVerts, 1, 0, 0);
    const VkDescriptorSet ds_b = dset_b.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipe.pipeline_layout(),
                            0, 1, &ds_b, 0, nullptr);
    vkCmdPushConstants(cb, gfx_pipe.pipeline_layout(), VK_SHADER_STAGE_VERTEX_BIT,
                       0, kPushBytes, &xb);
    vkCmdDraw(cb, kVerts, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);

    EXPECT_EQ(vkResetFences(context.device(), 1, &fence), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(context.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    EXPECT_EQ(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);

    const auto readback = readback_swapchain_image(
        context.physical_device(), context.device(), context.graphics_queue(),
        static_cast<std::uint32_t>(context.queue_families().graphics_family),
        target.image(), target.format(), 256, 256,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    EXPECT_TRUE(readback.submitted);
    return readback;
  };

  // Cube A: near, front face red. Cube B: far, front face green — its whole
  // footprint sits behind A's, so the depth test must hide it completely.
  float model_a[16], model_b[16];
  make_translation(0.0f, 0.0f, -4.0f, model_a);
  make_translation(0.0f, 0.0f, -8.0f, model_b);

  const auto frame1 = render_frame(model_a, model_b);
  EXPECT_GT(frame1.red_dominant_pixels, 5000U);  // near cube front face visible
  EXPECT_EQ(frame1.green_dominant_pixels, 0U);   // far cube fully occluded

  // Frame 2: swap depths — B in front now fully occludes A.
  make_translation(0.0f, 0.0f, -8.0f, model_a);
  make_translation(0.0f, 0.0f, -4.0f, model_b);
  const auto frame2 = render_frame(model_a, model_b);
  EXPECT_EQ(frame2.red_dominant_pixels, 0U);
  EXPECT_GT(frame2.green_dominant_pixels, 5000U);

  // Frame 3: restore frame-1 layout but rotate A 45 degrees about Y — the
  // image must change (animation) while the near cube still hides B.
  make_translation(0.0f, 0.0f, -4.0f, model_a);
  make_translation(0.0f, 0.0f, -8.0f, model_b);
  float rot[16], anim_a[16];
  make_rotation_y(0.785398f, rot);
  mat4_multiply(model_a, rot, anim_a);
  float rotb[16], anim_b[16];
  make_rotation_x(1.570796f, rotb);
  mat4_multiply(model_b, rotb, anim_b);
  const auto frame3 = render_frame(anim_a, anim_b);
  EXPECT_GT(frame3.red_dominant_pixels, 2000U);  // front face partially visible at 45 deg
  EXPECT_EQ(frame3.green_dominant_pixels, 0U);   // rotated far cube still occluded
  EXPECT_NE(frame3.hash, frame1.hash);           // rotation changed the image

  EXPECT_EQ(context.validation_error_count(), 0U);
  EXPECT_EQ(context.validation_warning_count(), 0U);

  vkDestroyFence(context.device(), fence, nullptr);
  vkDestroyCommandPool(context.device(), pool, nullptr);
  gfx_pipe.cleanup(context.device());
  target.cleanup(context.device());
  omnicpp::render::Allocation ma = mesh_a.value();
  omnicpp::render::Allocation mb = mesh_b.value();
  allocator.destroy_allocation(ma);
  allocator.destroy_allocation(mb);
  manager.cleanup();
  allocator.cleanup();
  context.cleanup();
#else
  GTEST_SKIP() << "Vulkan support or test shaders were not enabled";
#endif
}
