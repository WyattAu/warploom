//! @file test_ui_paint_golden.cpp
//! @brief Golden-image proofs for the M2 paint pipeline: the widget tree is
//!        laid out, painted to a PaintList, and rasterized through the
//!        deterministic software rasterizer. Assertions are on ACTUAL
//!        PIXELS — fill colors, border strips, z-order (button over panel),
//!        hidden-subtree culling, glyph-origin placement, and byte-level
//!        determinism (two runs → identical frame hash). No GPU required,
//!        so this runs on every CI leg.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "warploom/render/software_rasterizer.hpp"
#include "warploom/ui/widget.hpp"

namespace {

namespace ui = warploom::ui;
using omnicpp::render::SoftwareRasterizer;

constexpr ui::TextMetrics kMetrics{};  // 8 px chars, 16 px lines

//! Rasterizes a paint list into a software framebuffer (UI draw rules:
//! rects in order — fill then border strips — text runs last). The software
//! rasterizer depth-tests with strict less-than, so each successive
//! primitive gets a slightly LOWER z (painter's algorithm in z form).
class UiRasterizer final {
 public:
  UiRasterizer(std::uint32_t w, std::uint32_t h, std::uint32_t clear)
      : r_(w, h) {
    r_.clear(clear);
  }

  void draw(const ui::PaintList& p) {
    for (const auto& rect : p.rects) {
      fill_rect(rect.x, rect.y, rect.w, rect.h, rect.color);
      if (rect.border_color >> 24 != 0U && rect.border_width > 0.0F) {
        const float bw = rect.border_width;
        fill_rect(rect.x, rect.y, rect.w, bw, rect.border_color);                      // top
        fill_rect(rect.x, rect.y + rect.h - bw, rect.w, bw, rect.border_color);        // bottom
        fill_rect(rect.x, rect.y + bw, bw, rect.h - 2.0F * bw, rect.border_color);     // left
        fill_rect(rect.x + rect.w - bw, rect.y + bw, bw, rect.h - 2.0F * bw,
                  rect.border_color);                                                  // right
      }
    }
    for (const auto& t : p.texts) {
      draw_text(t.x, t.y, t.text, t.color);
    }
  }

  [[nodiscard]] std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const {
    return r_.get_pixel(x, y);
  }
  [[nodiscard]] std::uint64_t hash() const { return r_.frame_hash(); }
  [[nodiscard]] std::uint32_t width() const { return r_.width(); }
  [[nodiscard]] std::uint32_t height() const { return r_.height(); }

 private:
  void quad(float x, float y, float w, float h, std::uint32_t c) {
    const float z = z_;
    r_.draw_triangle(x, y, z, c, x + w, y, z, c, x + w, y + h, z, c);
    r_.draw_triangle(x, y, z, c, x + w, y + h, z, c, x, y + h, z, c);
  }

  void fill_rect(float x, float y, float w, float h, std::uint32_t c) {
    if (w <= 0.0F || h <= 0.0F) {
      return;
    }
    // Pixel-aligned fill: iterate the integer coverage directly so 1-px
    // strips are exact (triangle rasterization of sub-pixel quads is the
    // renderer's job, not this test's).
    const auto x0 = static_cast<int>(x);
    const auto y0 = static_cast<int>(y);
    const auto x1 = static_cast<int>(x + w);
    const auto y1 = static_cast<int>(y + h);
    for (int py = y0; py < y1; ++py) {
      for (int px = x0; px < x1; ++px) {
        draw_px(px, py, c);
      }
    }
    z_ -= 1.0e-6F;  // next rect wins over this one
  }

  void draw_px(int x, int y, std::uint32_t c) {
    if (x < 0 || y < 0 ||
        x >= static_cast<int>(r_.width()) ||
        y >= static_cast<int>(r_.height())) {
      return;
    }
    quad(static_cast<float>(x), static_cast<float>(y), 1.0F, 1.0F, c);
  }

  //! 5x7 built-in test glyphs (uppercase + digits we use). Deterministic and
  //! test-local: pixel-perfect font rendering ships with the M3 font module.
  static constexpr int kGlyphW = 5;
  static constexpr int kGlyphH = 7;
  //! Rows, MSB-left. Only the glyphs the tests use are defined.
  static const char* glyph(const char ch) {
    switch (ch) {
      // 7 rows x 5 cols = 35 cells (rows used to be 6 and the row-7 read
      // ran off the end of the literal; ASan caught it). Row 7 is blank.
      case 'O': return "011101000110001100011000111011100000";
      case 'K': return "100011001010100110001010011000100000";
      case 'X': return "100011000101010001000101011000100000";
      default:  return nullptr;
    }
  }

