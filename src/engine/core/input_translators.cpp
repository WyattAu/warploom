//! @file input_translators.cpp
//! @brief XCB keyboard/mouse + Linux joystick translation into InputState.

#include "engine/core/input_translators.hpp"

#include <cmath>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace omnicpp::core {

namespace {

//! Evdev keycodes (the Linux default X keycodes): keep in one place.
constexpr std::uint8_t kKeyW = 25;
constexpr std::uint8_t kKeyA = 38;
constexpr std::uint8_t kKeyS = 39;
constexpr std::uint8_t kKeyD = 40;
constexpr std::uint8_t kKeySpace = 65;
constexpr std::uint8_t kKeyLeft = 113;
constexpr std::uint8_t kKeyRight = 114;
constexpr std::uint8_t kKeyUp = 111;
constexpr std::uint8_t kKeyDown = 116;

}  // namespace

// ============================================================================
// XcbKeyMouseTranslator
// ============================================================================

void XcbKeyMouseTranslator::on_key(std::uint8_t keycode, bool pressed) {
  if (pressed) {
    keys_[keycode] = true;
  } else {
    keys_.erase(keycode);
  }
  // ESC (keycode 9) stays an app-level quit, not an action.
}

void XcbKeyMouseTranslator::on_motion(double x, double y) {
  if (have_last_) {
    look_dx_ += x - last_x_;
    look_dy_ += y - last_y_;
  }
  last_x_ = x;
  last_y_ = y;
  have_last_ = true;
}

void XcbKeyMouseTranslator::on_button(std::uint8_t button, bool pressed) {
  // Wheel events (buttons 4/5) arrive as press-only; accumulate into a
  // one-tick zoom nudge consumed by the next apply().
  if (pressed && (button == 4 || button == 5)) {
    wheel_accum_ += (button == 4) ? -1.0f : 1.0f;
  }
}

void XcbKeyMouseTranslator::apply(InputState& state) {
  const bool w = keys_.count(kKeyW) != 0U;
  const bool a = keys_.count(kKeyA) != 0U;
  const bool s = keys_.count(kKeyS) != 0U;
  const bool d = keys_.count(kKeyD) != 0U;
  const bool left = keys_.count(kKeyLeft) != 0U;
  const bool right = keys_.count(kKeyRight) != 0U;
  const bool up = keys_.count(kKeyUp) != 0U;
  const bool down = keys_.count(kKeyDown) != 0U;

  // Movement axes (normalized diagonal), the primary output for a camera.
  // Arrows mirror WASD in both the axes and the actions.
  float mx = ((d || right) ? 1.0f : 0.0f) - ((a || left) ? 1.0f : 0.0f);
  float my = (w ? 1.0f : 0.0f) - (s ? 1.0f : 0.0f);
  if (mx != 0.0f || my != 0.0f) {
    const float len = std::sqrt(mx * mx + my * my);
    if (len > 1.0f) {
      mx /= len;
      my /= len;
    }
  }
  state.set_axis("move_x", mx);
  state.set_axis("move_y", my);

  // Pointer deltas: accumulated since the last apply, then reset (the
  // consume-on-read contract for look axes).
  state.set_axis("look_x", static_cast<float>(look_dx_));
  state.set_axis("look_y", static_cast<float>(look_dy_));
  look_dx_ = 0.0;
  look_dy_ = 0.0;

  // Keyboard actions + wheel nudge -> the viewport action vocabulary.
  // Space is a plain held action; the edge (tap -> one toggle) comes from
  // InputState::action_pressed, exactly as with the gamepad A button.
  state.set_action("zoom_in", w || up || wheel_accum_ < 0.0f);
  state.set_action("zoom_out", s || down || wheel_accum_ > 0.0f);
  state.set_action("orbit_left", a || left);
  state.set_action("orbit_right", d || right);
  state.set_action("fade_toggle", keys_.count(kKeySpace) != 0U);
  wheel_accum_ = 0.0f;
}

// ============================================================================
// JoystickTranslator
// ============================================================================

float stick_deadzone(float v, float dz) {
  const float a = std::fabs(v);
  if (a <= dz) return 0.0f;
  return std::copysign((a - dz) / (1.0f - dz), v);
}

void JoystickTranslator::ensure_axis(std::size_t index) {
  if (axes_.size() <= index) axes_.resize(index + 1, 0.0f);
}

