//! @file test_scene_camera.cpp
//! @brief Deterministic tests for scene_camera: look-at/view-projection math
//!        conventions and the ECS orbit controller (look/zoom/pan, clamps,
//!        determinism, inactive-skip).
//!
//! Pure arithmetic — no Vulkan, no GPU. Runs in every preset.

#include <gtest/gtest.h>

#include <cmath>

#include "warploom/core/ecs.hpp"
#include "warploom/render/scene_camera.hpp"
#include "warploom/render/vulkan_scene.hpp"

namespace {

using omnicpp::core::Entity;
using omnicpp::core::World;
using omnicpp::render::OrbitCameraController;
using omnicpp::render::OrbitCameraInput;
using omnicpp::render::SceneCameraComponent;
using omnicpp::render::SceneMatrix;
using omnicpp::render::scene_camera_look_at;
using omnicpp::render::scene_camera_projection;
using omnicpp::render::scene_camera_view_projection;
using omnicpp::render::scene_identity_matrix;
using omnicpp::render::update_orbit_cameras;

constexpr float kPi = 3.14159265358979323846f;

//! Transform a world point by a column-major 4x4 matrix (homogeneous, w=1),
//! returning the perspective-divided NDC for a projection-style matrix.
//! When `out_w` is non-null it also receives the clip-space w.
void transform_point(const SceneMatrix& m, float px, float py, float pz,
                     float& out_x, float& out_y, float& out_z,
                     float* out_w = nullptr) {
  const float x = m[0] * px + m[4] * py + m[8] * pz + m[12];
  const float y = m[1] * px + m[5] * py + m[9] * pz + m[13];
  const float z = m[2] * px + m[6] * py + m[10] * pz + m[14];
  const float w = m[3] * px + m[7] * py + m[11] * pz + m[15];
  ASSERT_GT(w, 0.0f) << "point must be in front of the camera";
  out_x = x / w;
  out_y = y / w;
  out_z = z / w;
  if (out_w) *out_w = w;
}

//! Add a camera entity (SceneCameraComponent + OrbitCameraController).
Entity make_orbit_camera(World& world, OrbitCameraController controller = {}) {
  const auto entity = world.create_entity();
  world.add_component<SceneCameraComponent>(
      entity, SceneCameraComponent{scene_identity_matrix(), 0U, true});
  world.add_component<OrbitCameraController>(entity, controller);
  return entity;
}



}  // namespace

// =============================================================================
// Look-at and projection math
// =============================================================================

