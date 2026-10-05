//! @file test_ui_widget.cpp
//! @brief M2 UI-foundation proofs: arena integrity (remove keeps indices,
//!        O(1) append, subtree preserved), deterministic layout (flex
//!        distribution, cross alignment, container wrap), paint order
//!        (parent before child, text after rects), visibility culling, and
//!        determinism (same tree → identical paint list, twice).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "warploom/ui/widget.hpp"

namespace {

using warploom::ui::Align;
using warploom::ui::StackDirection;
using warploom::ui::TextMetrics;
using warploom::ui::Widget;
using warploom::ui::WidgetKind;
using warploom::ui::WidgetTree;
namespace ui = warploom::ui;

constexpr TextMetrics kMetrics{};  // 8 px chars, 16 px lines

//! Small helper: builds a fresh tree whose root IS the fixture panel (the
//! arena root, slot 0, retyped). All handles come from `tree.add(..., 0)`.
struct TreeFixture {
  WidgetTree tree;
  std::uint32_t root = 0;

  TreeFixture() {
    Widget& r = tree.get(tree.root());
    r.kind = WidgetKind::Panel;
    r.name = "root";
    r.color = 0xFF202020;
  }
};

// ============================================================================
// Arena integrity
// ============================================================================

TEST(WidgetArena, AddIsO1AppendAndOrderPreserved) {
  TreeFixture f;
  Widget a;
  a.name = "a";
  Widget b;
  b.name = "b";
  const auto ha = f.tree.add(std::move(a), f.root);
  const auto hb = f.tree.add(std::move(b), f.root);

  // Handles are arena slots: stable, ordered by insertion.
  EXPECT_NE(ha, hb);
  EXPECT_EQ(f.tree.get(f.root).first_child, ha);
  EXPECT_EQ(f.tree.get(f.root).last_child, hb);
  EXPECT_EQ(f.tree.get(ha).next_sibling, hb);
  EXPECT_EQ(f.tree.get(hb).next_sibling, ui::kInvalidWidget);
}

TEST(WidgetArena, RemoveKeepsIndicesAndUnlinks) {
  TreeFixture f;
  Widget a;
  a.name = "a";
  Widget b;
  b.name = "b";
  Widget c;
  c.name = "c";
  const auto ha = f.tree.add(std::move(a), f.root);
  const auto hb = f.tree.add(std::move(b), f.root);
  const auto hc = f.tree.add(std::move(c), f.root);

  ASSERT_TRUE(f.tree.remove(hb));
  // Arena slots never shift: both remaining handles still valid.
  EXPECT_EQ(f.tree.get(ha).name, "a");
  EXPECT_EQ(f.tree.get(hc).name, "c");
  // Chain is a -> c.
  EXPECT_EQ(f.tree.get(f.root).first_child, ha);
  EXPECT_EQ(f.tree.get(ha).next_sibling, hc);
  EXPECT_EQ(f.tree.get(f.root).last_child, hc);
  // Removed node is fully unlinked.
  EXPECT_EQ(f.tree.get(hb).parent, ui::kInvalidWidget);
  EXPECT_EQ(f.tree.get(hb).next_sibling, ui::kInvalidWidget);
  EXPECT_EQ(f.tree.alive_count(), 3U);  // root + a + c
}

TEST(WidgetArena, RemoveFirstChildAndOnlyChild) {
  TreeFixture f;
  Widget only;
  only.name = "only";
  const auto h = f.tree.add(std::move(only), f.root);

  ASSERT_TRUE(f.tree.remove(h));
  EXPECT_EQ(f.tree.get(f.root).first_child, ui::kInvalidWidget);
  EXPECT_EQ(f.tree.get(f.root).last_child, ui::kInvalidWidget);
  EXPECT_EQ(f.tree.alive_count(), 1U);  // root only

  // Removing again fails (already unlinked), root removal fails.
  EXPECT_FALSE(f.tree.remove(h));
  EXPECT_FALSE(f.tree.remove(f.tree.root()));
  EXPECT_FALSE(f.tree.remove(ui::kInvalidWidget));
}

TEST(WidgetArena, RemovePreservesSubtreeForReparenting) {
  TreeFixture f;
  Widget panel;
  panel.name = "panel";
  const auto hp = f.tree.add(std::move(panel), f.root);
  Widget child;
  child.name = "child";
  const auto hc = f.tree.add(std::move(child), hp);

  ASSERT_TRUE(f.tree.remove(hp));
  // Subtree stays attached to the removed node.
  EXPECT_EQ(f.tree.get(hp).first_child, hc);
  EXPECT_EQ(f.tree.get(hc).parent, hp);
  // Re-parent: link the detached panel under the fixture root again
  // (handles the empty-children case explicitly).
  f.tree.get(hp).parent = f.root;
  Widget& rp = f.tree.get(f.root);
  if (rp.first_child == ui::kInvalidWidget) {
    rp.first_child = hp;
  } else {
    f.tree.get(rp.last_child).next_sibling = hp;
  }
  rp.last_child = hp;
  EXPECT_EQ(f.tree.alive_count(), 3U);  // root + panel + child
}

TEST(WidgetArena, FindByNameReturnsFirstPreorderMatch) {
  TreeFixture f;
  Widget a;
  a.name = "dup";
  Widget b;
  b.name = "dup";
  const auto ha = f.tree.add(std::move(a), f.root);
  const auto hb = f.tree.add(std::move(b), f.root);
  (void)hb;
  EXPECT_EQ(f.tree.find_by_name("dup"), ha);
  EXPECT_EQ(f.tree.find_by_name("missing"), ui::kInvalidWidget);
}

// ============================================================================
// Layout
// ============================================================================

TEST(WidgetLayout, RootFillsViewport) {
  TreeFixture f;
  ui::compute_layout(f.tree, 640.0F, 480.0F, kMetrics);
  const Widget& r = f.tree.get(f.root);
  EXPECT_FLOAT_EQ(r.x, 0.0F);
  EXPECT_FLOAT_EQ(r.y, 0.0F);
  EXPECT_FLOAT_EQ(r.w, 640.0F);
  EXPECT_FLOAT_EQ(r.h, 480.0F);
}

TEST(WidgetLayout, VerticalStackSpacingAndOrder) {
  TreeFixture f;
  Widget a;
  a.kind = WidgetKind::Panel;
  a.fixed_h = 40.0F;
  Widget b;
  b.kind = WidgetKind::Panel;
  b.fixed_h = 60.0F;
  f.tree.get(f.root).spacing = 10.0F;
  const auto ha = f.tree.add(std::move(a), f.root);
  const auto hb = f.tree.add(std::move(b), f.root);

  ui::compute_layout(f.tree, 200.0F, 200.0F, kMetrics);
  const Widget& wa = f.tree.get(ha);
  const Widget& wb = f.tree.get(hb);
  EXPECT_FLOAT_EQ(wa.y, 0.0F);
  EXPECT_FLOAT_EQ(wb.y, 50.0F);  // 40 + 10
  EXPECT_FLOAT_EQ(wa.x, 0.0F);   // cross = start
  EXPECT_FLOAT_EQ(wb.x, 0.0F);
}

TEST(WidgetLayout, FlexGrowsProportionally) {
  TreeFixture f;
  Widget a;
  a.kind = WidgetKind::Panel;
  a.flex_grow = 1.0F;
  Widget b;
  b.kind = WidgetKind::Panel;
  b.flex_grow = 3.0F;
  const auto ha = f.tree.add(std::move(a), f.root);
  const auto hb = f.tree.add(std::move(b), f.root);

  ui::compute_layout(f.tree, 100.0F, 400.0F, kMetrics);
  // CSS-style flex: preferred size (16 px line) is the basis, the leftover
  // 400 - 32 = 368 px is split 1:3. a = 16 + 92 = 108, b = 16 + 276 = 292.
  EXPECT_FLOAT_EQ(f.tree.get(ha).h, 108.0F);
  EXPECT_FLOAT_EQ(f.tree.get(hb).h, 292.0F);
  EXPECT_FLOAT_EQ(f.tree.get(ha).y, 0.0F);
  EXPECT_FLOAT_EQ(f.tree.get(hb).y, 108.0F);
  // Zero-basis widgets fill exactly proportionally.
  TreeFixture g;
  Widget z0;
  z0.kind = WidgetKind::Container;
  z0.flex_grow = 1.0F;
  Widget z1;
  z1.kind = WidgetKind::Container;
  z1.flex_grow = 3.0F;
  const auto hz0 = g.tree.add(std::move(z0), g.root);
  const auto hz1 = g.tree.add(std::move(z1), g.root);
  ui::compute_layout(g.tree, 100.0F, 400.0F, kMetrics);
  EXPECT_FLOAT_EQ(g.tree.get(hz0).h, 100.0F);
  EXPECT_FLOAT_EQ(g.tree.get(hz1).h, 300.0F);
  EXPECT_FLOAT_EQ(g.tree.get(hz1).y, 100.0F);
}

TEST(WidgetLayout, CrossAlignCenterEndStretch) {
  TreeFixture f;
  Widget c;
  c.kind = WidgetKind::Panel;
  c.fixed_w = 40.0F;
  c.cross_align = Align::Center;
  Widget e;
  e.kind = WidgetKind::Panel;
  e.fixed_w = 40.0F;
  e.cross_align = Align::End;
  Widget s;
  s.kind = WidgetKind::Panel;
  s.fixed_w = 40.0F;
  s.cross_align = Align::Stretch;
  const auto hc = f.tree.add(std::move(c), f.root);
  const auto he = f.tree.add(std::move(e), f.root);
  const auto hs = f.tree.add(std::move(s), f.root);

  ui::compute_layout(f.tree, 200.0F, 300.0F, kMetrics);
  EXPECT_FLOAT_EQ(f.tree.get(hc).x, (200.0F - 40.0F) * 0.5F);
  EXPECT_FLOAT_EQ(f.tree.get(he).x, 200.0F - 40.0F);
  // Fixed cross size wins over stretch (CSS semantics).
  EXPECT_FLOAT_EQ(f.tree.get(hs).x, 0.0F);
  EXPECT_FLOAT_EQ(f.tree.get(hs).w, 40.0F);
  // Auto cross size + stretch fills the content box.
  TreeFixture g;
  Widget s2;
  s2.kind = WidgetKind::Panel;
  s2.cross_align = Align::Stretch;
  const auto hs2 = g.tree.add(std::move(s2), g.root);
  ui::compute_layout(g.tree, 200.0F, 300.0F, kMetrics);
  EXPECT_FLOAT_EQ(g.tree.get(hs2).w, 200.0F);
}

TEST(WidgetLayout, HorizontalStackDirection) {
  TreeFixture f;
  Widget a;
  a.kind = WidgetKind::Panel;
  a.fixed_w = 50.0F;
  Widget b;
  b.kind = WidgetKind::Panel;
  b.fixed_w = 70.0F;
  f.tree.get(f.root).direction = StackDirection::Horizontal;
  f.tree.get(f.root).spacing = 5.0F;
  const auto ha = f.tree.add(std::move(a), f.root);
  const auto hb = f.tree.add(std::move(b), f.root);

  ui::compute_layout(f.tree, 300.0F, 100.0F, kMetrics);
  EXPECT_FLOAT_EQ(f.tree.get(ha).x, 0.0F);
  EXPECT_FLOAT_EQ(f.tree.get(hb).x, 55.0F);  // 50 + 5
  // Cross (vertical) align start: both at top, height = preferred line.
  EXPECT_FLOAT_EQ(f.tree.get(ha).y, 0.0F);
}

TEST(WidgetLayout, ContainerWrapAutoSize) {
  TreeFixture f;
  Widget panel;
  panel.kind = WidgetKind::Panel;
  panel.padding = 8.0F;
  const auto hp = f.tree.add(std::move(panel), f.root);
  Widget a;
  a.kind = WidgetKind::Label;
  a.text = "ab";  // 2 * 8 = 16 px wide, 16 px tall
  const auto ha = f.tree.add(std::move(a), hp);
  Widget b;
  b.kind = WidgetKind::Label;
  b.text = "cdef";  // 32 px wide
  f.tree.get(hp).spacing = 4.0F;
  const auto hb = f.tree.add(std::move(b), hp);

  ui::compute_layout(f.tree, 800.0F, 600.0F, kMetrics);
  const Widget& wp = f.tree.get(hp);
  // Vertical: width = max(16, 32) + 16 padding; height = 16+16 + 4 + 32.
  EXPECT_FLOAT_EQ(wp.w, 48.0F);
  EXPECT_FLOAT_EQ(wp.h, 52.0F);
  // Children positioned inside the padding.
  EXPECT_FLOAT_EQ(f.tree.get(ha).x, 8.0F);
  EXPECT_FLOAT_EQ(f.tree.get(ha).y, 8.0F);
  EXPECT_FLOAT_EQ(f.tree.get(hb).y, f.tree.get(ha).y + 16.0F + 4.0F);
  // Labels measured from text.
  EXPECT_FLOAT_EQ(f.tree.get(hb).w, 32.0F);
}

TEST(WidgetLayout, DeterministicAcrossRuns) {
  auto build = [] {
    TreeFixture f;
    Widget top;
    top.kind = WidgetKind::Panel;
    top.flex_grow = 1.0F;
    top.padding = 6.0F;
    const auto ht = f.tree.add(std::move(top), f.root);
    Widget lbl;
    lbl.kind = WidgetKind::Label;
    lbl.text = "determinism";
    (void)f.tree.add(std::move(lbl), ht);
    Widget btn;
    btn.kind = WidgetKind::Button;
    btn.text = "OK";
    btn.fixed_w = 80.0F;
    btn.cross_align = Align::End;
    (void)f.tree.add(std::move(btn), ht);
    return f.tree;
  };
  WidgetTree t1 = build();
  WidgetTree t2 = build();
  ui::compute_layout(t1, 320.0F, 240.0F, kMetrics);
  ui::compute_layout(t2, 320.0F, 240.0F, kMetrics);

  ui::PaintList p1;
  ui::PaintList p2;
  ui::paint(t1, p1, kMetrics);
  ui::paint(t2, p2, kMetrics);
  ASSERT_EQ(p1.rects.size(), p2.rects.size());
  for (std::size_t i = 0; i < p1.rects.size(); ++i) {
    EXPECT_FLOAT_EQ(p1.rects[i].x, p2.rects[i].x);
    EXPECT_FLOAT_EQ(p1.rects[i].y, p2.rects[i].y);
    EXPECT_FLOAT_EQ(p1.rects[i].w, p2.rects[i].w);
    EXPECT_FLOAT_EQ(p1.rects[i].h, p2.rects[i].h);
    EXPECT_EQ(p1.rects[i].color, p2.rects[i].color);
  }
  ASSERT_EQ(p1.texts.size(), p2.texts.size());
}

// ============================================================================
// Paint
// ============================================================================

TEST(WidgetPaint, ParentBeforeChildAndTextAfterRects) {
  TreeFixture f;
  Widget panel;
  panel.kind = WidgetKind::Panel;
  panel.color = 0xFF111111;
  const auto hp = f.tree.add(std::move(panel), f.root);
  Widget btn;
  btn.kind = WidgetKind::Button;
  btn.text = "go";
  btn.color = 0xFF333333;
  btn.text_color = 0xFFEEEEEE;
  (void)f.tree.add(std::move(btn), hp);

  ui::PaintList p;
  ui::paint(f.tree, p, kMetrics);
  // Root panel, nested panel, button rect = 3 rects; 1 text.
  ASSERT_EQ(p.rects.size(), 3U);
  ASSERT_EQ(p.texts.size(), 1U);
  EXPECT_EQ(p.rects[0].color, 0xFF202020U);  // root first
  EXPECT_EQ(p.rects[1].color, 0xFF111111U);
  EXPECT_EQ(p.rects[2].color, 0xFF333333U);
  EXPECT_EQ(p.texts[0].text, "go");
  EXPECT_EQ(p.texts[0].color, 0xFFEEEEEEU);
}

TEST(WidgetPaint, InvisibleSubtreeCulled) {
  TreeFixture f;
  Widget hidden;
  hidden.kind = WidgetKind::Panel;
  hidden.visible = false;
  const auto hh = f.tree.add(std::move(hidden), f.root);
  Widget child;
  child.kind = WidgetKind::Label;
  child.text = "never";
  (void)f.tree.add(std::move(child), hh);

  ui::PaintList p;
  ui::paint(f.tree, p, kMetrics);
  EXPECT_EQ(p.rects.size(), 1U);  // root panel only
  EXPECT_TRUE(p.texts.empty());
}

TEST(WidgetPaint, CheckboxEmitsToggleRect) {
  TreeFixture f;
  Widget cb;
  cb.kind = WidgetKind::Checkbox;
  cb.text = "snap";
  cb.checked = true;
  cb.color = 0xFF00FF00;
  cb.padding = 4.0F;
  (void)f.tree.add(std::move(cb), f.root);

  ui::PaintList p;
  ui::paint(f.tree, p, kMetrics);
  ASSERT_EQ(p.rects.size(), 3U);  // root panel + row + toggle square
  const auto& square = p.rects[2];
  EXPECT_EQ(square.color, 0xFF00FF00U);  // checked -> widget color
  EXPECT_FLOAT_EQ(square.w, 0.6F * kMetrics.line_height);
  EXPECT_FLOAT_EQ(square.x, 4.0F);  // padding
  // Label after the square.
  EXPECT_FLOAT_EQ(p.texts[0].x, 4.0F + 0.6F * kMetrics.line_height + 4.0F);
}
}  // namespace
