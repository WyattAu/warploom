#pragma once

/**
 * @file vulkan_hiz_frame_state.hpp
 * @brief Frame-local state for previous-frame H-Z resources.
 *
 * This type owns no Vulkan handles. It defines the renderer contract around
 * two H-Z images: one image is built by the current frame while the completed
 * opposite image may be consumed by culling. Camera/projection/extent changes
 * invalidate the previous image so the first frame after a discontinuity does
 * not make visibility decisions from stale depth.
 */

#include "engine/core/deterministic_runtime.hpp"
#include "engine/render/vulkan_hiz_pyramid.hpp"
#include <cstdint>

namespace omnicpp::render {

enum class HiZInvalidation : std::uint32_t {
  none = 0,
  first_frame = 1U << 0U,
  resize = 1U << 1U,
  camera_cut = 1U << 2U,
  projection_change = 1U << 3U,
  explicit_reset = 1U << 4U,
};

[[nodiscard]] constexpr HiZInvalidation operator|(
    HiZInvalidation lhs, HiZInvalidation rhs) noexcept {
  return static_cast<HiZInvalidation>(static_cast<std::uint32_t>(lhs) |
                                      static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr bool has_hiz_invalidation(
    HiZInvalidation value, HiZInvalidation flag) noexcept {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
}

struct HiZFrameToken {
  std::uint64_t generation{0};
  std::uint32_t write_index{0};
  std::uint32_t previous_index{1};
  bool has_previous{false};
};

class VulkanHiZFrameState final {
public:
  [[nodiscard]] omnicpp::core::Result<void> configure(
      std::uint32_t render_width, std::uint32_t render_height,
      std::uint32_t tile_size = 32U, std::uint32_t levels = 0U) noexcept {
    if (render_width == 0U || render_height == 0U || tile_size == 0U) {
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
    }
    const std::uint32_t pyramid_width =
        (render_width + tile_size - 1U) / tile_size;
    const std::uint32_t pyramid_height =
        (render_height + tile_size - 1U) / tile_size;
    const std::uint32_t legal_levels =
        VulkanHiZPyramid::mip_levels_for_extent(pyramid_width, pyramid_height);
    if (levels == 0U) levels = legal_levels;
    if (levels == 0U || levels > legal_levels) {
      return omnicpp::core::Result<void>::error(omnicpp::core::RuntimeError::invalid_config);
    }

    const bool changed = !configured_ || render_width_ != render_width ||
                         render_height_ != render_height || tile_size_ != tile_size ||
                         levels_ != levels;
    render_width_ = render_width;
    render_height_ = render_height;
    tile_size_ = tile_size;
    pyramid_width_ = pyramid_width;
    pyramid_height_ = pyramid_height;
    levels_ = levels;
    configured_ = true;
    if (changed) invalidate(HiZInvalidation::resize);
    return omnicpp::core::Result<void>::ok();
  }

  [[nodiscard]] bool configured() const noexcept { return configured_; }
  [[nodiscard]] std::uint32_t render_width() const noexcept { return render_width_; }
  [[nodiscard]] std::uint32_t render_height() const noexcept { return render_height_; }
  [[nodiscard]] std::uint32_t tile_size() const noexcept { return tile_size_; }
  [[nodiscard]] std::uint32_t pyramid_width() const noexcept { return pyramid_width_; }
  [[nodiscard]] std::uint32_t pyramid_height() const noexcept { return pyramid_height_; }
  [[nodiscard]] std::uint32_t levels() const noexcept { return levels_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] bool has_previous_frame() const noexcept { return previous_valid_; }
  [[nodiscard]] std::uint32_t write_index() const noexcept { return write_index_; }
  [[nodiscard]] HiZInvalidation invalidation() const noexcept { return invalidation_; }

  void invalidate(HiZInvalidation reason) noexcept {
    invalidation_ = invalidation_ | reason;
    previous_valid_ = false;
    ++generation_;
  }

  void mark_camera_cut() noexcept { invalidate(HiZInvalidation::camera_cut); }
  void mark_projection_change() noexcept { invalidate(HiZInvalidation::projection_change); }
  void reset() noexcept {
    invalidation_ = HiZInvalidation::explicit_reset;
    previous_valid_ = false;
    ++generation_;
  }

  //! Selects the destination image and the only image eligible for sampling.
  [[nodiscard]] HiZFrameToken begin_frame() const noexcept {
    return HiZFrameToken{generation_, write_index_, write_index_ ^ 1U, previous_valid_};
  }

  //! Call only after current-frame reduction completed successfully.
  void complete_frame(const HiZFrameToken& token) noexcept {
    if (!configured_ || token.generation != generation_ ||
        token.write_index != write_index_) {
      return;
    }
    previous_valid_ = true;
    invalidation_ = HiZInvalidation::none;
    write_index_ ^= 1U;
  }

  //! Discards a partially recorded frame without changing ping-pong ownership.
  void discard_frame(const HiZFrameToken& token) noexcept {
    if (token.generation != generation_) return;
    previous_valid_ = false;
  }

private:
  bool configured_{false};
  bool previous_valid_{false};
  std::uint32_t render_width_{0};
  std::uint32_t render_height_{0};
  std::uint32_t tile_size_{0};
  std::uint32_t pyramid_width_{0};
  std::uint32_t pyramid_height_{0};
  std::uint32_t levels_{0};
  std::uint32_t write_index_{0};
  std::uint64_t generation_{0};
  HiZInvalidation invalidation_{HiZInvalidation::first_frame};
};

} // namespace omnicpp::render
