//! @file test_xcb_keymap.cpp
//! @brief Contract pins for the XCB param-editor letter table.
//!
//! The table is frozen to the server's measured layout (pc105/US: codes
//! from `xkbcomp :0 -`), so these tests pin every letter row, every
//! punctuation gap, and both range bounds. Pure integers — no X server.
//! Runs in every preset.

#include <gtest/gtest.h>

#include "xcb_keymap.hpp"

namespace {

using xcb_keymap::xcb_code_to_lower;

// Q-row 24..33 (measured: 24=q, 29=y on pc105/us — the old table put
// 'z' at 30 and swallowed the real 'q' at 24).
TEST(XcbKeymap, QRowCorrect) {
  EXPECT_EQ(xcb_code_to_lower(24), 'q');
  EXPECT_EQ(xcb_code_to_lower(25), 'w');
  EXPECT_EQ(xcb_code_to_lower(26), 'e');
  EXPECT_EQ(xcb_code_to_lower(27), 'r');
  EXPECT_EQ(xcb_code_to_lower(28), 't');
  EXPECT_EQ(xcb_code_to_lower(29), 'y');
  EXPECT_EQ(xcb_code_to_lower(30), 'u');
  EXPECT_EQ(xcb_code_to_lower(31), 'i');
  EXPECT_EQ(xcb_code_to_lower(32), 'o');
  EXPECT_EQ(xcb_code_to_lower(33), 'p');
}

// A-row 38..46 (measured: 38=a — the old table put 'a' at 36,
// Return's code, and yielded 'd' for the real 'a').
TEST(XcbKeymap, ARowCorrect) {
  EXPECT_EQ(xcb_code_to_lower(38), 'a');
  EXPECT_EQ(xcb_code_to_lower(39), 's');
  EXPECT_EQ(xcb_code_to_lower(40), 'd');
  EXPECT_EQ(xcb_code_to_lower(41), 'f');
  EXPECT_EQ(xcb_code_to_lower(42), 'g');
  EXPECT_EQ(xcb_code_to_lower(43), 'h');
  EXPECT_EQ(xcb_code_to_lower(44), 'j');
  EXPECT_EQ(xcb_code_to_lower(45), 'k');
  EXPECT_EQ(xcb_code_to_lower(46), 'l');
}

// Z-row 52..58 (measured: 52=z — the old table placed this row at
// 45..51 and read 52..58 out of bounds).
TEST(XcbKeymap, ZRowCorrect) {
  EXPECT_EQ(xcb_code_to_lower(52), 'z');
  EXPECT_EQ(xcb_code_to_lower(53), 'x');
  EXPECT_EQ(xcb_code_to_lower(54), 'c');
  EXPECT_EQ(xcb_code_to_lower(55), 'v');
  EXPECT_EQ(xcb_code_to_lower(56), 'b');
  EXPECT_EQ(xcb_code_to_lower(57), 'n');
  EXPECT_EQ(xcb_code_to_lower(58), 'm');
}

// Punctuation gaps and the range bounds yield 0 (swallowed, never
// typed): the bound is what contains the old out-of-bounds read.
TEST(XcbKeymap, GapsAndBoundsYieldZero) {
  EXPECT_EQ(xcb_code_to_lower(34), 0);
  EXPECT_EQ(xcb_code_to_lower(35), 0);
  EXPECT_EQ(xcb_code_to_lower(36), 0);  // Return (also handled upstream)
  EXPECT_EQ(xcb_code_to_lower(37), 0);
  EXPECT_EQ(xcb_code_to_lower(47), 0);
  EXPECT_EQ(xcb_code_to_lower(48), 0);
  EXPECT_EQ(xcb_code_to_lower(49), 0);
  EXPECT_EQ(xcb_code_to_lower(50), 0);
  EXPECT_EQ(xcb_code_to_lower(51), 0);
  EXPECT_EQ(xcb_code_to_lower(23), 0);
  EXPECT_EQ(xcb_code_to_lower(59), 0);
  EXPECT_EQ(xcb_code_to_lower(0), 0);
  EXPECT_EQ(xcb_code_to_lower(255), 0);
}

}  // namespace
