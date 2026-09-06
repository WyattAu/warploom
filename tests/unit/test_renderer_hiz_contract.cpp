//! @file test_renderer_hiz_contract.cpp
//! @brief Renderer-facing H-Z lifecycle contract tests.

#include <gtest/gtest.h>

#include "engine/render/vulkan_renderer.hpp"

TEST(VulkanRendererHiZ, DisabledByDefaultAndSafeBeforeInitialization) {
  omnicpp::render::VulkanRenderer renderer;
  EXPECT_FALSE(renderer.hiz_enabled());
  EXPECT_FALSE(renderer.hiz_frame_state().configured());
  EXPECT_EQ(renderer.hiz_pyramid(0U), nullptr);
  EXPECT_EQ(renderer.hiz_pyramid(1U), nullptr);
  EXPECT_EQ(renderer.hiz_pyramid(2U), nullptr);

  const auto token = renderer.begin_hiz_frame();
  renderer.discard_hiz_frame(token);
  renderer.complete_hiz_frame(token);
  EXPECT_FALSE(renderer.hiz_frame_state().has_previous_frame());
}

TEST(VulkanRendererHiZ, ConfigCanRequestRuntimeSizedResources) {
  omnicpp::render::RendererConfig config;
  config.enable_hiz = true;
  config.hiz_tile_size = 32U;
  config.hiz_levels = 0U;
  EXPECT_TRUE(config.enable_hiz);
  EXPECT_EQ(config.hiz_tile_size, 32U);
  EXPECT_EQ(config.hiz_levels, 0U);
}

TEST(VulkanRendererHiZ, CallbackTypesExposeCompleteFrameContract) {
  static_assert(sizeof(omnicpp::render::HiZFrameRecord) >= sizeof(omnicpp::render::HiZFrameToken));
  omnicpp::render::HiZRecordCallback callback = nullptr;
  EXPECT_EQ(callback, nullptr);
}
