#pragma once

/**
 * @file depth_convention.hpp
 * @brief Canonical forward-Z projection/depth equations for the renderer.
 *
 * View-space depth is positive distance from the camera. The projection maps
 * it to Vulkan's [0, 1] depth interval using a standard OpenGL-style matrix
 * followed by the renderer's configured viewport convention. H-Z stores the
 * maximum value in each footprint: a smaller value is nearer.
 */

namespace warploom::render::depth {

//! Exact depth value for a positive view-space distance, clamped to [0, 1].
[[nodiscard]] constexpr float ndc_from_view_distance(
    float view_distance, float near_plane, float far_plane) noexcept {
  const float inverse_range = 1.0f / (far_plane - near_plane);
  const float c1 = (far_plane + near_plane) * inverse_range;
  const float c2 = 2.0f * far_plane * near_plane * inverse_range;
  const float depth = c1 - c2 / view_distance;
  return depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
}

//! Conservative nearest point distance for a sphere centered in view space.
[[nodiscard]] constexpr float sphere_nearest_view_distance(
    float center_view_z, float radius, float near_plane) noexcept {
  const float nearest = -center_view_z - radius;
  return nearest > near_plane ? nearest : near_plane;
}

//! Exact projected depth of a sphere's nearest point.
[[nodiscard]] constexpr float sphere_nearest_depth(
    float center_view_z, float radius,
    float near_plane, float far_plane) noexcept {
  return ndc_from_view_distance(
      sphere_nearest_view_distance(center_view_z, radius, near_plane),
      near_plane, far_plane);
}

} // namespace warploom::render::depth

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef WARPLOOM_COMPAT_RENDER_NS
#define WARPLOOM_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // WARPLOOM_COMPAT_RENDER_NS
