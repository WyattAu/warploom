#pragma once

//! @file xcb_keymap.hpp
//! @brief Pure XCB param-editor letter mapping (input-hardening pass).
//!
//! Included by both the viewport (XCB key-press path) and the unit suite
//! (contract pins). Takes plain integers — no xcb dependency here.
//!
//! The XCB path has no layout engine (unlike the Wayland seat, which gets
//! layout-correct chars from the xkbcommon keysym), so this table is
//! frozen to the server's measured layout: pc105 model, US layout
//! (`setxkbmap -query` on :0; keycodes verified against the live server
//! via `xkbcomp :0 -`: 24=q..33=p, 38=a..46=l, 52=z..58=m). A server
//! running another layout mistypes letters on XCB — documented, not
//! fixable without an XCB xkbcommon client (out of scope).
//!
//! History (measured, loop70): the previous inline table,
//! "?qwertzuiop?asdfghjkl?yxcvbnm" (29 entries, indexed code-24),
//! matched no real layout — Q-row shifted +1 ('q' at 25), A-row +2
//! ('a' at 36, Return's code), Z-row at 45..51 instead of 52..58, and
//! codes 52..58 read out of bounds. Every letter it produced was wrong
//! or garbage; the guard (`<=51`) only contained the OOB read.

#include <cstdint>

namespace xcb_keymap {

//! X11 letter keycode -> lowercase ASCII for the param editor. Covers the
//! three letter rows (24..58); every other code — punctuation gaps,
//! digits (handled by the kDigitRow path), Return/ESC/BackSpace —
//! yields 0 and is swallowed by the caller exactly as before.
[[nodiscard]] inline char xcb_code_to_lower(std::uint8_t code) noexcept {
  if (code < 24U || code > 58U) return 0;
  // Index by code-24. Verified against `xkbcomp :0 -` (pc105/us):
  // q-row 24..33, A-row 38..46, Z-row 52..58; gaps are non-letters.
  static constexpr char kTable[] =
      "qwertyuiop"   // 24..33
      "????"         // 34..37 (brackets, Return handled upstream)
      "asdfghjkl"    // 38..46
      "?????"        // 47..51 (semicolon..tilde row tail)
      "zxcvbnm";     // 52..58
  static_assert(sizeof(kTable) == 36U);  // 35 entries + NUL; length and
                                         // range check above must agree
  const char c = kTable[code - 24U];
  return (c == '?') ? 0 : c;
}

}  // namespace xcb_keymap
