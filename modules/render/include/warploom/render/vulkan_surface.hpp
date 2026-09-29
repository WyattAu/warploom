#pragma once

/**
 * @file vulkan_surface.hpp
 * @brief Platform-specific Vulkan surface creation.
 *
 * Provides surface creation for X11 (xcb) and Wayland.
 * Each platform function is guarded by preprocessor defines.
 */

#include "warploom/core/deterministic_runtime.hpp"
#include "warploom/render/vulkan_types.hpp"
#include <cstdint>

namespace omnicpp::render {

//! Platform-specific surface creation result.
struct SurfaceCreateInfo {
  void* display{nullptr};
  void* window{nullptr};
  std::uint32_t width{800};
  std::uint32_t height{600};
};

//! Create a Vulkan surface from platform-specific handles.
//! The caller must provide the correct platform handles in SurfaceCreateInfo.
[[nodiscard]] omnicpp::core::Result<void> create_platform_surface(
    VkInstance instance,
    const SurfaceCreateInfo& info,
    VkSurfaceKHR& surface);

} // namespace omnicpp::render
