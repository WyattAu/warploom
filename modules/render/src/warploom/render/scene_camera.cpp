//! @file scene_camera.cpp
//! @brief Deterministic scene cameras (see scene_camera.hpp).

#include "warploom/render/scene_camera.hpp"

#include <cmath>

namespace omnicpp::render {
namespace {

//! Column-major 4x4 multiply: out = a * b.
void multiply4x4(const SceneMatrix& a, const SceneMatrix& b,
                 SceneMatrix& out) noexcept {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a[static_cast<std::size_t>(r + 4 * k)] *
               b[static_cast<std::size_t>(k + 4 * c)];
      }
      out[static_cast<std::size_t>(r + 4 * c)] = sum;
    }
  }
}

}  // namespace

SceneMatrix scene_camera_look_at(const float eye[3], const float target[3],
                                 const float up[3]) noexcept {
  // Right-handed basis: z_axis points out of the screen (eye - target),
  // x = up x z_axis, y = z_axis x x.
  float z_axis[3] = {eye[0] - target[0], eye[1] - target[1],
                     eye[2] - target[2]};
  const float z_len =
      std::sqrt(z_axis[0] * z_axis[0] + z_axis[1] * z_axis[1] +
                z_axis[2] * z_axis[2]);
  z_axis[0] /= z_len;
  z_axis[1] /= z_len;
  z_axis[2] /= z_len;

  float x_axis[3] = {up[1] * z_axis[2] - up[2] * z_axis[1],
                     up[2] * z_axis[0] - up[0] * z_axis[2],
                     up[0] * z_axis[1] - up[1] * z_axis[0]};
  const float x_len =
      std::sqrt(x_axis[0] * x_axis[0] + x_axis[1] * x_axis[1] +
                x_axis[2] * x_axis[2]);
  x_axis[0] /= x_len;
  x_axis[1] /= x_len;
  x_axis[2] /= x_len;

  float y_axis[3] = {z_axis[1] * x_axis[2] - z_axis[2] * x_axis[1],
                     z_axis[2] * x_axis[0] - z_axis[0] * x_axis[2],
                     z_axis[0] * x_axis[1] - z_axis[1] * x_axis[0]};

  // Row-major rotation/translation, stored column-major: row i of the
  // rotation is element (i, i, i, ...) reading columns 0..3.
  SceneMatrix m{};
  for (int row = 0; row < 3; ++row) {
    const float* axis = row == 0 ? x_axis : (row == 1 ? y_axis : z_axis);
    m[static_cast<std::size_t>(row)] = axis[0];
    m[static_cast<std::size_t>(row) + 4U] = axis[1];
    m[static_cast<std::size_t>(row) + 8U] = axis[2];
    // Translation: -(axis . eye).
    m[static_cast<std::size_t>(row) + 12U] =
        -(axis[0] * eye[0] + axis[1] * eye[1] + axis[2] * eye[2]);
  }
  m[3] = 0.0f;
  m[7] = 0.0f;
  m[11] = 0.0f;
  m[15] = 1.0f;
  return m;
}

SceneMatrix scene_camera_projection(float fov_y, float aspect, float z_near,
                                    float z_far) noexcept {
  SceneMatrix p{};
  p.fill(0.0f);
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  const float z_span_inv = 1.0f / (z_near - z_far);
  p[0] = f / aspect;
  p[5] = f;
  p[10] = z_far * z_span_inv;
  p[11] = -1.0f;
  p[14] = z_near * z_far * z_span_inv;
  return p;
}

SceneMatrix scene_camera_view_projection(const float eye[3],
                                         const float target[3],
                                         const float up[3], float fov_y,
                                         float aspect, float z_near,
                                         float z_far) noexcept {
  SceneMatrix view = scene_camera_look_at(eye, target, up);
  SceneMatrix proj = scene_camera_projection(fov_y, aspect, z_near, z_far);
  SceneMatrix combined{};
  multiply4x4(proj, view, combined);
  return combined;
}

