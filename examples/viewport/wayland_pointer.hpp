#pragma once

//! @file wayland_pointer.hpp
//! @brief Pure Wayland-seat pointer mappings (loop71, no compositor needed).
//!
//! Included by both the viewport (real wl_pointer events) and the unit
//! suite (contract pins). Takes plain integers — no wayland-client
//! dependency here; listener plumbing lives in main.cpp.
//!
//! The core facts: XCB button details are 1 = left, 2 = middle, 3 = right
//! (input_translators.hpp: on_button), while Wayland delivers Linux input
//! codes (BTN_LEFT 0x110, BTN_MIDDLE 0x112, BTN_RIGHT 0x111). Wheel on XCB
//! is buttons 4/5 press-only; on Wayland it is axis events, which need a
//! seat >= v5 discrete-step policy — deliberately unmapped here (deferred
//! to the input-hardening pass), so a wheel-only run degrades to no-zoom
//! rather than a mis-zoom. Pointer coordinates are wl_fixed_t (24.8); the
//! single conversion site is this header, used by main.cpp directly so the
//! divisor cannot drift from the protocol.

#include <cstdint>

namespace wayland_pointer {

//! Linux button code -> XCB button detail (1/2/3). Returns 0 when the
//! button has no XCB counterpart (side buttons, wheel-8/9, ...): the
//! caller drops it, exactly as an unhandled XCB detail falls through.
[[nodiscard]] inline std::uint8_t linux_button_to_x11(
    std::uint32_t button) noexcept {
  // linux/input-event-codes.h values, spelled out so this header stays
  // free of the kernel header: BTN_LEFT 0x110, BTN_RIGHT 0x111,
  // BTN_MIDDLE 0x112.
  switch (button) {
    case 0x110U: return 1;  // BTN_LEFT -> primary
    case 0x112U: return 2;  // BTN_MIDDLE
    case 0x111U: return 3;  // BTN_RIGHT
    default: return 0;
  }
}

//! wl_fixed_t (signed 24.8) -> surface-local pixels. The divisor is the
//! protocol: 256 per unit, sign-preserving for off-surface (negative)
//! positions, which the caller clamps or ignores like XCB out-of-window
//! coordinates.
[[nodiscard]] inline double pointer_fixed_to_double(
    std::int32_t fixed) noexcept {
  return static_cast<double>(fixed) / 256.0;
}

}  // namespace wayland_pointer
