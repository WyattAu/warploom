//! @file test_pt_sky_parity.cpp
//! @brief One sky in both paths. The raster sky pass (sky.vert/sky.frag) and
//!        the path tracer miss branch (pt_real.rgen, sky mode 1) evaluate the
//!        same shared model (sky_eval.glsl) with the same sun, so the two
//!        backgrounds agree pixel to pixel up to raster interpolation and the
//!        tracer's sub-pixel jitter — not up to a model difference.
//!
//!        The orientation test is here because it caught a real defect: the
//!        raster sky was vertically flipped (screen-top looked down), which
//!        no count-based assertion could see. Parity against a flipped sky
//!        would have enshrined the flip, so orientation is pinned first.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_path_tracer.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "vulkan_test_readback.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

using omnicpp::render::Allocation;
using omnicpp::render::VulkanPbrScene;

constexpr std::uint32_t kSize = 256U;
// Surface-anchored eye: the sky model is a planetary shell, so a unit-scale
// origin sits inside the planet and extinguishes everything (transmittance
// -> 0). Both paths use this anchor; it is the shared convention.
constexpr float kEyeY = 6360e3f + 100.0f;
constexpr float kTanHalfFov = 0.57735026f;  // 60-degree full fov

struct SkyParamsUbo {
  float sun_direction[4];
  float rayleigh[4];
  float mie[4];
  float misc[4];
};

SkyParamsUbo canonical_sky_params() {
  SkyParamsUbo params{};
  // Sun 45 degrees up, toward screen-top (+Z is the view's up): the top
  // half looks sunward, which is what the orientation test pins.
  params.sun_direction[0] = 0.0f;
  params.sun_direction[1] = 0.7f;
  params.sun_direction[2] = 0.7f;
  params.rayleigh[0] = 5.8e-6f;
  params.rayleigh[1] = 13.5e-6f;
  params.rayleigh[2] = 33.1e-6f;
  params.mie[0] = 21e-6f;
  params.mie[1] = 0.76f;
  // Deliberately low: keeps the whole frame in LDR so the UNORM readback
  // neither clamps nor loses precision, and unsaturated values discriminate
  // direction (where the bright patch is) from scale (how bright it is).
  params.misc[0] = 0.1f;
  return params;
}

struct ParityHarness {
  omnicpp::render::VulkanContext context;
  omnicpp::render::VulkanMemoryAllocator allocator;
  omnicpp::render::VulkanDescriptorManager descriptors;
  omnicpp::render::VulkanOffscreenTarget target;
  omnicpp::render::VulkanPipeline sky_pipeline;
  omnicpp::render::VulkanRenderer renderer;
  omnicpp::render::VulkanPathTracer tracer;

  VkDescriptorSetLayout sky_layout{VK_NULL_HANDLE};
  VkDescriptorSet sky_set{VK_NULL_HANDLE};
  Allocation sky_ubo{};
  VkCommandPool cmd_pool{VK_NULL_HANDLE};
  Allocation geo_alloc{};
  std::uint32_t queue_family{0};