void update_orbit_cameras(omnicpp::core::World& world, float aspect,
                          const OrbitCameraInput& input) noexcept {
  constexpr float kUp[3] = {0.0f, 1.0f, 0.0f};
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kPitchLimit = kPi * 0.499f;

  auto tick = [&](omnicpp::core::Entity entity, SceneCameraComponent& camera) {
    if (!camera.active) return;
    if (!world.has_component<OrbitCameraController>(entity)) return;
    OrbitCameraController& controller =
        world.get_component<OrbitCameraController>(entity);

    controller.yaw += input.look_x;
    controller.pitch += input.look_y;
    if (controller.pitch > kPitchLimit) controller.pitch = kPitchLimit;
    if (controller.pitch < -kPitchLimit) controller.pitch = -kPitchLimit;
    if (input.zoom > 0.0f) {
      controller.distance *= input.zoom;
      if (controller.distance < controller.min_distance) {
        controller.distance = controller.min_distance;
      }
      if (controller.distance > controller.max_distance) {
        controller.distance = controller.max_distance;
      }
    }

    // Offset direction of the camera from the target (spherical): azimuth
    // `yaw` around +Y, elevation `pitch` above the horizontal plane.
    const float cp = std::cos(controller.pitch);
    const float sp = std::sin(controller.pitch);
    const float cy = std::cos(controller.yaw);
    const float sy = std::sin(controller.yaw);
    float offset[3] = {cp * cy, sp, cp * sy};

    // Camera frame, matching scene_camera_look_at's basis exactly: with
    // z_axis = offset (target -> eye), look_at builds x_axis = up x z_axis
    // (screen right) and y_axis = z_axis x x_axis (screen up). Panning must
    // move the target along these same axes or a positive pan would track the
    // screen-left direction.
    float right[3] = {kUp[1] * offset[2] - kUp[2] * offset[1],
                      kUp[2] * offset[0] - kUp[0] * offset[2],
                      kUp[0] * offset[1] - kUp[1] * offset[0]};
    const float right_len = std::sqrt(right[0] * right[0] +
                                      right[1] * right[1] +
                                      right[2] * right[2]);
    right[0] /= right_len;
    right[1] /= right_len;
    right[2] /= right_len;
    float frame_up[3] = {offset[1] * right[2] - offset[2] * right[1],
                         offset[2] * right[0] - offset[0] * right[2],
                         offset[0] * right[1] - offset[1] * right[0]};
    const float up_len = std::sqrt(frame_up[0] * frame_up[0] +
                                   frame_up[1] * frame_up[1] +
                                   frame_up[2] * frame_up[2]);
    frame_up[0] /= up_len;
    frame_up[1] /= up_len;
    frame_up[2] /= up_len;

    // Pan: translate the orbit target along the camera frame. The view
    // direction (toward the target) is -offset.
    controller.target[0] += right[0] * input.pan_right +
                            frame_up[0] * input.pan_up -
                            offset[0] * input.pan_forward;
    controller.target[1] += right[1] * input.pan_right +
                            frame_up[1] * input.pan_up -
                            offset[1] * input.pan_forward;
    controller.target[2] += right[2] * input.pan_right +
                            frame_up[2] * input.pan_up -
                            offset[2] * input.pan_forward;

    float eye[3] = {controller.target[0] + offset[0] * controller.distance,
                    controller.target[1] + offset[1] * controller.distance,
                    controller.target[2] + offset[2] * controller.distance};
    camera.view_projection = scene_camera_view_projection(
        eye, controller.target.data(), kUp, controller.fov_y, aspect,
        controller.z_near, controller.z_far);
  };

  world.for_each<SceneCameraComponent>(
      [&](omnicpp::core::Entity entity, SceneCameraComponent& camera) {
        tick(entity, camera);
      });
}

}  // namespace omnicpp::render
