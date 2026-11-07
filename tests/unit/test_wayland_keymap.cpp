//! @file test_wayland_keymap.cpp
//! @brief Contract pins for the Wayland-seat input mappings (loop70).
//!
//! The viewport feeds its existing X11-code paths with evdev + 8, so these
//! tests pin the exact numbers both sides agree on: the physical-position
//! keycodes the action vocabulary and editor chords use, and the
//! printable range the param editor accepts. Pure integers — no
//! compositor, no devices. Runs in every preset.

#include <gtest/gtest.h>

#include "wayland_keymap.hpp"

namespace {

using wayland_keymap::evdev_to_x11;
using wayland_keymap::keysym_to_printable;

// The action vocabulary (input_translators.cpp: kKeyW/A/S/D etc.) and the
// editor chords (poll_events: 54/55 C/V, 9 ESC, 36 Return, 22 BackSpace,
// 119 Delete, 111/116/113/114 arrows) are X11 codes. Wayland delivers
// evdev; evdev + 8 must reproduce every one of them.
TEST(WaylandKeymap, PhysicalPositionsMatchXcbCodes) {
  EXPECT_EQ(evdev_to_x11(1), 9);    // ESC
  EXPECT_EQ(evdev_to_x11(17), 25);  // W
  EXPECT_EQ(evdev_to_x11(30), 38);  // A
  EXPECT_EQ(evdev_to_x11(31), 39);  // S
  EXPECT_EQ(evdev_to_x11(32), 40);  // D
  EXPECT_EQ(evdev_to_x11(46), 54);  // C (copy chord position)
  EXPECT_EQ(evdev_to_x11(47), 55);  // V (paste chord position)
  EXPECT_EQ(evdev_to_x11(57), 65);  // Space
  EXPECT_EQ(evdev_to_x11(28), 36);  // Return
  EXPECT_EQ(evdev_to_x11(14), 22);  // BackSpace
  EXPECT_EQ(evdev_to_x11(111), 119);  // Delete
  EXPECT_EQ(evdev_to_x11(103), 111);  // Up
  EXPECT_EQ(evdev_to_x11(108), 116);  // Down
  EXPECT_EQ(evdev_to_x11(105), 113);  // Left
  EXPECT_EQ(evdev_to_x11(106), 114);  // Right
  // Digit row and punctuation the param editor accepts by position.
  EXPECT_EQ(evdev_to_x11(2), 10);
  EXPECT_EQ(evdev_to_x11(11), 19);
  EXPECT_EQ(evdev_to_x11(52), 60);  // period position
  EXPECT_EQ(evdev_to_x11(53), 61);  // minus position
}

TEST(WaylandKeymap, OverflowYieldsZero) {
  EXPECT_EQ(evdev_to_x11(247), 255);
  EXPECT_EQ(evdev_to_x11(248), 0);
  EXPECT_EQ(evdev_to_x11(1000), 0);
}

// The keysym IS the character (xkbcommon decodes layout + modifiers), so
// the printable contract is a range: ASCII printables pass, everything
// else (control chars, NoSymbol, Delete-the-command) is rejected.
TEST(WaylandKeymap, PrintableRangePassesThrough) {
  EXPECT_EQ(keysym_to_printable(0x61), 'a');
  EXPECT_EQ(keysym_to_printable(0x41), 'A');
  EXPECT_EQ(keysym_to_printable(0x31), '1');
  EXPECT_EQ(keysym_to_printable(0x20), ' ');
  EXPECT_EQ(keysym_to_printable(0x2e), '.');
  EXPECT_EQ(keysym_to_printable(0x2d), '-');
  EXPECT_EQ(keysym_to_printable(0x7e), '~');
}

TEST(WaylandKeymap, NonPrintableRejected) {
  EXPECT_EQ(keysym_to_printable(0), 0);
  EXPECT_EQ(keysym_to_printable(0x1b), 0);  // Escape keysym, not text
  EXPECT_EQ(keysym_to_printable(0x0d), 0);  // Return keysym, not text
  EXPECT_EQ(keysym_to_printable(0x7f), 0);  // Delete keysym, not text
  EXPECT_EQ(keysym_to_printable(0x100), 0);
  EXPECT_EQ(keysym_to_printable(0xff08), 0);  // BackSpace keysym
}

}  // namespace
