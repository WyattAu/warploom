#pragma once

/**
 * @file software_rasterizer.hpp
 * @brief Deterministic software rasterizer for testing and headless rendering.
 *
 * Provides a pixel-accurate software rendering pipeline that:
 * - Rasterizes triangles with z-buffering
 * - Supports color interpolation
 * - Produces deterministic output (same input → same pixels)
 * - Requires no GPU or Vulkan — pure CPU computation
 *
 * Use cases:
 * - Testing rendering correctness without hardware
 * - Deterministic replay of rendered frames
 * - CI validation of rendering pipeline
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace warploom::render {

// ============================================================================
// Software Rasterizer
// ============================================================================

class SoftwareRasterizer final {
public:
  explicit SoftwareRasterizer(std::uint32_t width, std::uint32_t height)
      : width_(width), height_(height),
        color_buffer_(width * height, 0xFF000000),
        depth_buffer_(width * height, 1.0f) {}

  ~SoftwareRasterizer() = default;

  SoftwareRasterizer(const SoftwareRasterizer&) = delete;
  SoftwareRasterizer& operator=(const SoftwareRasterizer&) = delete;
  SoftwareRasterizer(SoftwareRasterizer&&) = delete;
  SoftwareRasterizer& operator=(SoftwareRasterizer&&) = delete;

  // -- Drawing --

  //! Clear all buffers.
  void clear(std::uint32_t color = 0xFF000000, float depth = 1.0f) noexcept {
    std::fill(color_buffer_.begin(), color_buffer_.end(), color);
    std::fill(depth_buffer_.begin(), depth_buffer_.end(), depth);
  }

  //! Draw a filled triangle with flat color.
  void draw_triangle(float x0, float y0, float z0, std::uint32_t color0,
                     float x1, float y1, float z1, std::uint32_t color1,
                     float x2, float y2, float z2, std::uint32_t color2) noexcept {
    // Bounding box
    int min_x = std::max(0, static_cast<int>(std::min({x0, x1, x2})));
    int max_x = std::min(static_cast<int>(width_) - 1, static_cast<int>(std::max({x0, x1, x2})));
    int min_y = std::max(0, static_cast<int>(std::min({y0, y1, y2})));
    int max_y = std::min(static_cast<int>(height_) - 1, static_cast<int>(std::max({y0, y1, y2})));

    // Edge function — supports both winding orders
    const float area = edge_function(x0, y0, x1, y1, x2, y2);
    if (area == 0.0f) return; // Degenerate
    const float abs_area = (area > 0.0f) ? area : -area;

    for (int y = min_y; y <= max_y; ++y) {
      for (int x = min_x; x <= max_x; ++x) {
        const float px = static_cast<float>(x) + 0.5f;
        const float py = static_cast<float>(y) + 0.5f;

        float w0 = edge_function(x1, y1, x2, y2, px, py);
        float w1 = edge_function(x2, y2, x0, y0, px, py);
        float w2 = edge_function(x0, y0, x1, y1, px, py);

        // Support both winding orders
        if (area < 0.0f) { w0 = -w0; w1 = -w1; w2 = -w2; }

        if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
          const float b0 = w0 / abs_area;
          const float b1 = w1 / abs_area;
          const float b2 = w2 / abs_area;

          const float z = b0 * z0 + b1 * z1 + b2 * z2;
          // Both coordinates are pixel positions for a fragment already
          // proven inside the triangle (the w0/w1/w2 >= 0 test above), so
          // neither can be negative here. Casting both makes that explicit;
          // casting only y would leave a negative x wrapping to a huge index.
          const auto idx = static_cast<std::size_t>(y) *
                               static_cast<std::size_t>(width_) +
                           static_cast<std::size_t>(x);

          if (z < depth_buffer_[idx]) {
            depth_buffer_[idx] = z;
            const std::uint8_t r = static_cast<std::uint8_t>(
                b0 * static_cast<float>((color0 >> 16) & 0xFF) + b1 * static_cast<float>((color1 >> 16) & 0xFF) + b2 * static_cast<float>((color2 >> 16) & 0xFF));
            const std::uint8_t g = static_cast<std::uint8_t>(
                b0 * static_cast<float>((color0 >> 8) & 0xFF) + b1 * static_cast<float>((color1 >> 8) & 0xFF) + b2 * static_cast<float>((color2 >> 8) & 0xFF));
            const std::uint8_t b = static_cast<std::uint8_t>(
                b0 * static_cast<float>(color0 & 0xFF) + b1 * static_cast<float>(color1 & 0xFF) + b2 * static_cast<float>(color2 & 0xFF));
            color_buffer_[idx] = 0xFF000000 | (static_cast<std::uint32_t>(r) << 16) |
                                 (static_cast<std::uint32_t>(g) << 8) | b;
          }
        }
      }
    }
  }

  // -- Accessors --

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] const std::uint32_t* color_data() const noexcept { return color_buffer_.data(); }
  [[nodiscard]] const float* depth_data() const noexcept { return depth_buffer_.data(); }
  [[nodiscard]] std::uint32_t get_pixel(std::uint32_t x, std::uint32_t y) const noexcept {
    if (x >= width_ || y >= height_) return 0;
    return color_buffer_[static_cast<std::size_t>(y) * width_ + x];
  }

  //! Compute a hash of the color buffer for deterministic comparison.
  [[nodiscard]] std::uint64_t frame_hash() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (auto pixel : color_buffer_) {
      hash ^= pixel;
      hash *= 1099511628211ULL;
    }
    return hash;
  }

private:
  [[nodiscard]] static float edge_function(float ax, float ay, float bx, float by,
                                           float cx, float cy) noexcept {
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
  }

  std::uint32_t width_;
  std::uint32_t height_;
  std::vector<std::uint32_t> color_buffer_;
  std::vector<float> depth_buffer_;
};

} // namespace warploom::render

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
