//! @file test_wayland_pointer.cpp
//! @brief Contract pins for the Wayland-seat pointer mappings (loop71).
//!
//! The viewport feeds its existing X11-detail paths with translated Linux
//! button codes, so these tests pin the exact numbers both sides agree
//! on: the three button details the canvas/drag/wire paths switch on, the
//! ignore-zero for everything else, and the wl_fixed divisor the motion
//! path converts with. Pure integers — no compositor, no devices. Runs in
//! every preset.

#include <gtest/gtest.h>

#include "wayland_pointer.hpp"

namespace {

using wayland_pointer::linux_button_to_x11;
using wayland_pointer::pointer_fixed_to_double;

// poll_events switches on detail 1 (canvas/toolbar/timeline/clip), 2 is
// passed through, 3 unlinks wires. Wayland must reproduce all three.
TEST(WaylandPointer, ButtonsMatchXcbDetails) {
  EXPECT_EQ(linux_button_to_x11(0x110U), 1);  // BTN_LEFT -> primary
  EXPECT_EQ(linux_button_to_x11(0x112U), 2);  // BTN_MIDDLE
  EXPECT_EQ(linux_button_to_x11(0x111U), 3);  // BTN_RIGHT
}

TEST(WaylandPointer, UnknownButtonsIgnored) {
  EXPECT_EQ(linux_button_to_x11(0), 0);
  EXPECT_EQ(linux_button_to_x11(0x113U), 0);  // BTN_SIDE
  EXPECT_EQ(linux_button_to_x11(0x114U), 0);  // BTN_EXTRA
  EXPECT_EQ(linux_button_to_x11(0x100U), 0);  // BTN_MISC (below range)
  EXPECT_EQ(linux_button_to_x11(0xdeadU), 0);
}

// wl_fixed_t is signed 24.8: 256 units per pixel, sign-preserving so
// off-surface (negative) positions survive the conversion the same way
// XCB out-of-window coordinates do.
TEST(WaylandPointer, FixedConversionIsProtocolDivisor) {
  EXPECT_DOUBLE_EQ(pointer_fixed_to_double(256), 1.0);
  EXPECT_DOUBLE_EQ(pointer_fixed_to_double(128), 0.5);
  EXPECT_DOUBLE_EQ(pointer_fixed_to_double(0), 0.0);
  EXPECT_DOUBLE_EQ(pointer_fixed_to_double(-512), -2.0);
  EXPECT_DOUBLE_EQ(pointer_fixed_to_double(256 * 1280 + 64), 1280.25);
}

}  // namespace
