/**
 * @file vulkan_surface.cpp
 * @brief Platform-specific Vulkan surface creation.
 */

#include "warploom/render/vulkan_surface.hpp"

#ifdef WARPLOOM_HAS_VULKAN
#include <vulkan/vulkan.h>
#ifdef __linux__
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan_wayland.h>
#endif
#endif

#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#include <windows.h>
#endif
#endif

namespace warploom::render {

::warploom::core::Result<void> create_platform_surface(
    VkInstance instance,
    const SurfaceCreateInfo& info,
    VkSurfaceKHR& surface) {
#ifdef WARPLOOM_HAS_VULKAN
  if (!instance) {
    return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
  }

#ifdef __linux__
  // Wayland surface creation (loop68): display is wl_display*, window is
  // wl_surface*. Types only here — the loader resolves the constructor;
  // nothing links a window system (same shape as the XCB branch).
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
  if (info.platform == SurfacePlatform::Wayland) {
    if (!info.display || !info.window) {
      surface = nullptr;
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }
    VkWaylandSurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info.display = static_cast<wl_display*>(info.display);
    create_info.surface = static_cast<wl_surface*>(info.window);
    const auto wayland_result =
        vkCreateWaylandSurfaceKHR(instance, &create_info, nullptr, &surface);
    if (wayland_result != VK_SUCCESS) {
      surface = nullptr;
      return ::warploom::core::Result<void>::error(
          ::warploom::core::RuntimeError::vulkan_not_available);
    }
    return ::warploom::core::Result<void>::ok();
  }
#endif
  // Try X11/XCB surface creation
  if (info.platform == SurfacePlatform::Xcb && info.display && info.window) {
    VkXcbSurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    create_info.connection = static_cast<xcb_connection_t*>(info.display);
    create_info.window = static_cast<xcb_window_t>(reinterpret_cast<std::uintptr_t>(info.window));

    auto result = vkCreateXcbSurfaceKHR(instance, &create_info, nullptr, &surface);
    if (result == VK_SUCCESS) {
      return ::warploom::core::Result<void>::ok();
    }
  }

  // Fallback: create an offscreen surface for headless rendering
  // Some drivers support this via VK_EXT_headless_surface
  surface = nullptr;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);

#elif defined(_WIN32)
  if (info.display && info.window) {
    VkWin32SurfaceCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create_info.hinstance = static_cast<HINSTANCE>(info.display);
    create_info.hwnd = static_cast<HWND>(info.window);

    auto result = vkCreateWin32SurfaceKHR(instance, &create_info, nullptr, &surface);
    if (result == VK_SUCCESS) {
      return ::warploom::core::Result<void>::ok();
    }
  }

  surface = nullptr;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);

#else
  // Unsupported platform
  (void)instance;
  (void)info;
  surface = nullptr;
  return ::warploom::core::Result<void>::ok();
#endif

#else
  (void)instance;
  (void)info;
  surface = nullptr;
  return ::warploom::core::Result<void>::error(::warploom::core::RuntimeError::vulkan_not_available);
#endif
}

} // namespace warploom::render