  void draw_text(float x, float y, const std::string& text,
                 std::uint32_t color) {
    float cx = x;
    for (const char ch : text) {
      const char* rows = glyph(ch);
      if (rows != nullptr) {
        for (std::size_t gy = 0; gy < static_cast<std::size_t>(kGlyphH); ++gy) {
          for (std::size_t gx = 0; gx < static_cast<std::size_t>(kGlyphW); ++gx) {
            if (rows[gy * static_cast<std::size_t>(kGlyphW) + gx] == '1') {
              draw_px(static_cast<int>(cx) + static_cast<int>(gx),
               static_cast<int>(y) + static_cast<int>(gy),
                      color);
            }
          }
        }
      }
      cx += kMetrics.char_width;
    }
    z_ -= 1.0e-6F;  // next text run layers above the previous
  }

  SoftwareRasterizer r_;
  float z_{0.999F};  //!< painter's-algorithm depth cursor (see class note)
};

//! Builds the demo tree: root panel, bordered sidebar with two buttons, a
//! checkbox, and a hidden subtree that must contribute nothing.
struct DemoUi {
  ui::WidgetTree tree;
  ui::PaintList paint;

  DemoUi() {
    ui::Widget& root = tree.get(tree.root());
    root.kind = ui::WidgetKind::Panel;
    root.color = 0xFF202020;

    ui::Widget sidebar;
    sidebar.kind = ui::WidgetKind::Panel;
    sidebar.name = "sidebar";
    sidebar.color = 0xFF303030;
    sidebar.border_color = 0xFFFF0000;
    sidebar.border_width = 2.0F;
    sidebar.fixed_w = 120.0F;
    sidebar.flex_grow = 1.0F;  // main-axis (vertical): fill viewport height
    sidebar.padding = 8.0F;
    sidebar.spacing = 6.0F;
    const auto hs = tree.add(std::move(sidebar), tree.root());

    ui::Widget open;
    open.kind = ui::WidgetKind::Button;
    open.name = "open";
    open.text = "OK";
    open.color = 0xFF5050F0;
    open.text_color = 0xFFFFFFFF;
    open.fixed_h = 24.0F;
    open.cross_align = ui::Align::Stretch;  // full sidebar content width
    (void)tree.add(std::move(open), hs);

    ui::Widget quit;
    quit.kind = ui::WidgetKind::Button;
    quit.name = "quit";
    quit.text = "X";
    quit.color = 0xFFF05050;
    quit.text_color = 0xFFFFFFFF;
    quit.fixed_h = 24.0F;
    quit.cross_align = ui::Align::Stretch;
    (void)tree.add(std::move(quit), hs);

    ui::Widget snap;
    snap.kind = ui::WidgetKind::Checkbox;
    snap.name = "snap";
    snap.text = "O";
    snap.color = 0xFF00C000;
    snap.text_color = 0xFF00C000;  // label drawn in the same green
    snap.checked = true;
    snap.padding = 4.0F;
    (void)tree.add(std::move(snap), hs);

    ui::Widget hidden;
    hidden.kind = ui::WidgetKind::Panel;
    hidden.name = "hidden";
    hidden.visible = false;
    hidden.color = 0xFFFF00FF;
    hidden.fixed_w = 40.0F;
    hidden.fixed_h = 40.0F;
    (void)tree.add(std::move(hidden), tree.root());

    ui::compute_layout(tree, 320.0F, 240.0F, kMetrics);
    ui::paint(tree, paint, kMetrics);
  }
};

// ============================================================================
// Golden pixel proofs
// ============================================================================

TEST(UiPaintGolden, RootFillAndSidebarZOrder) {
  DemoUi d;
  UiRasterizer fb(320, 240, 0xFF000000);
  fb.draw(d.paint);

  // Right side (past the 120-px sidebar): root panel fill.
  EXPECT_EQ(fb.pixel(300, 120), 0xFF202020U);
  // Sidebar interior (inside the 2-px border): sidebar fill.
  EXPECT_EQ(fb.pixel(100, 120), 0xFF303030U);
  // Sidebar border strips: exactly the border color, full height (the
  // hidden panel is display:none, so the sidebar's flex grow fills 240 px).
  EXPECT_EQ(fb.pixel(0, 120), 0xFFFF0000U);
  EXPECT_EQ(fb.pixel(119, 120), 0xFFFF0000U);
  EXPECT_EQ(fb.pixel(60, 0), 0xFFFF0000U);
  EXPECT_EQ(fb.pixel(60, 239), 0xFFFF0000U);
}

TEST(UiPaintGolden, ButtonsDrawOverSidebar) {
  DemoUi d;
  UiRasterizer fb(320, 240, 0xFF000000);
  fb.draw(d.paint);

  // First button: y = 8 (padding), h = 24, spans the sidebar's content box.
  // Center of the open button must be its fill, not the sidebar's.
  EXPECT_EQ(fb.pixel(60, 20), 0xFF5050F0U);
  // Second button below it (24 + 6 spacing): quit button fill.
  EXPECT_EQ(fb.pixel(60, 50), 0xFFF05050U);
  // Between the buttons (the 6-px gap): sidebar fill shows through.
  EXPECT_EQ(fb.pixel(60, 36), 0xFF303030U);
}

TEST(UiPaintGolden, HiddenSubtreeContributesNothing) {
  DemoUi d;
  UiRasterizer fb(320, 240, 0xFF000000);
  fb.draw(d.paint);

  // The hidden panel is display:none: it consumed no layout space, and the
  // paint list itself carries no magenta rect.
  for (const auto& r : d.paint.rects) {
    EXPECT_NE(r.color, 0xFFFF00FFU);
  }
}

TEST(UiPaintGolden, CheckboxToggleSquareAndLabel) {
  DemoUi d;
  UiRasterizer fb(320, 240, 0xFF000000);
  fb.draw(d.paint);

  // Checkbox row starts after both buttons: y = 8 + 24 + 6 + 24 + 6 = 68.
  // Toggle square: x = 8 + 4 = 12, side = 9.6 -> 9 px coverage, y = 75.
  // Checked, so it paints the widget's green.
  EXPECT_EQ(fb.pixel(13, 75), 0xFF00C000U);
  // Label 'O' glyph at text origin (25.6, 72): row 0 "01110" lights (2,0)
  // -> pixel (27, 72), drawn in text_color (green).
  EXPECT_EQ(fb.pixel(27, 72), 0xFF00C000U);
}

TEST(UiPaintGolden, GlyphOriginMatchesPaintTextPosition) {
  // Minimal tree: one button, text 'OK'.
  ui::WidgetTree tree;
  ui::Widget& root = tree.get(tree.root());
  root.kind = ui::WidgetKind::Panel;
  root.color = 0xFF000000;
  ui::Widget btn;
  btn.kind = ui::WidgetKind::Button;
  btn.text = "OK";
  btn.color = 0xFF101010;
  btn.text_color = 0xFFFFFFFF;
  btn.fixed_w = 40.0F;
  btn.fixed_h = 24.0F;
  (void)tree.add(std::move(btn), tree.root());
  ui::compute_layout(tree, 100.0F, 100.0F, kMetrics);
  ui::PaintList p;
  ui::paint(tree, p, kMetrics);

  ASSERT_EQ(p.texts.size(), 1U);
  const auto tx = static_cast<std::uint32_t>(p.texts[0].x);
  const auto ty = static_cast<std::uint32_t>(p.texts[0].y);

  UiRasterizer fb(100, 100, 0xFF000000);
  fb.draw(p);
  // 'O' glyph: row 0 is "01110" -> pixels (1..3, 0) lit; pixel (2, 0) is the
  // center of the top bar, at text origin + glyph offset.
  EXPECT_EQ(fb.pixel(tx + 2, ty + 0), 0xFFFFFFFFU);
  // Outside the glyph but inside the button fill (button spans the origin).
  EXPECT_EQ(fb.pixel(tx + 0, ty + 0), 0xFF101010U);
  // 'O' row 3 "10001": left stem lit, center hole shows button fill.
  EXPECT_EQ(fb.pixel(tx + 0, ty + 3), 0xFFFFFFFFU);
  EXPECT_EQ(fb.pixel(tx + 2, ty + 3), 0xFF101010U);
  // 'K' starts one char advance later: its top-left stem pixel.
  EXPECT_EQ(fb.pixel(tx + 8 + 0, ty + 0), 0xFFFFFFFFU);
}

TEST(UiPaintGolden, DeterministicFrameHash) {
  const auto run = [] {
    DemoUi d;
    UiRasterizer fb(320, 240, 0xFF000000);
    fb.draw(d.paint);
    return fb.hash();
  };
  EXPECT_EQ(run(), run());
}
}  // namespace
