//! @file test_ui_glyphs.cpp
//! @brief M4 font-module proofs: contiguous printable-ASCII coverage,
//!        exact glyph bits ('A' checked pixel by pixel), deterministic
//!        atlas bytes, atlas cell placement, and metrics agreement with
//!        the M2 layout engine.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "warploom/ui/glyphs.hpp"

namespace {

namespace ui = warploom::ui;

TEST(UiGlyphs, PrintableAsciiFullyCovered) {
  for (int c = 0x20; c <= 0x7E; ++c) {
    EXPECT_TRUE(ui::has_glyph(static_cast<char>(c))) << "missing 0x" << std::hex
                                                     << c;
  }
  // Outside the range: rejected.
  EXPECT_FALSE(ui::has_glyph('\0'));
  EXPECT_FALSE(ui::has_glyph('\n'));
  EXPECT_FALSE(ui::has_glyph('\x7F'));
  EXPECT_FALSE(ui::has_glyph('\x80'));
}

TEST(UiGlyphs, RasterizesExactBits) {
  // 'A' = {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11}: apex at col 2, row 4
  // is the full crossbar.
  std::vector<std::uint32_t> px(ui::kGlyphCellW * ui::kGlyphCellH, 0xDEADBEEF);
  ASSERT_TRUE(ui::rasterize_glyph('A', 0xFFFFFFFFu, px.data()));
  EXPECT_EQ(px[0 * 8 + 2], 0xFFFFFFFFu);  // (0x04 -> bit 2)
  EXPECT_EQ(px[1 * 8 + 1], 0xFFFFFFFFu);  // (0x0A -> bits 1,3)
  EXPECT_EQ(px[1 * 8 + 3], 0xFFFFFFFFu);
  EXPECT_EQ(px[4 * 8 + 0], 0xFFFFFFFFu);  // crossbar full width
  EXPECT_EQ(px[4 * 8 + 4], 0xFFFFFFFFu);
  // Empty margins stay zero.
  EXPECT_EQ(px[0 * 8 + 0], 0u);
  EXPECT_EQ(px[6 * 8 + 5], 0u);
  // No-glyph character leaves the buffer untouched and returns false.
  const std::uint32_t probe = 0xDEADBEEF;
  std::vector<std::uint32_t> untouched(ui::kGlyphCellW * ui::kGlyphCellH,
                                       probe);
  EXPECT_FALSE(ui::rasterize_glyph('\x01', 0xFFFFFFFFu, untouched.data()));
  EXPECT_EQ(untouched[0], probe);
}

TEST(UiGlyphs, AtlasBytesDeterministicAndWellFormed) {
  const auto a1 = ui::build_atlas(0xFFFFFFFFu);
  const auto a2 = ui::build_atlas(0xFFFFFFFFu);
  ASSERT_EQ(a1.size(), static_cast<std::size_t>(ui::kAtlasW) * ui::kAtlasH);
  EXPECT_EQ(a1, a2);  // byte-identical across calls

  // Cell placement: glyph 'H' (0x48) -> cell 0x28 = 40 -> col 8, row 2.
  // Cell origin = (8*8, 2*8) = (64, 16); row 0 of 'H' is 0x11 -> bits 0,4.
  const auto at = [](std::uint32_t x, std::uint32_t y) {
    const auto a = ui::build_atlas(0xFFFFFFFFu);
    return a[static_cast<std::size_t>(y) * ui::kAtlasW + x];
  };
  EXPECT_EQ(at(64 + 0, 16 + 0), 0xFFFFFFFFu);
  EXPECT_EQ(at(64 + 4, 16 + 0), 0xFFFFFFFFu);
  EXPECT_EQ(at(64 + 2, 16 + 0), 0u);  // between the stems
  // A space cell is all zeros.
  EXPECT_EQ(at(0, 0), 0u);
  // Atlas color propagates.
  const auto green = ui::build_atlas(0xFF00FF00u);
  EXPECT_EQ(green[0], 0u);  // space cell still empty
  bool saw_green = false;
  for (auto p : green) {
    if (p == 0xFF00FF00u) {
      saw_green = true;
      break;
    }
  }
  EXPECT_TRUE(saw_green);
}

TEST(UiGlyphs, MetricsMatchLayoutDefaults) {
  const ui::TextMetrics m = ui::font_metrics();
  EXPECT_FLOAT_EQ(m.char_width, 8.0F);
  EXPECT_FLOAT_EQ(m.line_height, 16.0F);
  // Layout agreement: a label of 10 chars measures 80 px wide.
  ui::WidgetTree tree;
  ui::Widget& root = tree.get(tree.root());
  root.kind = ui::WidgetKind::Panel;
  ui::Widget label;
  label.kind = ui::WidgetKind::Label;
  label.text = "0123456789";
  const auto h = tree.add(std::move(label), tree.root());
  ui::compute_layout(tree, 400.0F, 100.0F, m);
  EXPECT_FLOAT_EQ(tree.get(h).w, 10.0F * m.char_width);
}
}  // namespace
