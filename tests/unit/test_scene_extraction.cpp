//! @file test_scene_extraction.cpp
//! @brief Deterministic ECS-to-render snapshot tests.

#include <gtest/gtest.h>

#include <cmath>

#include "warploom/core/ecs.hpp"
#include "warploom/render/frustum.hpp"
#include "warploom/render/vulkan_scene.hpp"

namespace {

using omnicpp::core::World;
using omnicpp::render::SceneCameraComponent;
using omnicpp::render::SceneMesh;
using omnicpp::render::SceneRenderableComponent;
using omnicpp::render::SceneTransformComponent;
using omnicpp::render::scene_identity_matrix;
using omnicpp::render::extract_vulkan_scene;

TEST(SceneExtraction, SelectsDeterministicActiveCamera) {
  World world;
  const auto low_priority = world.create_entity();
  const auto tie_late = world.create_entity();
  const auto tie_early = world.create_entity();

  auto low_matrix = scene_identity_matrix();
  low_matrix[12] = 11.0f;
  auto tie_late_matrix = scene_identity_matrix();
  tie_late_matrix[12] = 22.0f;
  auto tie_early_matrix = scene_identity_matrix();
  tie_early_matrix[12] = 33.0f;
  world.add_component<SceneCameraComponent>(low_priority,
      SceneCameraComponent{low_matrix, 3U, true});
  world.add_component<SceneCameraComponent>(tie_late,
      SceneCameraComponent{tie_late_matrix, 2U, true});
  world.add_component<SceneCameraComponent>(tie_early,
      SceneCameraComponent{tie_early_matrix, 2U, true});

  const auto scene = extract_vulkan_scene(world);
  EXPECT_FLOAT_EQ(scene.camera.view_projection[12], 22.0f);
}

TEST(SceneExtraction, FiltersInvisibleAndSortsByEntityId) {
  World world;
  SceneMesh mesh_a;
  SceneMesh mesh_b;
  const auto first = world.create_entity();
  const auto second = world.create_entity();
  const auto hidden = world.create_entity();

  auto first_model = scene_identity_matrix();
  first_model[12] = 1.0f;
  auto second_model = scene_identity_matrix();
  second_model[12] = 2.0f;
  world.add_component<SceneRenderableComponent>(first, {&mesh_a, true});
  world.add_component<SceneTransformComponent>(first, {first_model});
  world.add_component<SceneRenderableComponent>(second, {&mesh_b, true});
  world.add_component<SceneTransformComponent>(second, {second_model});
  world.add_component<SceneRenderableComponent>(hidden, {&mesh_a, false});

  const auto scene = extract_vulkan_scene(world);
  ASSERT_EQ(scene.objects.size(), 2U);
  EXPECT_EQ(scene.objects[0].mesh, &mesh_a);
  EXPECT_EQ(scene.objects[1].mesh, &mesh_b);
  EXPECT_FLOAT_EQ(scene.objects[0].model[12], 1.0f);
  EXPECT_FLOAT_EQ(scene.objects[1].model[12], 2.0f);
}

TEST(SceneExtraction, CopiesStateIntoSnapshot) {
  World world;
  SceneMesh mesh;
  const auto camera = world.create_entity();
  const auto object = world.create_entity();
  auto camera_matrix = scene_identity_matrix();
  camera_matrix[13] = 4.0f;
  auto model = scene_identity_matrix();
  model[14] = -5.0f;
  world.add_component<SceneCameraComponent>(camera,
      SceneCameraComponent{camera_matrix, 0U, true});
  world.add_component<SceneRenderableComponent>(object, {&mesh, true});
  world.add_component<SceneTransformComponent>(object, {model});

  const auto snapshot = extract_vulkan_scene(world);
  world.get_component<SceneCameraComponent>(camera).view_projection[13] = 99.0f;
  world.get_component<SceneTransformComponent>(object).model[14] = 99.0f;

  ASSERT_EQ(snapshot.objects.size(), 1U);
  EXPECT_FLOAT_EQ(snapshot.camera.view_projection[13], 4.0f);
  EXPECT_FLOAT_EQ(snapshot.objects[0].model[14], -5.0f);
}

TEST(SceneExtraction, MissingTransformUsesIdentity) {
  World world;
  SceneMesh mesh;
  const auto object = world.create_entity();
  world.add_component<SceneRenderableComponent>(object, {&mesh, true});

  const auto scene = extract_vulkan_scene(world);
  ASSERT_EQ(scene.objects.size(), 1U);
  EXPECT_EQ(scene.objects[0].model, scene_identity_matrix());
}

TEST(SceneExtraction, HandleResourcesAreCopiedIntoSnapshot) {
  World world;
  omnicpp::render::VulkanSceneResourceRegistry resources;
  SceneMesh mesh;
  mesh.index_count = 36U;
  const auto mesh_handle = resources.create_mesh(mesh);
  const auto material_handle = resources.create_material(
      omnicpp::render::SceneMaterial{{0.25f, 0.5f, 0.75f, 1.0f}});
  const auto object = world.create_entity();
  world.add_component<SceneRenderableComponent>(object,
      SceneRenderableComponent{nullptr, true, mesh_handle, material_handle, nullptr});

  const auto snapshot = extract_vulkan_scene(world, resources);
  ASSERT_EQ(snapshot.objects.size(), 1U);
  EXPECT_EQ(snapshot.objects[0].mesh, nullptr);
  EXPECT_EQ(snapshot.objects[0].mesh_handle, mesh_handle);
  EXPECT_TRUE(snapshot.objects[0].has_material);
  EXPECT_FLOAT_EQ(snapshot.objects[0].material_value.base_color[2], 0.75f);
  EXPECT_EQ(snapshot.objects[0].mesh_value.index_count, 36U);
  EXPECT_TRUE(snapshot.material_push_constants);

  (void)resources.destroy(mesh_handle, 1U);
  (void)resources.destroy(material_handle, 1U);
  EXPECT_EQ(snapshot.objects[0].mesh_value.index_count, 36U);
  EXPECT_FLOAT_EQ(snapshot.objects[0].material_value.base_color[0], 0.25f);
}

// ============================================================================
// Frustum culling
// ============================================================================

namespace {

//! Same camera convention as the hardware tests: origin looking down -Z with
//! a 45-degree-ish vertical FOV and near/far 0.1/100.
void make_test_camera(SceneCameraComponent& out) {
  out.priority = 0U;
  out.active = true;
  auto& m = out.view_projection;
  m.fill(0.0f);
  const float fov_y = 1.05f;
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float zn = 1.0f / (0.1f - 100.0f);
  m[0] = f;         // square aspect
  m[5] = f;
  m[10] = 100.0f * zn;
  m[11] = -1.0f;
  m[14] = 0.1f * 100.0f * zn;
}

omnicpp::render::SceneBounds unit_box() {
  omnicpp::render::SceneBounds bounds;
  bounds.valid = true;
  bounds.min = {-1.0f, -1.0f, -1.0f};
  bounds.max = {1.0f, 1.0f, 1.0f};
  return bounds;
}

} // namespace

//! Point/box membership against a perspective frustum extracted from a real
//! camera matrix (same code path extraction uses).
TEST(FrustumCulling, ExtractsPlanesAndClassifiesPoints) {
  SceneCameraComponent camera;
  make_test_camera(camera);
  const auto frustum =
      omnicpp::render::Frustum::from_view_projection(camera.view_projection.data());
  const auto tiny = [&frustum](float x, float y, float z) {
    const float center[3] = {x, y, z};
    const float extent[3] = {0.001f, 0.001f, 0.001f};
    return omnicpp::render::Frustum::box_outside(frustum, center, extent);
  };

  EXPECT_FALSE(tiny(0.0f, 0.0f, -3.0f));    // straight ahead, inside
  EXPECT_FALSE(tiny(-1.0f, 1.0f, -3.0f));   // inside cone
  EXPECT_TRUE(tiny(0.0f, 0.0f, 3.0f));      // behind the camera
  EXPECT_TRUE(tiny(40.0f, 0.0f, -3.0f));    // far right
  EXPECT_TRUE(tiny(-40.0f, 0.0f, -3.0f));   // far left
  EXPECT_TRUE(tiny(0.0f, 40.0f, -3.0f));    // far above
  EXPECT_TRUE(tiny(0.0f, 0.0f, -0.01f));    // between eye and near plane
  EXPECT_TRUE(tiny(0.0f, 0.0f, -200.0f));   // beyond far plane
}

//! Extraction drops objects whose transformed bounds are fully outside the
//! active camera frustum and reports deterministic stats; without a camera no
//! object is ever culled.
TEST(FrustumCulling, ExtractionCullsOnlyObjectsOutsideFrustum) {
  World world;
  SceneMesh mesh;
  const auto camera = world.create_entity();
  SceneCameraComponent camera_component;
  make_test_camera(camera_component);
  world.add_component<SceneCameraComponent>(camera, camera_component);

  const auto inside = world.create_entity();
  const auto behind = world.create_entity();
  const auto side = world.create_entity();
  const auto unbounded = world.create_entity();
  auto identity = scene_identity_matrix();
  auto behind_model = scene_identity_matrix();
  behind_model[14] = 30.0f;   // behind the camera
  auto side_model = scene_identity_matrix();
  side_model[12] = 50.0f;     // far to the right at view depth
  world.add_component<SceneRenderableComponent>(
      inside, {&mesh, true, {}, {}, nullptr, unit_box()});
  world.add_component<SceneRenderableComponent>(
      behind, {&mesh, true, {}, {}, nullptr, unit_box()});
  world.add_component<SceneRenderableComponent>(
      side, {&mesh, true, {}, {}, nullptr, unit_box()});
  world.add_component<SceneRenderableComponent>(unbounded, {&mesh, true});
  world.add_component<SceneTransformComponent>(inside, {identity});
  world.add_component<SceneTransformComponent>(behind, {behind_model});
  world.add_component<SceneTransformComponent>(side, {side_model});

  omnicpp::render::SceneExtractionStats stats;
  const auto scene = extract_vulkan_scene(world, &stats);
  ASSERT_EQ(scene.objects.size(), 2U);  // inside + unbounded survive
  EXPECT_TRUE(stats.camera_present);
  EXPECT_EQ(stats.visible_objects, 4U);
  EXPECT_EQ(stats.culled_objects, 2U);
  EXPECT_EQ(scene.objects[0].mesh, &mesh);
}

TEST(FrustumCulling, NoCameraMeansNoCulling) {
  World world;
  SceneMesh mesh;
  const auto behind = world.create_entity();
  auto behind_model = scene_identity_matrix();
  behind_model[14] = 30.0f;
  world.add_component<SceneRenderableComponent>(
      behind, {&mesh, true, {}, {}, nullptr, unit_box()});
  world.add_component<SceneTransformComponent>(behind, {behind_model});

  omnicpp::render::SceneExtractionStats stats;
  const auto scene = extract_vulkan_scene(world, &stats);
  ASSERT_EQ(scene.objects.size(), 1U);
  EXPECT_FALSE(stats.camera_present);
  EXPECT_EQ(stats.visible_objects, 1U);
  EXPECT_EQ(stats.culled_objects, 0U);
}

} // namespace
