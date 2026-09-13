#pragma once

//! @file glyphs.hpp
//! @brief M4 engine bitmap font: deterministic 5x7 glyphs, text measurement,
//!        and atlas packing for GPU text rendering.
//!
//! The font is engine-owned (no system fonts, no external files): every
//! printable ASCII character is a fixed 5x7 bit pattern, so text measurement
//! and rasterization are identical on every machine — the same determinism
//! contract as the rest of the engine. The GPU path packs the glyphs into a
//! tightly-packed atlas (kGlyphColumns x kGlyphRows cells of 8x8 px each,
//! leaving a 1px/3px guard so linear sampling cannot bleed neighbours);
//! the CPU path rasterizes straight from the bits.
//!
//! Contracts (machine-checked in test_ui_glyphs.cpp):
//!   - glyph coverage is contiguous over printable ASCII [0x20, 0x7E];
//!   - atlas bytes are deterministic (same build → same bytes);
//!   - measure_text matches the metrics used by the M2 layout engine
//!     (char_width advance per character).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "engine/ui/widget.hpp"

namespace omnicpp::ui {

//! Glyph cell geometry in the atlas (each glyph lives in one cell).
inline constexpr std::uint32_t kGlyphCellW = 8;
inline constexpr std::uint32_t kGlyphCellH = 8;
//! Atlas grid: 16 columns x 8 rows = 128 cells; cells [0, 126] hold the
//! printable ASCII glyphs, cell 127 (last) is reserved SOLID WHITE so the
//! UI renderer can point solid rects at a known-opaque texel.
inline constexpr std::uint32_t kGlyphColumns = 16;
inline constexpr std::uint32_t kGlyphRows = 8;
inline constexpr std::uint32_t kGlyphCellCount = kGlyphColumns * kGlyphRows;  // 128
inline constexpr std::uint32_t kSolidCell = kGlyphCellCount - 1;              // 127
inline constexpr std::uint32_t kAtlasW = kGlyphColumns * kGlyphCellW;  // 128
inline constexpr std::uint32_t kAtlasH = kGlyphRows * kGlyphCellH;     // 64

//! True when `c` has a defined glyph.
[[nodiscard]] bool has_glyph(char c) noexcept;

//! Rasterizes one glyph's 5x7 bits into `out_pixels` (row-major RGBA8,
//! pitch = 8 px). Pixels outside the 5x7 box stay 0. Returns false when the
//! character has no glyph.
[[nodiscard]] bool rasterize_glyph(char c, std::uint32_t color,
                                   std::uint32_t* out_pixels) noexcept;

//! Packed RGBA8 atlas (kAtlasW x kAtlasH), deterministic bytes. Cell (col,
//! row) holds glyph 0x20 + row*16 + col, rasterized at the cell origin with
//! a 1px bottom/right guard inside the 8x8 cell — except the reserved last
//! cell (kSolidCell), which is filled solid so rect quads sample opaque
//! white regardless of the glyph color channel.
[[nodiscard]] std::vector<std::uint32_t> build_atlas(
    std::uint32_t color = 0xFFFFFFFFu);

//! Packed height (pixels of glyph advance) of one line of text — the M2
//! layout engine uses `TextMetrics` (char_width, line_height); this returns
//! the same numbers so shader-side scaling agrees with layout.
[[nodiscard]] TextMetrics font_metrics() noexcept;

}  // namespace omnicpp::ui
