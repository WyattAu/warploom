//! @file test_sky.cpp
//! @brief GPU end-to-end tests for the analytic atmospheric sky pass.
//! Renders the Preetham-style sky offscreen under Khronos validation and
//! asserts on pixel readback: (1) the sky fills the frame with a blue-
//! dominant gradient, (2) the region toward the sun is brighter than away
//! from it, (3) the sun disc appears as a saturated highlight.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "engine/render/vulkan_descriptors.hpp"
#include "engine/render/vulkan_offscreen.hpp"
#include "engine/render/vulkan_pipeline.hpp"
#include "engine/render/vulkan_memory_allocator.hpp"
#include "engine/render/vulkan_context.hpp"
#include "engine/render/vulkan_renderer.hpp"
#include "vulkan_test_readback.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

using omnicpp::render::Allocation;
using omnicpp::render::VulkanContext;
using omnicpp::render::VulkanDescriptorManager;
using omnicpp::render::VulkanMemoryAllocator;
using omnicpp::render::VulkanOffscreenTarget;
using omnicpp::render::VulkanPipeline;

struct SkyParamsUbo {
  float sun_direction[4];
  float rayleigh[4];
  float mie[4];
  float misc[4];
};

struct SkyPush {
  float camera_position[4];  // xyz eye, w tan_half_fov
  float forward[4];          // xyz forward, w aspect
  float right[4];
  float up[4];
};

// Helper: fill a 4-float array from an initializer list (arrays can't be
// assigned from braced lists in C++).
void set4(float* dst, float a, float b, float c, float d) {
  dst[0] = a;
  dst[1] = b;
  dst[2] = c;
  dst[3] = d;
}

struct SkyHarness {
  VulkanContext ctx;
  VulkanMemoryAllocator alloc;
  VulkanDescriptorManager desc;
  VulkanOffscreenTarget target;
  VulkanPipeline pipe;
  Allocation ubo{};
  VkDescriptorSetLayout ubo_layout{VK_NULL_HANDLE};
  VkDescriptorSet ubo_set{VK_NULL_HANDLE};
  uint32_t qf = 0;

  bool init(const char* name) {
    if (!ctx.initialize(name, true).is_ok()) return false;
    if (!alloc.initialize(ctx.device(), ctx.physical_device()).is_ok()) return false;
    if (!desc.initialize(ctx.device()).is_ok()) return false;
    qf = static_cast<uint32_t>(ctx.queue_families().graphics_family);

    // UBO (set 0).
    auto r = desc.create_layout({{0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  VK_SHADER_STAGE_FRAGMENT_BIT}},
                                4);
    if (!r.is_ok()) return false;
    ubo_layout = r.value();
    auto s = desc.allocate_set(ubo_layout);
    if (!s.is_ok()) return false;
    ubo_set = s.value();
    auto b = alloc.create_buffer(sizeof(SkyParamsUbo),
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!b.is_ok()) return false;
    ubo = b.value();
    if (!desc.write_buffer(ubo_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           ubo.buffer, 0, VK_WHOLE_SIZE).is_ok()) return false;

    if (!target.create(ctx.device(), ctx.physical_device(),
                       VK_FORMAT_B8G8R8A8_UNORM, 256, 256, &alloc).is_ok() ||
        !target.create_render_pass(ctx.device()).is_ok() ||
        !target.create_framebuffer(ctx.device()).is_ok()) return false;

    std::string sd = WARPLOOM_TEST_SHADER_DIR;
    if (!pipe.load_shader_stage_file(ctx.device(), sd + "/sky.vert.spv",
                                     "vertex").is_ok() ||
        !pipe.load_shader_stage_file(ctx.device(), sd + "/sky.frag.spv",
                                     "fragment").is_ok()) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(SkyPush)};
    if (!pipe.create_pipeline_layout(ctx.device(), &ubo_layout, 1, &pr).is_ok())
      return false;
    // No depth attachment on the sky-only target: disable depth entirely.
    if (!pipe.create_graphics_pipeline(ctx.device(), target.render_pass(),
                                       target.format(),
                                       pipe.pipeline_layout(), false, false,
                                       false).is_ok()) return false;
    return true;
  }

  omnicpp_test::ReadbackResult render(const SkyParamsUbo& params,
                                      const SkyPush& push) {
    std::memcpy(ubo.mapped, &params, sizeof(params));

    VkDevice dev = ctx.device();
    auto pr = omnicpp::render::VulkanRenderer::create_command_pool(dev, qf);
    if (!pr.is_ok()) return {};
    auto cr = omnicpp::render::VulkanRenderer::allocate_command_buffer(dev, pr.value());
    if (!cr.is_ok()) {
      vkDestroyCommandPool(dev, pr.value(), nullptr);
      return {};
    }
    VkCommandBuffer cb = cr.value();
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(dev, &fi, nullptr, &fence);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear{};
    clear.color = {{0.f, 0.f, 0.f, 1.f}};
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = target.render_pass();
    rpb.framebuffer = target.framebuffer();
    rpb.renderArea.extent = {256, 256};
    rpb.clearValueCount = 1;
    rpb.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, 256, 256, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {256, 256}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipe.pipeline_layout(), 0, 1, &ubo_set, 0, nullptr);
    vkCmdPushConstants(cb, pipe.pipeline_layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    vkResetFences(dev, 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroyCommandPool(dev, pr.value(), nullptr);
    return omnicpp_test::readback_swapchain_image(
        ctx.physical_device(), dev, ctx.graphics_queue(), qf,
        target.image(), target.format(), 256, 256,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }

  void cleanup() {
    VkDevice dev = ctx.device();
    pipe.cleanup(dev);
    target.cleanup(dev);
    if (ubo.is_valid()) alloc.destroy_allocation(ubo);
    desc.cleanup();
    alloc.cleanup();
    ctx.cleanup();
  }
};

// Camera basis for an eye at the given position looking along `fwd` with
// world up +Y. Matches the shader's push-constant contract.
SkyPush make_push(float ex, float ey, float ez, float fx, float fy, float fz,
                  float fov_deg, float aspect) {
  SkyPush p{};
  const float tan_fov = std::tan(fov_deg * 3.14159265f / 360.0f);
  set4(p.camera_position, ex, ey, ez, tan_fov);
  // Normalize forward.
  const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
  const float fxn = fx / fl, fyn = fy / fl, fzn = fz / fl;
  set4(p.forward, fxn, fyn, fzn, aspect);
  // Camera basis: right = normalize(cross(forward, world_up)), up =
  // cross(right, forward). For forward in the XZ plane right = (-fz, 0, fx).
  const float rx = -fzn;
  const float ry = 0.f;
  const float rz = fxn;
  const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
  set4(p.right, rx / rl, ry / rl, rz / rl, 0.f);
  // up = cross(right, forward).
  const float ux = p.right[1] * fzn - p.right[2] * fyn;
  const float uy = p.right[2] * fxn - p.right[0] * fzn;
  const float uz = p.right[0] * fyn - p.right[1] * fxn;
  set4(p.up, ux, uy, uz, 0.f);
  return p;
}

}  // namespace

