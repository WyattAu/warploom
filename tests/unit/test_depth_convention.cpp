//! @file test_depth_convention.cpp
//! @brief Pure CPU contract tests for projection depth and H-Z sizing.

#include <gtest/gtest.h>

#include <cmath>

#include "engine/render/depth_convention.hpp"
#include "engine/render/vulkan_hiz_pyramid.hpp"

TEST(DepthConvention, MatchesCanonicalProjection) {
  using omnicpp::render::depth::ndc_from_view_distance;
  constexpr float near_plane = 0.1f;
  constexpr float far_plane = 100.0f;

  EXPECT_NEAR(ndc_from_view_distance(near_plane, near_plane, far_plane), 0.0f, 1e-6f);
  EXPECT_NEAR(ndc_from_view_distance(far_plane, near_plane, far_plane), 1.0f, 1e-6f);
  EXPECT_NEAR(ndc_from_view_distance(25.0f, near_plane, far_plane),
              1.002002f - 0.2002002f / 25.0f, 1e-5f);
}

TEST(DepthConvention, SphereNearestDepthIsConservative) {
  using omnicpp::render::depth::sphere_nearest_depth;
  const float front = sphere_nearest_depth(-10.0f, 1.0f, 0.1f, 100.0f);
  const float center = sphere_nearest_depth(-10.0f, 0.0f, 0.1f, 100.0f);
  EXPECT_LT(front, center);
  EXPECT_NEAR(sphere_nearest_depth(-0.05f, 1.0f, 0.1f, 100.0f), 0.0f, 1e-6f);
}

TEST(HiZPyramid, ComputesRuntimeMipChain) {
  using omnicpp::render::VulkanHiZPyramid;
  EXPECT_EQ(VulkanHiZPyramid::mip_levels_for_extent(0, 64), 0U);
  EXPECT_EQ(VulkanHiZPyramid::mip_levels_for_extent(1, 1), 1U);
  EXPECT_EQ(VulkanHiZPyramid::mip_levels_for_extent(256, 256), 9U);
  EXPECT_EQ(VulkanHiZPyramid::mip_levels_for_extent(640, 480), 10U);
  EXPECT_EQ(VulkanHiZPyramid::mip_levels_for_extent(513, 7), 10U);
}