void JoystickTranslator::ensure_button(std::size_t index) {
  if (buttons_.size() <= index) buttons_.resize(index + 1, false);
}

void JoystickTranslator::on_event(const JsEvent& event) {
  constexpr std::uint8_t kTypeButton = 0x01;
  constexpr std::uint8_t kTypeAxis = 0x02;
  constexpr std::uint8_t kTypeInit = 0x80;

  if (event.type & kTypeAxis) {
    ensure_axis(event.number);
    if (event.number == 6U || event.number == 7U) {
      // HAT0 dpad axes report raw -1/0/1 (already normalized).
      axes_[event.number] = static_cast<float>(event.value);
    } else {
      // js_event values are signed 16-bit; normalize to [-1, 1]. Triggers
      // (axes 2/5 in the modern xpad layout) idle at -32768, so remap
      // those to [0, 1].
      float v = static_cast<float>(event.value) / 32767.0f;
      if (event.number == 2U || event.number == 5U) {
        v = (v + 1.0f) * 0.5f;
      }
      axes_[event.number] = v;
    }
  } else if (event.type & kTypeButton) {
    ensure_button(event.number);
    buttons_[event.number] = event.value != 0;
  }
  // Init-flag events (0x80) carry the same payloads; the branches above
  // handle them identically, which fills the initial cache correctly.
}

void JoystickTranslator::apply(InputState& state) const {
  // Modern xpad layout: 0/1 left stick, 2 left trigger (idle -32768),
  // 3/4 right stick, 5 right trigger, 6/7 dpad HAT0 (-1/0/1).
  const float lx = axes_.size() > 0U ? axes_[0] : 0.0f;
  const float ly = axes_.size() > 1U ? axes_[1] : 0.0f;
  const float lt = axes_.size() > 2U ? axes_[2] : 0.0f;
  const float rx = axes_.size() > 3U ? axes_[3] : 0.0f;
  const float ry = axes_.size() > 4U ? axes_[4] : 0.0f;
  const float rt = axes_.size() > 5U ? axes_[5] : 0.0f;
  const float dx = axes_.size() > 6U ? axes_[6] : 0.0f;
  const float dy = axes_.size() > 7U ? axes_[7] : 0.0f;

  // Left stick -> movement; right stick -> look. Standard deadzones.
  state.set_axis("move_x", stick_deadzone(lx, 0.18f));
  state.set_axis("move_y", stick_deadzone(-ly, 0.18f));
  state.set_axis("look_x", stick_deadzone(rx, 0.22f));
  state.set_axis("look_y", stick_deadzone(ry, 0.22f));

  // Triggers (post-remap [0,1]) -> zoom; dpad -> orbit + zoom. A trigger
  // at/above half pull counts (half-pull = the remapped idle midpoint).
  const bool zoom_in_trig = rt >= 0.5f;
  const bool zoom_out_trig = lt >= 0.5f;
  const bool dpad_left = dx < -0.5f;
  const bool dpad_right = dx > 0.5f;
  const bool dpad_up = dy < -0.5f;
  const bool dpad_down = dy > 0.5f;
  state.set_action("zoom_in", zoom_in_trig || dpad_up);
  state.set_action("zoom_out", zoom_out_trig || dpad_down);
  state.set_action("orbit_left", dpad_left);
  state.set_action("orbit_right", dpad_right);

  // A (button 0) -> fade toggle edge; consume-on-read so a press maps to
  // exactly one edge in game logic.
  const bool a_held = !buttons_.empty() && buttons_[0];
  state.set_action("fade_toggle", a_held && !a_prev_applied_);
  a_prev_applied_ = a_held;
}

#if defined(__linux__)

// ============================================================================
// LinuxJoystickDriver
// ============================================================================

LinuxJoystickDriver::LinuxJoystickDriver(int index) {
  const std::string path = "/dev/input/js" + std::to_string(index);
  fd_ = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
}

LinuxJoystickDriver::~LinuxJoystickDriver() {
  if (fd_ >= 0) ::close(fd_);
}

void LinuxJoystickDriver::poll(InputState& state) {
  if (fd_ < 0) return;
  // Drain every available event (8ms kernel buffer granularity).
  JsEvent event{};
  while (::read(fd_, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event))) {
    translator_.on_event(event);
  }
  translator_.apply(state);
}

#endif  // __linux__

}  // namespace omnicpp::core
