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

//! Platform-specific surface creation result.
struct SurfaceCreateInfo {
  void* display{nullptr};
  void* window{nullptr};
  std::uint32_t width{800};
  std::uint32_t height{600};
};

//! Create a Vulkan surface from platform-specific handles.
//! The caller must provide the correct platform handles in SurfaceCreateInfo.
[[nodiscard]] ::warploom::core::Result<void> create_platform_surface(
    VkInstance instance,
    const SurfaceCreateInfo& info,
    VkSurfaceKHR& surface);

} // namespace warploom::render

// S5-B compat footer: legacy `omnicpp::render` spellings keep resolving during the
// transition (docs/warploom-identity-plan.md, phase 1a). A using-directive
// in a namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types. Guarded per namespace (a
// shared guard would suppress later headers' distinct directives). The
// nested render::depth family resolves through this directive.
#ifndef OMNICPP_COMPAT_RENDER_NS
#define OMNICPP_COMPAT_RENDER_NS
namespace omnicpp::render {
    using namespace ::warploom::render;
}
#endif  // OMNICPP_COMPAT_RENDER_NS
