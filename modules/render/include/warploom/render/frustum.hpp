#pragma once

/**
 * @file frustum.hpp
 * @brief Deterministic CPU view-frustum culling for scene extraction.
 *
 * Planes are extracted from a combined view-projection matrix stored in the
 * engine's column-major SceneMatrix convention (element [col*4 + row]).
 * With clip-space extents [-w, w], the six outward-facing planes of the
 * frustum are the classic Gribb-Hartmann combinations of the matrix rows.
 * A box is fully outside when a single plane separates all of its
 * (transformed) corners, which is what culling needs: conservative enough to
 * never drop a visible object.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace omnicpp::render {

//! Row vector of a 4x4 column-major matrix stored as float[16].
struct Row4 {
  float x{0.0f}, y{0.0f}, z{0.0f}, w{0.0f};
};

//! Four outward-facing half-spaces. Inside means distance >= 0 for a point.
struct Frustum {
  Row4 planes[6]{};

  [[nodiscard]] static Frustum from_view_projection(
      const float m[16]) noexcept {
    // Row i of the matrix is (m[i], m[4+i], m[8+i], m[12+i]).
    const Row4 rows[4] = {
        {m[0], m[4], m[8], m[12]},
        {m[1], m[5], m[9], m[13]},
        {m[2], m[6], m[10], m[14]},
        {m[3], m[7], m[11], m[15]}};
    // left:   row3 + row0   (x >= -w)
    // right:  row3 - row0   (x <=  w)
    // bottom: row3 + row1   (y >= -w)
    // top:    row3 - row1   (y <=  w)
    // near:   row3 + row2   (z >= -w)
    // far:    row3 - row2   (z <=  w)
    Frustum f;
    f.planes[0] = add(rows[3], rows[0]);
    f.planes[1] = sub(rows[3], rows[0]);
    f.planes[2] = add(rows[3], rows[1]);
    f.planes[3] = sub(rows[3], rows[1]);
    f.planes[4] = add(rows[3], rows[2]);
    f.planes[5] = sub(rows[3], rows[2]);
    for (int i = 0; i < 6; ++i) normalize(f.planes[i]);
    return f;
  }

  //! Signed distance of a point to a plane (positive = inside side).
  [[nodiscard]] static float distance(const Row4& plane, float x, float y,
                                      float z) noexcept {
    return plane.x * x + plane.y * y + plane.z * z + plane.w;
  }

  //! True when the axis-aligned box (with center/extent, world space) is
  //! completely outside the frustum: some plane keeps every box point on its
  //! negative side.
  [[nodiscard]] static bool box_outside(const Frustum& frustum,
                                        const float center[3],
                                        const float extent[3]) noexcept {
    for (const Row4& plane : frustum.planes) {
      const float d = distance(plane, center[0], center[1], center[2]);
      const float radius = std::abs(plane.x) * extent[0] +
                           std::abs(plane.y) * extent[1] +
                           std::abs(plane.z) * extent[2];
      if (d + radius < 0.0f) return true;
    }
    return false;
  }

private:
  [[nodiscard]] static Row4 add(const Row4& a, const Row4& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
  }
  [[nodiscard]] static Row4 sub(const Row4& a, const Row4& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
  }
  static void normalize(Row4& p) noexcept {
    const float length = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (length > 0.0f) {
      p.x /= length;
      p.y /= length;
      p.z /= length;
      p.w /= length;
    }
  }
};

} // namespace omnicpp::render
