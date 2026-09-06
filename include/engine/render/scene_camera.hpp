#pragma once

/**
 * @file scene_camera.hpp
 * @brief Deterministic scene cameras: look-at/projection math and an ECS
 *        orbit controller.
 *
 * The render scene path consumes `SceneCameraComponent::view_projection`
 * (column-major). This module is the interactive half: `scene_camera_*`
 * functions build those matrices from a conventional pinhole camera, and
 * `update_orbit_cameras()` mutates `OrbitCameraController` components from a
 * deterministic `OrbitCameraInput` and writes the recomputed matrix into the
 * entity's `SceneCameraComponent`. Everything is pure arithmetic — no Vulkan,
 * no clock, no randomness — so identical inputs always produce identical
 * output, and the whole module runs in headless builds.
 *
 * Conventions (matching the existing scene path): right-handed look-at view
 * space with the camera looking down -Z at rest (eye at the origin, target at
 * (0,0,-1) yields the identity view), and the projection used by every
 * hardware scene test (fov_y over the vertical axis, aspect = width / height,
 * column-major with w = -z_view).
 */

#include "engine/core/ecs.hpp"
#include "engine/render/vulkan_scene.hpp"

#include <array>
#include <cstdint>

namespace omnicpp::render {

//! Right-handed look-at view matrix (column-major, translation in [12..14]).
//! `up` must not be parallel to eye->target.
[[nodiscard]] SceneMatrix scene_camera_look_at(const float eye[3],
                                               const float target[3],
                                               const float up[3]) noexcept;

//! Perspective projection over fov_y (radians) with aspect = width/height,
//! matching the scene path's projection convention exactly.
[[nodiscard]] SceneMatrix scene_camera_projection(float fov_y, float aspect,
                                                  float z_near,
                                                  float z_far) noexcept;

//! projection * look_at(eye, target, up) in one call.
[[nodiscard]] SceneMatrix scene_camera_view_projection(
    const float eye[3], const float target[3], const float up[3], float fov_y,
    float aspect, float z_near, float z_far) noexcept;

//! Deterministic per-tick camera input. Units are documented per field; zero
//! input leaves the camera exactly where it was.
struct OrbitCameraInput {
  //! Look deltas in radians for this tick.
  float look_x{0.0f};  //!< rotate around the world +Y axis
  float look_y{0.0f};  //!< elevate/depress the orbit
  //! Orbit radius multiplier applied once per tick (e.g. 0.95 zooms in).
  float zoom{1.0f};
  //! Pan the orbit target in world units along the camera frame.
  float pan_right{0.0f};
  float pan_up{0.0f};
  float pan_forward{0.0f};  //!< positive moves the target into the scene
};

//! ECS component: an orbiting camera. The entity must also carry a
//! `SceneCameraComponent`; `update_orbit_cameras` writes its view_projection.
struct OrbitCameraController {
  std::array<float, 3> target{0.0f, 0.0f, 0.0f};
  //! Camera offset direction: azimuth around +Y and elevation in radians.
  float yaw{0.0f};
  float pitch{0.45f};
  //! Distance from the target (the orbit radius).
  float distance{10.0f};
  float fov_y{1.05f};
  float z_near{0.1f};
  float z_far{100.0f};
  float min_distance{0.5f};
  float max_distance{200.0f};
};

//! Apply `input` to every entity carrying both an active SceneCameraComponent
//! and an OrbitCameraController, then recompute that camera's
//! view_projection. `aspect` is the render target's width / height.
//! Inactive scene cameras are skipped. Deterministic.
void update_orbit_cameras(omnicpp::core::World& world, float aspect,
                          const OrbitCameraInput& input) noexcept;

}  // namespace omnicpp::render