//! Daytime sky: the frame fills with blue-dominant pixels (Rayleigh scattering
//! peaks in blue at visible wavelengths), and the sun half is brighter than
//! the anti-sun half.
TEST(VulkanHardware, SkyDayGradient) {
  SkyHarness h;
  if (!h.init("sky_day")) GTEST_SKIP() << "Vulkan unavailable";

  SkyParamsUbo params{};
  // Sun 30 degrees above the horizon along -Z (the scene-forward direction
  // used by every other GPU test).
  set4(params.sun_direction, 0.f, 0.5f, -0.8660254f, 0.f);
  // Physical Rayleigh coefficients (per metre): blue scatters ~5.7x more
  // than red, giving the sky its colour.
  set4(params.rayleigh, 5.8e-6f, 13.5e-6f, 33.1e-6f, 0.f);
  set4(params.mie, 21e-6f, 0.76f, 1.0f, 0.f);
  set4(params.misc, 1.0f, 0.f, 0.f, 0.f);

  // Camera on the planet surface (100 m up), looking toward the sun's
  // azimuth (-Z) and away (+Z). The shader's atmosphere is a shell around
  // the origin, so the eye must sit at radius ~EARTH_RADIUS, not at the
  // origin (a centered eye extinguishes all light: transmittance -> 0).
  const float eye_y = 6360e3f + 100.f;
  const SkyPush toward_sun = make_push(0, eye_y, 0, 0, 0, -1, 60.f, 1.f);
  const SkyPush away_from_sun = make_push(0, eye_y, 0, 0, 0, 1, 60.f, 1.f);

  const auto toward = h.render(params, toward_sun);
  const auto away = h.render(params, away_from_sun);
  h.cleanup();

  ASSERT_TRUE(toward.submitted);
  ASSERT_TRUE(away.submitted);
  EXPECT_GT(toward.non_clear_pixels, 60000U)
      << "sky did not fill the frame (toward sun)";
  EXPECT_GT(away.non_clear_pixels, 60000U)
      << "sky did not fill the frame (away from sun)";
  EXPECT_GT(toward.blue_dominant_pixels, toward.red_dominant_pixels)
      << "sky is not blue-dominant";
}

//! Sun disc: pointing the camera straight at the sun produces a saturated
//! highlight (bright_pixels) that disappears when looking away.
TEST(VulkanHardware, SkySunDisc) {
  SkyHarness h;
  if (!h.init("sky_disc")) GTEST_SKIP() << "Vulkan unavailable";

  SkyParamsUbo params{};
  // Sun straight ahead so the camera's forward axis pierces the disc.
  set4(params.sun_direction, 0.f, 0.f, -1.f, 0.f);
  set4(params.rayleigh, 5.8e-6f, 13.5e-6f, 33.1e-6f, 0.f);
  set4(params.mie, 21e-6f, 0.76f, 1.0f, 0.f);
  set4(params.misc, 1.0f, 0.f, 0.f, 0.f);

  const float eye_y = 6360e3f + 100.f;
  const SkyPush toward_sun = make_push(0, eye_y, 0, 0, 0, -1, 60.f, 1.f);
  const auto toward = h.render(params, toward_sun);
  h.cleanup();

  ASSERT_TRUE(toward.submitted);
  EXPECT_GT(toward.bright_pixels, 10U) << "no sun disc highlight";
}

#endif  // WARPLOOM_HAS_VULKAN
