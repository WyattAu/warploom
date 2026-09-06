//! @file test_scene_resources.cpp
//! @brief Generation-safe scene resource and frame-upload contract tests.

#include <gtest/gtest.h>

#include "engine/render/vulkan_scene.hpp"

namespace {

using omnicpp::render::MeshHandle;
using omnicpp::render::SceneMaterial;
using omnicpp::render::SceneMesh;
using omnicpp::render::VulkanSceneResourceRegistry;

TEST(SceneResources, GenerationCheckedMeshHandlesRetireBeforeReuse) {
  VulkanSceneResourceRegistry resources;
  SceneMesh mesh_a;
  mesh_a.index_count = 3U;
  const MeshHandle first = resources.create_mesh(mesh_a);
  ASSERT_TRUE(first.valid());
  ASSERT_NE(resources.resolve(first), nullptr);
  EXPECT_EQ(resources.live_mesh_count(), 1U);

  EXPECT_TRUE(resources.destroy(first, 7U));
  EXPECT_EQ(resources.resolve(first), nullptr);
  EXPECT_FALSE(resources.destroy(first, 7U));
  resources.collect(6U);
  const MeshHandle second_before_retirement = resources.create_mesh(mesh_a);
  EXPECT_NE(second_before_retirement.index, first.index);

  EXPECT_TRUE(resources.destroy(second_before_retirement, 8U));
  resources.collect(7U);
  const MeshHandle second = resources.create_mesh(mesh_a);
  EXPECT_NE(second.index, second_before_retirement.index);
  EXPECT_NE(second.generation, first.generation);
  EXPECT_EQ(resources.resolve(first), nullptr);
  EXPECT_NE(resources.resolve(second), nullptr);
}

TEST(SceneResources, MaterialHandlesAreIndependentAndRetirable) {
  VulkanSceneResourceRegistry resources;
  const auto red = resources.create_material(SceneMaterial{{1.0f, 0.0f, 0.0f, 1.0f}});
  const auto green = resources.create_material(SceneMaterial{{0.0f, 1.0f, 0.0f, 1.0f}});
  ASSERT_NE(resources.resolve(red), nullptr);
  ASSERT_NE(resources.resolve(green), nullptr);
  EXPECT_FLOAT_EQ(resources.resolve(red)->base_color[0], 1.0f);
  EXPECT_FLOAT_EQ(resources.resolve(green)->base_color[1], 1.0f);
  EXPECT_TRUE(resources.destroy(red, 10U));
  resources.collect(9U);
  EXPECT_EQ(resources.resolve(red), nullptr);
  EXPECT_NE(resources.resolve(green), nullptr);
  resources.collect(10U);
  EXPECT_EQ(resources.live_material_count(), 1U);
}

} // namespace