TEST(SceneCameraMath, LookAtEyeAtOriginLookingNegZIsIdentity) {
  // The scene convention: at rest the camera sits at the origin looking down
  // -Z (this is exactly the pose every frustum/hardware test assumes).
  const float eye[3] = {0.0f, 0.0f, 0.0f};
  const float target[3] = {0.0f, 0.0f, -1.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const auto view = scene_camera_look_at(eye, target, up);
  EXPECT_EQ(view, scene_identity_matrix());
}

TEST(SceneCameraMath, LookAtMapsWorldIntoViewSpace) {
  // Camera at (0,0,5) looking at the origin: view direction is -Z, so the
  // origin is 5 units straight ahead (view z = -5) and stays centred.
  const float eye[3] = {0.0f, 0.0f, 5.0f};
  const float target[3] = {0.0f, 0.0f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const auto view = scene_camera_look_at(eye, target, up);

  // Row 0 (x_axis) is stored column-major at [0],[4],[8],[12], etc.
  const float x = view[0] * 0.0f + view[4] * 0.0f + view[8] * 0.0f + view[12];
  const float y = view[1] * 0.0f + view[5] * 0.0f + view[9] * 0.0f + view[13];
  const float z = view[2] * 0.0f + view[6] * 0.0f + view[10] * 0.0f + view[14];
  EXPECT_FLOAT_EQ(x, 0.0f);
  EXPECT_FLOAT_EQ(y, 0.0f);
  EXPECT_FLOAT_EQ(z, -5.0f);

  // A point offset right (+X world) and up (+Y world) of the target keeps its
  // handedness: +X world is screen-right, +Y world is screen-up.
  const float px = view[0] + view[4] + view[12];
  const float py = view[1] + view[5] + view[13];
  const float pz = view[2] + view[6] + view[14];
  EXPECT_FLOAT_EQ(px, 1.0f);
  EXPECT_FLOAT_EQ(py, 1.0f);
  EXPECT_FLOAT_EQ(pz, -5.0f);
}

TEST(SceneCameraMath, ProjectionMatchesSceneConvention) {
  // Same closed form as the hardware/extraction tests: fov_y over the
  // vertical axis, w = -z_view, NDC near->0 far->1.
  const float fov_y = 1.05f;
  const float aspect = 1.6f;
  const float z_near = 0.1f;
  const float z_far = 100.0f;
  const auto p = scene_camera_projection(fov_y, aspect, z_near, z_far);

  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float inv = 1.0f / (z_near - z_far);
  EXPECT_FLOAT_EQ(p[0], f / aspect);
  EXPECT_FLOAT_EQ(p[5], f);
  EXPECT_FLOAT_EQ(p[10], z_far * inv);
  EXPECT_FLOAT_EQ(p[11], -1.0f);
  EXPECT_FLOAT_EQ(p[14], z_near * z_far * inv);
  EXPECT_FLOAT_EQ(p[15], 0.0f);

  // Near plane maps to NDC z = 0, far plane to NDC z = 1.
  float nx, ny, nz;
  transform_point(p, 0.0f, 0.0f, -z_near, nx, ny, nz);
  EXPECT_NEAR(nz, 0.0f, 1e-5f);
  float fx, fy, fz;
  transform_point(p, 0.0f, 0.0f, -z_far, fx, fy, fz);
  EXPECT_NEAR(fz, 1.0f, 1e-5f);
}

TEST(SceneCameraMath, ViewProjectionComposesLookAtTimesProjection) {
  const float fov_y = 1.05f;
  const float aspect = 1.0f;
  const float z_near = 0.1f;
  const float z_far = 100.0f;
  const float eye[3] = {0.0f, 0.0f, 5.0f};
  const float target[3] = {0.0f, 0.0f, 0.0f};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const auto vp =
      scene_camera_view_projection(eye, target, up, fov_y, aspect, z_near, z_far);

  // The world-space origin (the target, 5 units ahead of the camera) lands at
  // the exact NDC centre, at the closed-form depth for z_view = -5.
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float inv = 1.0f / (z_near - z_far);
  const float ndc_z_at_5 = (z_far * inv * -5.0f + z_near * z_far * inv) / 5.0f;
  float cx, cy, cz;
  transform_point(vp, 0.0f, 0.0f, 0.0f, cx, cy, cz);
  EXPECT_NEAR(cx, 0.0f, 1e-6f);
  EXPECT_NEAR(cy, 0.0f, 1e-6f);
  EXPECT_NEAR(cz, ndc_z_at_5, 1e-5f);

  // A point 1 unit to the right at the same depth maps to positive NDC x
  // scaled by f / depth (aspect 1).
  float rx, ry, rz;
  transform_point(vp, 1.0f, 0.0f, 0.0f, rx, ry, rz);
  EXPECT_NEAR(rx, f / 5.0f, 1e-5f);
  EXPECT_NEAR(ry, 0.0f, 1e-6f);
  EXPECT_NEAR(rz, cz, 1e-6f);
}

// =============================================================================
// Orbit controller
// =============================================================================

TEST(SceneCameraOrbit, ZeroInputWritesCanonicalViewProjection) {
  World world;
  const auto entity = make_orbit_camera(world);

  update_orbit_cameras(world, 1.0f, OrbitCameraInput{});
  const auto& controller = world.get_component<OrbitCameraController>(entity);

  // Defaults: yaw 0, pitch 0.45, distance 10, target origin. The controller
  // must produce exactly the public math function's output for that pose.
  const float cp = std::cos(controller.pitch);
  const float sp = std::sin(controller.pitch);
  float eye[3] = {controller.target[0] + cp * controller.distance,
                  controller.target[1] + sp * controller.distance,
                  controller.target[2]};
  const float up[3] = {0.0f, 1.0f, 0.0f};
  const auto expected = scene_camera_view_projection(
      eye, controller.target.data(), up, controller.fov_y, 1.0f,
      controller.z_near, controller.z_far);
  EXPECT_EQ(world.get_component<SceneCameraComponent>(entity).view_projection,
            expected);
}

TEST(SceneCameraOrbit, IsDeterministicAcrossIdenticalWorlds) {
  // World is not copyable, so build two identical worlds in place and verify
  // the same tick input drives them to bit-identical view matrices.
  auto build = [](World& world) {
    auto controller = OrbitCameraController{};
    controller.yaw = 0.7f;
    controller.pitch = -0.2f;
    controller.distance = 6.0f;
    controller.target = {1.0f, 2.0f, 3.0f};
    make_orbit_camera(world, controller);
  };
  World a;
  World b;
  build(a);
  build(b);

  OrbitCameraInput input;
  input.look_x = 0.13f;
  input.look_y = -0.07f;
  input.zoom = 0.9f;
  input.pan_right = 0.4f;
  input.pan_forward = 0.2f;
  update_orbit_cameras(a, 1.3f, input);
  update_orbit_cameras(b, 1.3f, input);

  SceneMatrix va{};
  SceneMatrix vb{};
  a.for_each<SceneCameraComponent>(
      [&](Entity, const SceneCameraComponent& cam) { va = cam.view_projection; });
  b.for_each<SceneCameraComponent>(
      [&](Entity, const SceneCameraComponent& cam) { vb = cam.view_projection; });
  EXPECT_EQ(va, vb);
}

TEST(SceneCameraOrbit, LookYawOrbitsCameraAroundTarget) {
  World world;
  const auto entity = make_orbit_camera(world);

  OrbitCameraInput input;
  input.look_x = kPi * 0.5f;
  update_orbit_cameras(world, 1.0f, input);

  const auto& controller = world.get_component<OrbitCameraController>(entity);
  EXPECT_NEAR(controller.yaw, kPi * 0.5f, 1e-5f);
  // Target and radius are untouched by a pure look.
  EXPECT_FLOAT_EQ(controller.target[0], 0.0f);
  EXPECT_FLOAT_EQ(controller.target[1], 0.0f);
  EXPECT_FLOAT_EQ(controller.target[2], 0.0f);
  EXPECT_FLOAT_EQ(controller.distance, 10.0f);

  // The target must still sit dead-centre in front of the camera exactly at
  // the orbit radius: NDC x/y = 0 and clip w = distance (w = -z_view), and
  // the camera must have swung around (eye no longer on +X).
  const auto& vp = world.get_component<SceneCameraComponent>(entity).view_projection;
  float cx, cy, cz, cw;
  transform_point(vp, controller.target[0], controller.target[1],
                  controller.target[2], cx, cy, cz, &cw);
  EXPECT_NEAR(cx, 0.0f, 1e-5f);
  EXPECT_NEAR(cy, 0.0f, 1e-5f);
  EXPECT_NEAR(cw, controller.distance, 1e-4f);
  EXPECT_GT(cz, 0.0f);  // in front, mapped into [0,1] depth
  EXPECT_LT(cz, 1.0f);
}

TEST(SceneCameraOrbit, PitchIsClamped) {
  World world;
  auto controller = OrbitCameraController{};
  controller.pitch = 0.0f;
  const auto entity = make_orbit_camera(world, controller);

  OrbitCameraInput input;
  input.look_y = 1.0f;  // way past the pole
  update_orbit_cameras(world, 1.0f, input);
  update_orbit_cameras(world, 1.0f, input);

  const float kPitchLimit = kPi * 0.499f;
  const auto& after = world.get_component<OrbitCameraController>(entity);
  EXPECT_NEAR(after.pitch, kPitchLimit, 1e-5f);
  EXPECT_LE(after.pitch, kPitchLimit);

  // Same clamp on the way down.
  OrbitCameraInput down;
  down.look_y = -10.0f;
  update_orbit_cameras(world, 1.0f, down);
  EXPECT_NEAR(world.get_component<OrbitCameraController>(entity).pitch,
              -kPitchLimit, 1e-5f);
}

TEST(SceneCameraOrbit, ZoomScalesDistanceAndClamps) {
  World world;
  auto controller = OrbitCameraController{};
  controller.distance = 10.0f;
  controller.min_distance = 2.0f;
  controller.max_distance = 25.0f;
  const auto entity = make_orbit_camera(world, controller);

  // Zoom in (0.5x per tick) two ticks: 10 -> 5 -> 2.5.
  OrbitCameraInput in;
  in.zoom = 0.5f;
  update_orbit_cameras(world, 1.0f, in);
  update_orbit_cameras(world, 1.0f, in);
  EXPECT_NEAR(world.get_component<OrbitCameraController>(entity).distance, 2.5f,
              1e-5f);

  // More zooming bottoms out at min_distance.
  OrbitCameraInput more;
  more.zoom = 0.01f;
  update_orbit_cameras(world, 1.0f, more);
  EXPECT_FLOAT_EQ(world.get_component<OrbitCameraController>(entity).distance,
                  2.0f);

  // Zoom out caps at max_distance.
  OrbitCameraInput out;
  out.zoom = 100.0f;
  update_orbit_cameras(world, 1.0f, out);
  EXPECT_FLOAT_EQ(world.get_component<OrbitCameraController>(entity).distance,
                  25.0f);
}

TEST(SceneCameraOrbit, PanMovesTargetAlongCameraFrame) {
  World world;
  auto controller = OrbitCameraController{};
  controller.pitch = 0.45f;
  const auto entity = make_orbit_camera(world, controller);

  // Pure right-pan. At yaw 0 the camera sits on +X looking back at the
  // origin; the view frame (look_at basis: up x offset) puts screen-right on
  // -Z, so a positive pan moves the target to z = -1.
  OrbitCameraInput right;
  right.pan_right = 1.0f;
  update_orbit_cameras(world, 1.0f, right);
  const auto& after_right = world.get_component<OrbitCameraController>(entity);
  EXPECT_NEAR(after_right.target[0], 0.0f, 1e-5f);
  EXPECT_NEAR(after_right.target[1], 0.0f, 1e-5f);
  EXPECT_NEAR(after_right.target[2], -1.0f, 1e-5f);

  // Forward pan pushes the target away along -offset (into the scene).
  OrbitCameraInput forward;
  forward.pan_forward = 1.0f;
  update_orbit_cameras(world, 1.0f, forward);
  const auto& after_forward = world.get_component<OrbitCameraController>(entity);
  EXPECT_NEAR(after_forward.target[0], -std::cos(0.45f), 1e-5f);
  EXPECT_NEAR(after_forward.target[1], -std::sin(0.45f), 1e-5f);
}

TEST(SceneCameraOrbit, SkipsInactiveAndControllerlessCameras) {
  World world;

  // Active camera with controller: gets updated.
  const auto active = make_orbit_camera(world);

  // Inactive camera with controller: must be left completely untouched.
  const auto inactive = world.create_entity();
  auto inactive_vp = scene_identity_matrix();
  inactive_vp[12] = 7.0f;
  world.add_component<SceneCameraComponent>(
      inactive, SceneCameraComponent{inactive_vp, 0U, false});
  auto inactive_controller = OrbitCameraController{};
  inactive_controller.distance = 3.0f;
  world.add_component<OrbitCameraController>(inactive, inactive_controller);

  // Active camera but no OrbitCameraController: untouched (for_each iterates
  // SceneCameraComponent holders, the controller lookup must skip it).
  const auto no_controller = world.create_entity();
  auto plain_vp = scene_identity_matrix();
  plain_vp[13] = 9.0f;
  world.add_component<SceneCameraComponent>(
      no_controller, SceneCameraComponent{plain_vp, 0U, true});

  OrbitCameraInput input;
  input.look_x = 0.5f;
  input.pan_right = 3.0f;
  update_orbit_cameras(world, 1.0f, input);

  EXPECT_NEAR(world.get_component<OrbitCameraController>(active).yaw, 0.5f,
              1e-5f);

  const auto& untouched_inactive = world.get_component<SceneCameraComponent>(inactive);
  EXPECT_EQ(untouched_inactive.view_projection, inactive_vp);
  EXPECT_FLOAT_EQ(world.get_component<OrbitCameraController>(inactive).distance,
                  3.0f);

  const auto& untouched_plain = world.get_component<SceneCameraComponent>(no_controller);
  EXPECT_EQ(untouched_plain.view_projection, plain_vp);
}

TEST(SceneCameraOrbit, MultipleCamerasAllUpdate) {
  World world;
  const auto a = make_orbit_camera(world);
  const auto b = make_orbit_camera(world);

  OrbitCameraInput input;
  input.look_x = 0.25f;
  update_orbit_cameras(world, 1.0f, input);

  EXPECT_NEAR(world.get_component<OrbitCameraController>(a).yaw, 0.25f, 1e-5f);
  EXPECT_NEAR(world.get_component<OrbitCameraController>(b).yaw, 0.25f, 1e-5f);
}
