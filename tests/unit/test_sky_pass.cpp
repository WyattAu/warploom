//! @file test_sky_pass.cpp
//! @brief Unit tests for VulkanSkyPass (the one owned raster-sky construction).
//!
//! record_pbr_scene() already knew how to draw the sky; every consumer built
//! the pipeline and params by hand. This pins the shared construction: a
//! built pass is ready, its UBO carries the params given at initialize (and
//! later set_params writes land in the mapped allocation), and apply_to()
//! points a scene at all four fields. The negative cases pin the failure
//! contract the viewport relies on: a bad target is an error (never a
//! half-built pass), and apply_to() on an unbuilt pass is a no-op (the
//! scene keeps the clear-colour background).
//!
//! Creation-only: no draw or dispatch executes, so this runs on software
//! Vulkan too (the llvmpipe JIT crash the parity test skips on needs an
//! executing shader; nothing here executes one).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_offscreen.hpp"
#include "warploom/render/vulkan_scene.hpp"
#include "warploom/render/vulkan_sky_pass.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

using warploom::render::Allocation;
using warploom::render::SkyParams;
using warploom::render::VulkanContext;
using warploom::render::VulkanDescriptorManager;
using warploom::render::VulkanMemoryAllocator;
using warploom::render::VulkanOffscreenTarget;
using warploom::render::VulkanPbrScene;
using warploom::render::VulkanSkyPass;

struct SkyPassHarness {
  VulkanContext context;
  VulkanMemoryAllocator allocator;
  VulkanDescriptorManager descriptors;
  VulkanOffscreenTarget target;
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
    queue_family = static_cast<std::uint32_t>(
        context.queue_families().graphics_family);
    if (!target
             .create(context.device(), context.physical_device(),
                     VK_FORMAT_B8G8R8A8_UNORM, 64U, 64U, &allocator)
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
    return true;
  }

  void cleanup() {
    target.cleanup(context.device());
    descriptors.cleanup();
    allocator.cleanup();
    context.cleanup();
  }
};

TEST(VulkanSkyPass, BuildsAndAppliesToScene) {
  SkyPassHarness h;
  if (!h.initialize("sky_pass_builds")) {
    GTEST_SKIP() << "Vulkan context unavailable";
  }
  VulkanSkyPass pass;
  SkyParams params;
  params.sun_direction = {0.0f, 0.0f, 1.0f, 0.0f};
  params.misc = {0.5f, 0.0f, 0.0f, 0.0f};
  std::string error;
  const auto built = pass.initialize(
      h.context.device(), h.allocator, h.descriptors, h.target.render_pass(),
      h.target.format(), WARPLOOM_TEST_SHADER_DIR, params, error);
  EXPECT_TRUE(built.is_ok()) << error;
  EXPECT_TRUE(pass.ready());

  // The UBO carries the initialize params, and set_params() lands in the
  // mapped allocation the shader reads -- a write that silently went
  // nowhere would leave the default sun in place with no diagnostic.
  ASSERT_NE(pass.mapped_params(), nullptr);
  EXPECT_EQ(std::memcmp(pass.mapped_params(), &params, sizeof(params)), 0);
  SkyParams changed = params;
  changed.sun_direction = {0.0f, 1.0f, 0.0f, 0.0f};
  pass.set_params(changed);
  EXPECT_EQ(std::memcmp(pass.mapped_params(), &changed, sizeof(changed)), 0);

  VulkanPbrScene scene;
  EXPECT_EQ(scene.sky_pipeline, VK_NULL_HANDLE);
  pass.set_view({0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                0.57735026f, 1.0f);
  pass.apply_to(scene);
  EXPECT_NE(scene.sky_pipeline, VK_NULL_HANDLE);
  EXPECT_NE(scene.sky_pipeline_layout, VK_NULL_HANDLE);
  EXPECT_NE(scene.sky_set, VK_NULL_HANDLE);
  // Surface anchor, not the unit-scale camera: the model is a planetary
  // shell and a unit-scale eye extinguishes it.
  EXPECT_FLOAT_EQ(scene.sky_view.camera_position[0], 0.0f);
  EXPECT_FLOAT_EQ(scene.sky_view.camera_position[1], 6360e3f + 100.0f);
  EXPECT_FLOAT_EQ(scene.sky_view.camera_position[2], 0.0f);
  EXPECT_FLOAT_EQ(scene.sky_view.camera_position[3], 0.57735026f);
  EXPECT_FLOAT_EQ(scene.sky_view.forward[2], 1.0f);
  EXPECT_FLOAT_EQ(scene.sky_view.forward[3], 1.0f);
  pass.cleanup(h.context.device());
  EXPECT_FALSE(pass.ready());
  h.cleanup();
}

TEST(VulkanSkyPass, BadTargetIsAnErrorNeverAReadyPass) {
  SkyPassHarness h;
  if (!h.initialize("sky_pass_bad_target")) {
    GTEST_SKIP() << "Vulkan context unavailable";
  }
  VulkanSkyPass pass;
  std::string error;
  const SkyParams params{};
  EXPECT_FALSE(pass
                   .initialize(h.context.device(), h.allocator, h.descriptors,
                               VK_NULL_HANDLE, h.target.format(),
                               WARPLOOM_TEST_SHADER_DIR, params, error)
                   .is_ok());
  EXPECT_FALSE(pass.ready());
  EXPECT_FALSE(error.empty());
  // apply_to on the unbuilt pass is a no-op: the scene keeps no sky, i.e.
  // the clear-colour background, exactly the viewport's fallback.
  VulkanPbrScene scene;
  pass.apply_to(scene);
  EXPECT_EQ(scene.sky_pipeline, VK_NULL_HANDLE);
  EXPECT_EQ(scene.sky_set, VK_NULL_HANDLE);
  pass.cleanup(h.context.device());  // safe on an unbuilt pass
  h.cleanup();
}

}  // namespace

#endif  // WARPLOOM_HAS_VULKAN