  [[nodiscard]] bool initialize(const char* app_name) {
    if (!context.initialize(app_name, true).is_ok()) return false;
    if (!allocator.initialize(context.device(), context.physical_device())
             .is_ok()) {
      context.cleanup();
      return false;
    }
    if (!descriptors.initialize(context.device()).is_ok()) {
      allocator.cleanup();
      context.cleanup();
      return false;
    }
    queue_family =
        static_cast<std::uint32_t>(context.queue_families().graphics_family);

    if (!target
             .create(context.device(), context.physical_device(),
                     VK_FORMAT_B8G8R8A8_UNORM, kSize, kSize, &allocator)
             .is_ok() ||
        !target.create_depth(context.device(), context.physical_device(),
                             VK_FORMAT_D32_SFLOAT)
             .is_ok() ||
        !target.create_render_pass(context.device()).is_ok() ||
        !target.create_framebuffer(context.device()).is_ok()) {
      descriptors.cleanup();
      allocator.cleanup();
      context.cleanup();
      return false;
    }

    const std::vector<omnicpp::render::ReflectedBinding> sky_bindings = {
        {0U, 0U, 1U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         VK_SHADER_STAGE_FRAGMENT_BIT}};
    auto sky_layout_result = descriptors.create_layout(sky_bindings, 4U);
    if (!sky_layout_result.is_ok()) return false;
    sky_layout = sky_layout_result.value();
    auto sky_set_result = descriptors.allocate_set(sky_layout);
    if (!sky_set_result.is_ok()) return false;
    sky_set = sky_set_result.value();
    auto sky_ubo_result = allocator.create_buffer(
        sizeof(SkyParamsUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!sky_ubo_result.is_ok()) return false;
    sky_ubo = sky_ubo_result.value();
    if (!descriptors
             .write_buffer(sky_set, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           sky_ubo.buffer, 0U, VK_WHOLE_SIZE)
             .is_ok()) {
      return false;
    }
    const SkyParamsUbo params = canonical_sky_params();
    std::memcpy(sky_ubo.mapped, &params, sizeof(params));

    const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
    if (!sky_pipeline
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/sky.vert.spv", "vertex")
             .is_ok() ||
        !sky_pipeline
             .load_shader_stage_file(context.device(),
                                     shader_dir + "/sky.frag.spv", "fragment")
             .is_ok()) {
      return false;
    }
    std::vector<VkDescriptorSetLayout> sky_layouts = {sky_layout};
    VkPushConstantRange sky_push{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, 64U};
    if (!sky_pipeline
             .create_pipeline_layout(context.device(), sky_layouts.data(), 1U,
                                     &sky_push)
             .is_ok()) {
      return false;
    }
    if (!sky_pipeline
             .create_graphics_pipeline(context.device(), target.render_pass(),
                                       target.format(),
                                       sky_pipeline.pipeline_layout(), true,
                                       false, false)
             .is_ok()) {
      return false;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family;
    if (vkCreateCommandPool(context.device(), &pool_info, nullptr,
                            &cmd_pool) != VK_SUCCESS) {
      return false;
    }

    // One triangle BEHIND the camera (camera looks -Z, triangle at z = +5):
    // every traced ray misses, so the accumulation is pure sky.
    const std::array<float, 9> tri = {-1.0f, -1.0f, 5.0f,  //
                                     1.0f,  -1.0f, 5.0f,  //
                                     0.0f,  1.0f,  5.0f};
    auto geo_result = allocator.create_buffer(
        static_cast<VkDeviceSize>(tri.size() * sizeof(float)),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!geo_result.is_ok()) return false;
    geo_alloc = geo_result.value();
    std::memcpy(geo_alloc.mapped, tri.data(), tri.size() * sizeof(float));
    return true;
  }

  VulkanPbrScene make_sky_scene() {
    VulkanPbrScene scene;
    scene.sky_pipeline = sky_pipeline.pipeline();
    scene.sky_pipeline_layout = sky_pipeline.pipeline_layout();
    scene.sky_set = sky_set;    scene.sky_view.camera_position = {0.0f, kEyeY, 0.0f, kTanHalfFov};
    // Zenith view: every pixel looks at sky, spanning dim zenith (top) to
    // the warm glow toward the 45-degree sun (bottom). No horizon, no
    // ground branch — this test pins sky-model parity, not geometry.
    scene.sky_view.forward = {0.0f, 1.0f, 0.0f, 1.0f};
    scene.sky_view.right = {1.0f, 0.0f, 0.0f, 0.0f};
    scene.sky_view.up = {0.0f, 0.0f, 1.0f, 0.0f};
    return scene;
  }

  [[nodiscard]] omnicpp_test::ReadbackResult render_raster_sky() {
    auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        context.device(), cmd_pool);
    if (!cb_result.is_ok()) return {};
    const VkCommandBuffer cb = cb_result.value();
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(context.device(), &fence_info, nullptr, &fence);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const bool began = vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS;
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = target.render_pass();
    rb.framebuffer = target.framebuffer();
    rb.renderArea.extent = {kSize, kSize};
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0U};
    rb.clearValueCount = 2U;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0.0f,  0.0f, static_cast<float>(kSize),
                        static_cast<float>(kSize), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {kSize, kSize}};
    vkCmdSetScissor(cb, 0, 1, &scissor);
    const VulkanPbrScene scene = make_sky_scene();
    const bool recorded =
        began && renderer.record_sky_pre_draw(cb, scene).is_ok();
    vkCmdEndRenderPass(cb);
    const bool ended = vkEndCommandBuffer(cb) == VK_SUCCESS;
    bool submitted = false;
    if (recorded && ended) {
      (void)vkResetFences(context.device(), 1, &fence);
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = 1U;
      submit.pCommandBuffers = &cb;
      submitted =
          vkQueueSubmit(context.graphics_queue(), 1U, &submit, fence) ==
              VK_SUCCESS &&
          vkWaitForFences(context.device(), 1U, &fence, VK_TRUE,
                          UINT64_MAX) == VK_SUCCESS;
    }
    vkDestroyFence(context.device(), fence, nullptr);
    if (!submitted) return {};
    return omnicpp_test::readback_swapchain_image(
        context.physical_device(), context.device(), context.graphics_queue(),
        queue_family, target.image(), target.format(), kSize, kSize,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*store_pixels=*/true);
  }

  [[nodiscard]] bool submit_and_wait(VkCommandBuffer cb) {
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(context.device(), &fence_info, nullptr, &fence) !=
        VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &cb;
    const bool ok =
        vkQueueSubmit(context.graphics_queue(), 1U, &submit, fence) ==
            VK_SUCCESS &&
        vkWaitForFences(context.device(), 1U, &fence, VK_TRUE, UINT64_MAX) ==
            VK_SUCCESS;
    vkDestroyFence(context.device(), fence, nullptr);
    return ok;
  }

  [[nodiscard]] ::warploom::core::Result<std::vector<float>> render_pt_sky() {
    VkBufferDeviceAddressInfo addr_info{};
    addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addr_info.buffer = geo_alloc.buffer;
    omnicpp::render::PathTracerConfig cfg{};
    cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
    cfg.width = kSize;
    cfg.height = kSize;
    cfg.max_bounces = 1U;  // every ray misses on its first segment
    omnicpp::render::PathTracerMesh mesh{};
    mesh.vertex_buffer_address =
        vkGetBufferDeviceAddress(context.device(), &addr_info);
    mesh.triangle_count = 1U;
    mesh.max_vertex = 2U;
    cfg.meshes = {mesh};
    cfg.instances = {
        omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.8F)};
    // Analytic sky with every word pinned to the canonical set — never rely
    // on the struct defaults matching the raster harness by coincidence.
    cfg.sky.mode = 1U;
    const SkyParamsUbo params = canonical_sky_params();
    for (std::size_t i = 0; i < 4U; ++i) {
      cfg.sky.sun_direction[i] = params.sun_direction[i];
      cfg.sky.rayleigh[i] = params.rayleigh[i];
      cfg.sky.mie[i] = params.mie[i];
      cfg.sky.misc[i] = params.misc[i];
    }
    cfg.sky.origin = {0.0F, kEyeY, 0.0F, 0.0F};
    const auto created = tracer.create(
        context.physical_device(), context.device(), context.graphics_queue(),
        queue_family, allocator, cfg);
    if (!created.is_ok() || !tracer.ready()) {
      return ::warploom::core::Result<std::vector<float>>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    omnicpp::render::PathTracerCamera cam{};
    cam.position = {0.0F, 0.0F, 0.0F};
    cam.right = {1.0F, 0.0F, 0.0F};
    // Same zenith view as the raster harness above.
    cam.up = {0.0F, 0.0F, 1.0F};
    cam.forward = {0.0F, 1.0F, 0.0F};
    // Same angular mapping as the raster sky view: the tracer used to
    // hardcode a 90-degree field, which no raster view shares.
    cam.tan_half_fov = kTanHalfFov;
    cam.aspect = 1.0F;
    tracer.set_camera(cam);

    auto cb_result = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        context.device(), cmd_pool);
    if (!cb_result.is_ok()) {
      return ::warploom::core::Result<std::vector<float>>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    const VkCommandBuffer cb = cb_result.value();
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
      return ::warploom::core::Result<std::vector<float>>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    tracer.record_round(cb, /*frame=*/0U, /*clear_first=*/true);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
      return ::warploom::core::Result<std::vector<float>>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    if (!submit_and_wait(cb)) {
      return ::warploom::core::Result<std::vector<float>>::error(
          ::warploom::core::RuntimeError::invalid_config);
    }
    return tracer.read_mean();
  }

  void cleanup() {
    const VkDevice device = context.device();
    tracer.destroy();
    if (geo_alloc.is_valid()) allocator.destroy_allocation(geo_alloc);
    if (cmd_pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, cmd_pool, nullptr);
    }
    sky_pipeline.cleanup(device);
    target.cleanup(device);
    if (sky_ubo.is_valid()) allocator.destroy_allocation(sky_ubo);
    descriptors.cleanup();
    allocator.cleanup();
    context.cleanup();
  }
};

