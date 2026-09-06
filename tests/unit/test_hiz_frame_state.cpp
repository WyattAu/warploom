//! @file test_hiz_frame_state.cpp
//! @brief Pure contract tests for H-Z frame state and graph image subresources.

#include <gtest/gtest.h>

#include "engine/render/vulkan_hiz_frame_state.hpp"
#include "engine/render/vulkan_render_graph.hpp"

TEST(HiZFrameState, ConfiguresRuntimeExtentAndInvalidatesFirstFrame) {
  omnicpp::render::VulkanHiZFrameState state;
  ASSERT_TRUE(state.configure(640U, 480U, 32U).is_ok());
  EXPECT_EQ(state.pyramid_width(), 20U);
  EXPECT_EQ(state.pyramid_height(), 15U);
  EXPECT_EQ(state.levels(), 5U);
  EXPECT_FALSE(state.has_previous_frame());
  EXPECT_TRUE(omnicpp::render::has_hiz_invalidation(
      state.invalidation(), omnicpp::render::HiZInvalidation::resize));

  const auto first = state.begin_frame();
  EXPECT_EQ(first.write_index, 0U);
  EXPECT_EQ(first.previous_index, 1U);
  EXPECT_FALSE(first.has_previous);
  state.complete_frame(first);
  EXPECT_TRUE(state.has_previous_frame());
  EXPECT_EQ(state.write_index(), 1U);
  EXPECT_EQ(state.invalidation(), omnicpp::render::HiZInvalidation::none);

  const auto second = state.begin_frame();
  EXPECT_EQ(second.write_index, 1U);
  EXPECT_EQ(second.previous_index, 0U);
  EXPECT_TRUE(second.has_previous);
  state.complete_frame(second);
  EXPECT_EQ(state.write_index(), 0U);
}

TEST(HiZFrameState, CameraAndProjectionChangesInvalidatePreviousDepth) {
  omnicpp::render::VulkanHiZFrameState state;
  ASSERT_TRUE(state.configure(256U, 256U, 32U, 4U).is_ok());
  auto token = state.begin_frame();
  state.complete_frame(token);
  ASSERT_TRUE(state.has_previous_frame());

  state.mark_camera_cut();
  EXPECT_FALSE(state.has_previous_frame());
  EXPECT_TRUE(omnicpp::render::has_hiz_invalidation(
      state.invalidation(), omnicpp::render::HiZInvalidation::camera_cut));

  state.mark_projection_change();
  EXPECT_TRUE(omnicpp::render::has_hiz_invalidation(
      state.invalidation(), omnicpp::render::HiZInvalidation::projection_change));

  const auto stale = state.begin_frame();
  state.complete_frame(stale);
  EXPECT_TRUE(state.has_previous_frame());
  EXPECT_EQ(state.invalidation(), omnicpp::render::HiZInvalidation::none);
}

TEST(HiZFrameState, RejectsInvalidConfigurationAndStaleCompletion) {
  omnicpp::render::VulkanHiZFrameState state;
  EXPECT_FALSE(state.configure(0U, 64U).is_ok());
  EXPECT_FALSE(state.configure(64U, 64U, 0U).is_ok());
  ASSERT_TRUE(state.configure(64U, 64U, 32U, 2U).is_ok());
  EXPECT_FALSE(state.configure(64U, 64U, 32U, 3U).is_ok());

  const auto token = state.begin_frame();
  state.mark_camera_cut();
  state.complete_frame(token);
  EXPECT_FALSE(state.has_previous_frame());

  const auto current = state.begin_frame();
  state.complete_frame(current);
  EXPECT_TRUE(state.has_previous_frame());
}

TEST(RenderGraph, CompilesPerMipImageUses) {
  using namespace omnicpp::render;
  const VkImage image = reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(0x1234));

  GraphComputePass reduce_level_zero{};
  reduce_level_zero.name = "hiz_l0";
  reduce_level_zero.image_uses.push_back({
      image, 0U, 1U, 0U, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      0x1U, 0x20U});

  GraphComputePass reduce_level_one{};
  reduce_level_one.name = "hiz_l1";
  reduce_level_one.image_uses.push_back({
      image, 1U, 1U, 0U, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      0x1U, 0x20U});

  const std::vector<GraphNode> nodes = {
      GraphNode::from_compute(reduce_level_zero),
      GraphNode::from_compute(reduce_level_one),
  };
  const auto compiled = compile_graph(nodes);
  ASSERT_EQ(compiled.barriers_per_node.size(), 2U);
  ASSERT_EQ(compiled.barriers_per_node[0].size(), 1U);
  ASSERT_EQ(compiled.barriers_per_node[1].size(), 1U);
  EXPECT_EQ(compiled.barriers_per_node[0][0].base_mip, 0U);
  EXPECT_EQ(compiled.barriers_per_node[1][0].base_mip, 1U);
  EXPECT_EQ(compiled.barriers_per_node[1][0].old_layout, VK_IMAGE_LAYOUT_UNDEFINED);
}
