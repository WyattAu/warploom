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
//! is buttons 4/5 press-only; on Wayland it is axis_value120 (pointer
//! v8+), mapped below to the same 4/5 press steps the translator
//! consumes. Pointer coordinates are wl_fixed_t (24.8); the
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

//! One wheel action: an XCB-style button (4 = up, 5 = down) pressed
//! `count` times. Button 0 with count 0 means "no action".
struct WheelStep {
  std::uint8_t button;
  std::uint32_t count;
};

//! axis_value120 (pointer v8+) -> XCB wheel buttons. Each 120 is one
//! detent; the quotient truncates toward zero, so smooth-scroll
//! partials (< 120) are dropped instead of mis-zooming. Sign follows
//! the protocol: value120 shares motion-event coordinates (y grows
//! down), so positive is down -> button 5, negative is up -> 4 —
//! matching the translator, where 4 nudges negative and 5 positive.
//! Non-vertical axes have no XCB counterpart and yield no action.
//! axis_discrete (v5, deprecated since v8 and never sent to v8+
//! clients) is deliberately unhandled: binding the seat at v8 means
//! exactly one wheel source exists and double-counting is impossible.
[[nodiscard]] inline WheelStep axis_value120_to_wheel(
    std::uint32_t axis, std::int32_t value120) noexcept {
  // WL_POINTER_AXIS_VERTICAL_SCROLL == 0; horizontal is 1.
  if (axis != 0U || value120 == 0) return {0, 0};
  const std::int32_t steps = value120 / 120;  // truncates toward zero
  if (steps == 0) return {0, 0};
  const std::uint8_t button =
      static_cast<std::uint8_t>(steps < 0 ? 4 : 5);
  const std::uint32_t count =
      static_cast<std::uint32_t>(steps < 0 ? -steps : steps);
  return {button, count};
}

}  // namespace wayland_pointer
