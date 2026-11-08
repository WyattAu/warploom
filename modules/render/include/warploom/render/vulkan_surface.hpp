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

namespace warploom::render {

//! Which window-system backend the handles belong to. Explicit — never
//! probed: passing Wayland handles to the XCB constructor (or vice versa)
//! would wrap garbage into a plausible-looking surface.
//! Defaults to Xcb so the existing callers behave identically.
enum class SurfacePlatform : std::uint8_t { Xcb, Wayland };

//! Platform-specific surface creation result.
struct SurfaceCreateInfo {
  void* display{nullptr};
  void* window{nullptr};
  std::uint32_t width{800};
  std::uint32_t height{600};
  SurfacePlatform platform{SurfacePlatform::Xcb};
};

//! Create a Vulkan surface from platform-specific handles.
//! The caller must provide the correct platform handles in SurfaceCreateInfo.
[[nodiscard]] ::warploom::core::Result<void> create_platform_surface(
    VkInstance instance,
    const SurfaceCreateInfo& info,
    VkSurfaceKHR& surface);

} // namespace warploom::render