bool on_software_vulkan(const ParityHarness& h) {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(h.context.physical_device(), &props);
  return std::strstr(props.deviceName, "llvmpipe") != nullptr;
}

}  // namespace

//! The raster sky must not be vertically flipped: with the sun high, the top
//! half of the frame looks toward it and the bottom half looks away. This
//! caught the sky.vert NDC bug (screen-top looked down); every count-based
//! sky assertion passed with the flip in place.
TEST(VulkanHardware, SkyPassTopHalfBrighterThanBottom) {
  ParityHarness h;
  if (!h.initialize("pt_sky_parity")) {
    GTEST_SKIP() << "Vulkan context unavailable";
  }
  if (on_software_vulkan(h)) {
    h.cleanup();
    GTEST_SKIP() << "llvmpipe: upstream Mesa JIT crash in the sky path";
  }
  const auto readback = h.render_raster_sky();
  h.cleanup();
  ASSERT_TRUE(readback.submitted);
  ASSERT_EQ(readback.pixels.size(), kSize * kSize);

  std::uint64_t top_sum = 0U;
  std::uint64_t bottom_sum = 0U;
  for (std::uint32_t y = 0; y < kSize; ++y) {
    for (std::uint32_t x = 0; x < kSize; ++x) {
      const std::uint32_t pixel =
          readback.pixels[static_cast<std::size_t>(y) * kSize + x];
      const std::uint64_t luma = (pixel & 0xFFU) + ((pixel >> 8U) & 0xFFU) +
                                 ((pixel >> 16U) & 0xFFU);
      if (y < kSize / 2U) {
        top_sum += luma;
      } else {
        bottom_sum += luma;
      }
    }
  }
  const double top_mean =
      static_cast<double>(top_sum) / (kSize * kSize / 2U);
  const double bottom_mean =
      static_cast<double>(bottom_sum) / (kSize * kSize / 2U);
  std::fprintf(stderr, "sky orientation: top-half mean %.2f bottom-half mean %.2f\n",
               top_mean, bottom_mean);
  EXPECT_GT(top_mean, bottom_mean)
      << "sky is vertically flipped: the sun-high half must be brighter";
}

