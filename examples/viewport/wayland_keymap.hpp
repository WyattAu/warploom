#pragma once

//! @file wayland_keymap.hpp
//! @brief Pure Wayland-seat input mappings (loop70, no compositor needed).
//!
//! Included by both the viewport (real wl_keyboard events) and the unit
//! suite (contract pins). Takes plain integers — no wayland-client or
//! xkbcommon dependency here; keymap/state management lives in main.cpp.
//!
//! The core fact: X11 keycodes are evdev codes + 8 (the XKB default
//! offset, stable since 2008). Wayland delivers raw evdev codes, so the
//! physical-position paths in poll_events (arrows, WASD, C/V chords at
//! 54/55, ESC 9, Return 36, BackSpace 22, Delete 119) keep working
//! unchanged when fed evdev + 8. Printable characters come from the
//! xkbcommon keysym instead (layout-correct, unlike the XCB tables,
//! which hardcode pc105/QWERTZ positions).

#include <cstdint>

namespace wayland_keymap {

//! evdev keycode -> X11 keycode (evdev + 8). Returns 0 when out of range:
//! evdev codes above 247 would overflow the X11 byte.
[[nodiscard]] inline std::uint8_t evdev_to_x11(std::uint32_t evdev) noexcept {
  if (evdev > 247U) return 0;
  return static_cast<std::uint8_t>(evdev + 8U);
}

//! Keysym -> printable ASCII for the param editor. The keysym IS the
//! character (per layout and modifiers, decoded by xkbcommon), so this
//! is a range check, not a table: 32..126 pass through, everything else
//! (including NoSymbol 0) yields 0. Delete (127) is an app command, not
//! text — also 0.
[[nodiscard]] inline char keysym_to_printable(std::uint32_t keysym) noexcept {
  if (keysym < 32U || keysym > 126U) return 0;
  return static_cast<char>(keysym);
}

}  // namespace wayland_keymap
