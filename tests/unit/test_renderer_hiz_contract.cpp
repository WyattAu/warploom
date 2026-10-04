//! @file test_renderer_hiz_contract.cpp
//! @brief Renderer-facing H-Z lifecycle contract tests.

#include <gtest/gtest.h>

#include "warploom/render/vulkan_renderer.hpp"

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

// The reduction must read the depth the scene actually wrote. Under HDR
// compose the scene renders into the HDR intermediate's own depth, so the
// swapchain's depth describes an earlier frame. Before this was tracked, H-Z
// under compose reduced that stale attachment and published a pyramid that
// did not match what was drawn.
TEST(VulkanRendererHiZ, SelectsTheDepthTheSceneWrote) {
  using Source = omnicpp::render::VulkanRenderer::HiZDepthSource;
  const VkImage hdr = reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(0x100));
  const VkImage swap = reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(0x200));
  const VkImageView hdr_view =
      reinterpret_cast<VkImageView>(static_cast<std::uintptr_t>(0x101));
  const VkImageView swap_view =
      reinterpret_cast<VkImageView>(static_cast<std::uintptr_t>(0x201));
  const Source hdr_ok{hdr, hdr_view, true};
  const Source swap_ok{swap, swap_view, true};
  const Source hdr_unsampled{hdr, hdr_view, false};

  // Composing: the HDR depth wins, because that is what the scene wrote.
  const auto composing =
      omnicpp::render::VulkanRenderer::select_hiz_depth_source(true, hdr_ok, swap_ok);
  EXPECT_EQ(composing.image, hdr);
  EXPECT_EQ(composing.view, hdr_view);
  EXPECT_TRUE(composing.available());

  // Not composing: the swapchain depth is the one in use.
  const auto direct =
      omnicpp::render::VulkanRenderer::select_hiz_depth_source(false, hdr_ok, swap_ok);
  EXPECT_EQ(direct.image, swap);

  // The regression: compose on, HDR depth unreadable. Falling back to the
  // swapchain depth here would reduce an attachment the scene never wrote.
  EXPECT_FALSE(
      omnicpp::render::VulkanRenderer::select_hiz_depth_source(true, hdr_unsampled, swap_ok)
          .available());
  // Same when there is no HDR depth at all.
  EXPECT_FALSE(omnicpp::render::VulkanRenderer::select_hiz_depth_source(
                   true, Source{}, swap_ok)
                   .available());
  // And no compose means no HDR depth is consulted at all.
  EXPECT_EQ(omnicpp::render::VulkanRenderer::select_hiz_depth_source(
                false, Source{}, swap_ok)
                .image,
            swap);
  EXPECT_FALSE(omnicpp::render::VulkanRenderer::select_hiz_depth_source(
                   false, Source{}, Source{})
                   .available());
}