//! Path tracer (mode 1, every ray misses) vs the raster sky pass with the
//! same basis, sun and resolution: identical model, identical inputs, so the
//! backgrounds agree up to raster interpolation and sub-pixel jitter.
TEST(VulkanHardware, PathTracerAnalyticSkyMatchesRasterSky) {
  ParityHarness h;
  if (!h.initialize("pt_sky_parity")) {
    GTEST_SKIP() << "Vulkan context unavailable";
  }
  if (on_software_vulkan(h)) {
    h.cleanup();
    GTEST_SKIP() << "llvmpipe: upstream Mesa JIT crash in the sky path";
  }
  const auto readback = h.render_raster_sky();
  ASSERT_TRUE(readback.submitted);
  ASSERT_EQ(readback.pixels.size(), kSize * kSize);
  auto mean_result = h.render_pt_sky();
  h.cleanup();
  ASSERT_TRUE(mean_result.is_ok()) << "PT stack unavailable (no RT?)";
  const std::vector<float>& mean = mean_result.value();
  ASSERT_EQ(mean.size(), kSize * kSize * 4U);

  double max_diff = 0.0;
  double sum_diff = 0.0;
  std::uint32_t traced_blue_dominant = 0U;
  for (std::uint32_t y = 0; y < kSize; ++y) {
    for (std::uint32_t x = 0; x < kSize; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * kSize + x;
      const std::uint32_t pixel = readback.pixels[i];
      // Documented packing: r | g<<8 | b<<16 | a<<24.
      const double raster_r =
          static_cast<double>((pixel >> 0U) & 0xFFU) / 255.0;
      const double raster_g =
          static_cast<double>((pixel >> 8U) & 0xFFU) / 255.0;
      const double raster_b =
          static_cast<double>((pixel >> 16U) & 0xFFU) / 255.0;
      const double traced_rgb[3] = {
          static_cast<double>(mean[i * 4U]),
          static_cast<double>(mean[i * 4U + 1U]),
          static_cast<double>(mean[i * 4U + 2U])};
      const double raster_rgb[3] = {raster_r, raster_g, raster_b};
      for (int c = 0; c < 3; ++c) {
        double traced = traced_rgb[c];
        traced = traced < 0.0 ? 0.0 : (traced > 1.0 ? 1.0 : traced);
        const double diff = std::fabs(raster_rgb[c] - traced);
        if (diff > max_diff) max_diff = diff;
        sum_diff += diff;
      }
      if (traced_rgb[2] > traced_rgb[0] && traced_rgb[2] > traced_rgb[1]) {
        ++traced_blue_dominant;
      }
    }
  }
  const double mean_diff = sum_diff / (kSize * kSize * 3U);
  std::fprintf(stderr, "sky parity: max abs diff %.4f mean abs diff %.5f\n",
               max_diff, mean_diff);
  // Both pictures are sky, not black: most pixels are blue-dominant. The
  // traced side mirrors the raster per-pixel count (not channel sums: the
  // bright warm patch toward the sun outweighs the dim blue zenith in a
  // sum even though most pixels are blue).
  EXPECT_GT(readback.blue_dominant_pixels, kSize * kSize / 2U);
  EXPECT_GT(traced_blue_dominant, kSize * kSize / 2U);
  // Measured 0.0112 / 0.0018 on the RTX 2060; the headroom covers 8-bit
  // readback quantization and the tracer's sub-pixel jitter.
  EXPECT_LT(max_diff, 0.05);
  EXPECT_LT(mean_diff, 0.01);
}

#endif  // WARPLOOM_HAS_VULKAN
