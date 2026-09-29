#pragma once

/**
 * @file input_translators.hpp
 * @brief Device-event -> InputState translation for real input hardware.
 *
 * The translators are pure: they consume decoded device events (XCB keycodes
 * and pointer deltas, Linux joystick js_event structs) and produce writes
 * into an InputState. Device I/O lives elsewhere (the viewport's XCB event
 * loop, LinuxJoystickDriver's fd reader), which keeps this mapping fully
 * unit-testable without a display server or physical hardware.
 *
 * Keyboard mapping assumes the evdev keycode table (the Linux default for
 * X servers since 2008): 9=ESC, 25=W, 38=A, 39=S, 40=D, 111=Up, 116=Down,
 * 113=Left, 114=Right, 65=Space.
 */

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "warploom/core/input_state.hpp"

namespace warploom::core {

//! XCB key + pointer translation into the viewport's action vocabulary:
//! orbit_left/orbit_right (A,D / arrows), zoom_in/zoom_out (W,S / Up,Down /
//! wheel), fade_toggle (Space), move_x/move_y (WASD axes), look_x/look_y
//! (pointer deltas).
class XcbKeyMouseTranslator final {
 public:
  //! Key event: evdev keycode + pressed state (XCB_KEY_PRESS / _RELEASE).
  void on_key(std::uint8_t keycode, bool pressed);
  //! Pointer motion: absolute window coordinates; deltas accumulate until
  //! the next apply().
  void on_motion(double x, double y);
  //! Pointer button: 1 = primary, 4/5 = wheel up/down.
  void on_button(std::uint8_t button, bool pressed);

  //! Writes the tracked state into `state`. Called once per tick by the
  //! driver adapter; per-frame deltas reset here.
  void apply(InputState& state);

  //! True when at least one key is currently held (telemetry sanity).
  [[nodiscard]] bool any_key_held() const noexcept { return !keys_.empty(); }
  [[nodiscard]] std::size_t held_key_count() const noexcept {
    return keys_.size();
  }

 private:
  std::unordered_map<std::uint8_t, bool> keys_{};  // keycode -> pressed
  double last_x_{0.0};
  double last_y_{0.0};
  bool have_last_{false};
  double look_dx_{0.0};  //!< accumulated pointer delta since last apply()
  double look_dy_{0.0};
  float wheel_accum_{0.0f};  //!< wheel notches since last apply()
};

//! Decoded Linux joystick event (layout-compatible with struct js_event
//! from <linux/joystick.h>).
struct JsEvent {
  std::uint32_t time{0};
  std::int16_t value{0};
  std::uint8_t type{0};
  std::uint8_t number{0};
};

//! Axis/button deadzone with rescale: |v| <= dz -> 0; otherwise linearly
//! rescaled to the full [-1, 1] range. Standard stick-feel correction.
[[nodiscard]] float stick_deadzone(float v, float dz);

//! Linux joystick translation: left stick -> move_x/move_y, right stick ->
//! look_x/look_y, LB/RB -> zoom_out/zoom_in, A (button 0) -> fade_toggle,
//! dpad (axes 6/7) -> orbit/zoom.
class JoystickTranslator final {
 public:
  void on_event(const JsEvent& event);
  void apply(InputState& state) const;

  [[nodiscard]] std::size_t axis_count() const noexcept {
    return axes_.size();
  }
  [[nodiscard]] std::size_t button_count() const noexcept {
    return buttons_.size();
  }
  //! Raw axis value (post-normalization), for telemetry.
  [[nodiscard]] float axis(std::size_t index) const noexcept {
    return index < axes_.size() ? axes_[index] : 0.0f;
  }

 private:
  void ensure_axis(std::size_t index);
  void ensure_button(std::size_t index);

  std::vector<float> axes_{};
  std::vector<bool> buttons_{};
  //! Previous A-button state for edge detection. Mutable because apply()
  //! is logically const (a consumed read updates only the edge tracker).
  mutable bool a_prev_applied_{false};
};

#if defined(__linux__)
//! Real device driver: reads /dev/input/jsN (non-blocking) and feeds
//! JoystickTranslator. Implements the InputDriver interface; unavailable
//! (open() fails) when no gamepad is attached, which the caller treats as
//! "device absent", not an error.
class LinuxJoystickDriver final : public InputDriver {
 public:
  //! Opens /dev/input/js<index>; use is_open() to check success.
  explicit LinuxJoystickDriver(int index = 0);
  ~LinuxJoystickDriver() override;
  LinuxJoystickDriver(const LinuxJoystickDriver&) = delete;
  LinuxJoystickDriver& operator=(const LinuxJoystickDriver&) = delete;

  void poll(InputState& state) override;
  [[nodiscard]] const char* name() const noexcept override {
    return "linux_joystick";
  }
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
  [[nodiscard]] const JoystickTranslator& translator() const noexcept {
    return translator_;
  }

 private:
  int fd_{-1};
  JoystickTranslator translator_{};
};
#endif  // __linux__

}  // namespace warploom::core
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).
#ifndef OMNICPP_COMPAT_CORE_NS
#define OMNICPP_COMPAT_CORE_NS
namespace omnicpp::core {
    using namespace ::warploom::core;
}
#endif  // OMNICPP_COMPAT_CORE_NS
