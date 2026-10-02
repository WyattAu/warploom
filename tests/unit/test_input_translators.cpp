//! @file test_input_translators.cpp
//! @brief Pure translator proofs: XCB key/mouse and Linux joystick mapping
//!        into InputState. The device-translation layer is fully testable
//!        without a display server or physical hardware — only the thin
//!        device readers (XCB event loop, /dev/input/jsN fd) contain no
//!        logic and are excluded by design.
//!
//! Joystick layout follows the canonical Linux xpad (Xbox) convention:
//! axes 0/1 left stick, 2 left trigger (idle -32768), 3/4 right stick,
//! 5 right trigger, 6/7 dpad (HAT0, values -1/0/1); button 0 = A.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "warploom/core/input_state.hpp"
#include "warploom/core/input_translators.hpp"

namespace {

using omnicpp::core::InputState;
using omnicpp::core::JoystickTranslator;
using omnicpp::core::JsEvent;
using omnicpp::core::stick_deadzone;
using omnicpp::core::XcbKeyMouseTranslator;

// Evdev keycodes used by the translator.
constexpr std::uint8_t kKeyW = 25;
constexpr std::uint8_t kKeyA = 38;
constexpr std::uint8_t kKeyS = 39;
constexpr std::uint8_t kKeyD = 40;
constexpr std::uint8_t kKeySpace = 65;
constexpr std::uint8_t kKeyLeft = 113;
constexpr std::uint8_t kKeyRight = 114;
constexpr std::uint8_t kKeyUp = 111;
constexpr std::uint8_t kKeyDown = 116;

// ============================================================================
// XCB keyboard + mouse
// ============================================================================

TEST(InputXcb, NeutralApplyProducesNoActionsAndZeroAxes) {
  XcbKeyMouseTranslator t;
  InputState in;
  t.apply(in);
  EXPECT_FALSE(in.action("zoom_in"));
  EXPECT_FALSE(in.action("zoom_out"));
  EXPECT_FALSE(in.action("orbit_left"));
  EXPECT_FALSE(in.action("orbit_right"));
  EXPECT_FLOAT_EQ(in.axis("move_x"), 0.0f);
  EXPECT_FLOAT_EQ(in.axis("move_y"), 0.0f);
  EXPECT_FLOAT_EQ(in.axis("look_x"), 0.0f);
}

TEST(InputXcb, WasdMapsToZoomOrbitAndMoveAxes) {
  XcbKeyMouseTranslator t;
  InputState in;

  t.on_key(kKeyW, true);
  t.apply(in);
  EXPECT_TRUE(in.action("zoom_in"));
  EXPECT_FLOAT_EQ(in.axis("move_y"), 1.0f);
  EXPECT_FLOAT_EQ(in.axis("move_x"), 0.0f);

  t.on_key(kKeyW, false);
  t.on_key(kKeyS, true);
  t.apply(in);
  EXPECT_TRUE(in.action("zoom_out"));
  EXPECT_FLOAT_EQ(in.axis("move_y"), -1.0f);

  t.on_key(kKeyS, false);
  t.on_key(kKeyA, true);
  t.apply(in);
  EXPECT_TRUE(in.action("orbit_left"));
  EXPECT_FLOAT_EQ(in.axis("move_x"), -1.0f);

  t.on_key(kKeyA, false);
  t.on_key(kKeyD, true);
  t.apply(in);
  EXPECT_TRUE(in.action("orbit_right"));
  EXPECT_FLOAT_EQ(in.axis("move_x"), 1.0f);
}

TEST(InputXcb, DiagonalMovementIsNormalized) {
  XcbKeyMouseTranslator t;
  t.on_key(kKeyW, true);
  t.on_key(kKeyD, true);
  InputState in;
  t.apply(in);
  const float len = std::hypot(in.axis("move_x"), in.axis("move_y"));
  EXPECT_NEAR(len, 1.0f, 1e-5f);
  EXPECT_NEAR(in.axis("move_x"), std::sqrt(0.5f), 1e-5f);
}

TEST(InputXcb, ArrowKeysMirrorWasd) {
  XcbKeyMouseTranslator t;
  t.on_key(kKeyUp, true);
  t.on_key(kKeyLeft, true);
  InputState in;
  t.apply(in);
  EXPECT_TRUE(in.action("zoom_in"));
  EXPECT_TRUE(in.action("orbit_left"));

  t.on_key(kKeyUp, false);
  t.on_key(kKeyLeft, false);
  t.on_key(kKeyDown, true);
  t.on_key(kKeyRight, true);
  t.apply(in);
  EXPECT_TRUE(in.action("zoom_out"));
  EXPECT_TRUE(in.action("orbit_right"));
}

TEST(InputXcb, KeyReleaseClearsAction) {
  XcbKeyMouseTranslator t;
  t.on_key(kKeyW, true);
  InputState in1;
  t.apply(in1);
  ASSERT_TRUE(in1.action("zoom_in"));
  t.on_key(kKeyW, false);
  InputState in2;
  t.apply(in2);
  EXPECT_FALSE(in2.action("zoom_in"));
  EXPECT_FLOAT_EQ(in2.axis("move_y"), 0.0f);
}

TEST(InputXcb, PointerDeltasAccumulateAndConsumeOnRead) {
  XcbKeyMouseTranslator t;
  t.on_motion(100.0, 100.0);  // baseline
  t.on_motion(103.0, 98.0);
  t.on_motion(104.5, 96.5);   // total +4.5, -3.5
  InputState in1;
  t.apply(in1);
  EXPECT_NEAR(in1.axis("look_x"), 4.5f, 1e-4f);
  EXPECT_NEAR(in1.axis("look_y"), -3.5f, 1e-4f);
  // Consumed: next apply is zero without new motion.
  InputState in2;
  t.apply(in2);
  EXPECT_FLOAT_EQ(in2.axis("look_x"), 0.0f);
  EXPECT_FLOAT_EQ(in2.axis("look_y"), 0.0f);
}

TEST(InputXcb, WheelIsOneTickZoomNudge) {
  XcbKeyMouseTranslator t;
  t.on_button(4, true);  // wheel up
  InputState in1;
  t.apply(in1);
  EXPECT_TRUE(in1.action("zoom_in"));
  EXPECT_FALSE(in1.action("zoom_out"));
  // Nudge does not persist to the next tick.
  InputState in2;
  t.apply(in2);
  EXPECT_FALSE(in2.action("zoom_in"));

  t.on_button(5, true);  // wheel down
  InputState in3;
  t.apply(in3);
  EXPECT_TRUE(in3.action("zoom_out"));
}

TEST(InputXcb, EscIsNotMappedToAnyAction) {
  XcbKeyMouseTranslator t;
  t.on_key(9, true);  // ESC
  InputState in;
  t.apply(in);
  EXPECT_FALSE(in.action("zoom_in"));
  EXPECT_FALSE(in.action("zoom_out"));
  EXPECT_FALSE(in.action("orbit_left"));
  EXPECT_FALSE(in.action("orbit_right"));
}

TEST(InputXcb, HeldKeyCountTracksState) {
  XcbKeyMouseTranslator t;
  EXPECT_EQ(t.held_key_count(), 0U);
  t.on_key(kKeyW, true);
  t.on_key(kKeyA, true);
  EXPECT_EQ(t.held_key_count(), 2U);
  t.on_key(kKeyA, false);
  EXPECT_EQ(t.held_key_count(), 1U);
  // Duplicate press is idempotent.
  t.on_key(kKeyW, true);
  EXPECT_EQ(t.held_key_count(), 1U);
}

// ============================================================================
// Joystick translation (xpad layout)
// ============================================================================

JsEvent axis_event(std::uint8_t number, std::int16_t value,
                   bool init = false) {
  JsEvent e;
  e.type = 0x02 | (init ? 0x80 : 0x00);
  e.number = number;
  e.value = value;
  return e;
}

JsEvent button_event(std::uint8_t number, bool pressed) {
  JsEvent e;
  e.type = 0x01;
  e.number = number;
  e.value = pressed ? 1 : 0;
  return e;
}

TEST(InputJoystick, DeadzoneRescaleMath) {
  EXPECT_FLOAT_EQ(stick_deadzone(0.0f, 0.18f), 0.0f);
  EXPECT_FLOAT_EQ(stick_deadzone(0.18f, 0.18f), 0.0f);
  EXPECT_FLOAT_EQ(stick_deadzone(-0.1f, 0.18f), 0.0f);
  // Just outside the deadzone: small positive, linearly rescaled.
  EXPECT_NEAR(stick_deadzone(0.28f, 0.18f), 0.1f / 0.82f, 1e-5f);
  // Full deflection stays 1.0 (and -1.0).
  EXPECT_NEAR(stick_deadzone(1.0f, 0.18f), 1.0f, 1e-6f);
  EXPECT_NEAR(stick_deadzone(-1.0f, 0.18f), -1.0f, 1e-6f);
}

TEST(InputJoystick, NeutralCacheProducesNoActions) {
  JoystickTranslator t;
  // Init cache-fill: all eight axes centered, no buttons.
  for (std::uint8_t i = 0; i < 8; ++i) {
    t.on_event(axis_event(i, (i == 2U || i == 5U) ? -32768 : 0, true));
  }
  InputState in;
  t.apply(in);
  EXPECT_FLOAT_EQ(in.axis("move_x"), 0.0f);
  EXPECT_FLOAT_EQ(in.axis("move_y"), 0.0f);
  EXPECT_FLOAT_EQ(in.axis("look_x"), 0.0f);
  EXPECT_FALSE(in.action("zoom_in"));
  EXPECT_FALSE(in.action("zoom_out"));
  EXPECT_FALSE(in.action("fade_toggle"));
}

TEST(InputJoystick, LeftStickDrivesMoveAxesWithDeadzone) {
  JoystickTranslator t;
  t.on_event(axis_event(0, 32767));  // full right
  t.on_event(axis_event(1, -32767)); // full up (stick -Y is up)
  InputState in;
  t.apply(in);
  EXPECT_NEAR(in.axis("move_x"), 1.0f, 1e-5f);
  EXPECT_NEAR(in.axis("move_y"), 1.0f, 1e-5f);

  // Small deflection inside the deadzone reads zero.
  JoystickTranslator small;
  small.on_event(axis_event(0, 3000));
  InputState in2;
  small.apply(in2);
  EXPECT_FLOAT_EQ(in2.axis("move_x"), 0.0f);
}

TEST(InputJoystick, RightStickDrivesLookAxes) {
  JoystickTranslator t;
  t.on_event(axis_event(3, 16384));   // right stick half right
  t.on_event(axis_event(4, -16384));  // right stick half up
  InputState in;
  t.apply(in);
  // Convention match with the pointer: look_y is screen-space (down is
  // positive), so stick up (raw -Y) reads negative.
  const float half = 16384.0f / 32767.0f;
  EXPECT_NEAR(in.axis("look_x"), stick_deadzone(half, 0.22f), 1e-5f);
  EXPECT_NEAR(in.axis("look_y"), stick_deadzone(-half, 0.22f), 1e-5f);
}

TEST(InputJoystick, TriggersZoomWithIdleRemap) {
  JoystickTranslator t;
  // Right trigger (axis 5) half pull: idle -32768, so half = ~0.
  t.on_event(axis_event(5, 0));
  InputState in1;
  t.apply(in1);
  EXPECT_TRUE(in1.action("zoom_in"));
  // Full release (idle value) is not a zoom.
  JoystickTranslator t2;
  t2.on_event(axis_event(5, -32768));
  InputState in2;
  t2.apply(in2);
  EXPECT_FALSE(in2.action("zoom_in"));
  // Left trigger (axis 2) full pull -> zoom_out.
  JoystickTranslator t3;
  t3.on_event(axis_event(2, 32767));
  InputState in3;
  t3.apply(in3);
  EXPECT_TRUE(in3.action("zoom_out"));
}

TEST(InputJoystick, DpadMapsToOrbitAndZoom) {
  JoystickTranslator t;
  t.on_event(axis_event(6, -1));  // HAT0X left
  t.on_event(axis_event(7, -1));  // HAT0Y up
  InputState in;
  t.apply(in);
  EXPECT_TRUE(in.action("orbit_left"));
  EXPECT_TRUE(in.action("zoom_in"));
  EXPECT_FALSE(in.action("orbit_right"));
  EXPECT_FALSE(in.action("zoom_out"));

  JoystickTranslator t2;
  t2.on_event(axis_event(6, 1));  // right
  t2.on_event(axis_event(7, 1));  // down
  InputState in2;
  t2.apply(in2);
  EXPECT_TRUE(in2.action("orbit_right"));
  EXPECT_TRUE(in2.action("zoom_out"));
}

TEST(InputJoystick, ButtonAIsASingleEdge) {
  JoystickTranslator t;
  InputState in1;
  t.apply(in1);  // establish previous state (not pressed)
  t.on_event(button_event(0, true));
  InputState in2;
  t.apply(in2);
  EXPECT_TRUE(in2.action("fade_toggle"));  // edge this tick
  // Held: no new edge.
  InputState in3;
  t.apply(in3);
  EXPECT_FALSE(in3.action("fade_toggle"));
  // Release: no edge.
  t.on_event(button_event(0, false));
  InputState in4;
  t.apply(in4);
  EXPECT_FALSE(in4.action("fade_toggle"));
}

TEST(InputJoystick, InitEventsFillCacheWithoutActionSpikes) {
  JoystickTranslator t;
  // Trigger init events carry the idle -32768; they must not read as zoom.
  t.on_event(axis_event(2, -32768, true));
  t.on_event(axis_event(5, -32768, true));
  InputState in;
  t.apply(in);
  EXPECT_FALSE(in.action("zoom_in"));
  EXPECT_FALSE(in.action("zoom_out"));
}

// ============================================================================
// Linux device driver shell (no hardware required)
// ============================================================================

#if defined(__linux__)
TEST(InputJoystickDevice, AbsentDeviceIsNotAnError) {
  // js63 does not exist on any realistic system; the driver must open-fail
  // gracefully and poll as a no-op.
  omnicpp::core::LinuxJoystickDriver driver(63);
  EXPECT_FALSE(driver.is_open());
  InputState in;
  driver.poll(in);  // must not crash or write anything
  EXPECT_FLOAT_EQ(in.axis("move_x"), 0.0f);
  EXPECT_FALSE(in.action("zoom_in"));
}
#endif

}  // namespace
