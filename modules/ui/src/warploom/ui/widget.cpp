//! @file widget.cpp
//! @brief Widget arena, layout engine, and paint-list emission (see header).
//!
//! Layout algorithm (deterministic, no reflow, yoga-style single pass):
//!   1. Measure sweep (bottom-up): every widget records its *preferred*
//!      size in w/h — fixed size if set, text measurement for leaves,
//!      wrap-of-children (padding + children + spacing) for containers.
//!   2. Arrange sweep (top-down): each parent assigns final rects inside
//!      its content box — main-axis size = fixed > flex share of leftover
//!      > preferred; cross-axis = fixed > stretch (content box) > preferred
//!      with Center/End shifting from the start edge. Children are placed
//!      along the direction axis with `spacing`, then recursed into.
//!
//! Preferred sizes live in w/h only between the sweeps; after arrange,
//! w/h are final. The same tree always yields the same rects.

#include "warploom/ui/widget.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace warploom::ui {

namespace {

// ============================================================================
// Measure sweep
// ============================================================================

//! Preferred main/cross size of a leaf from its content. Padding is part of
//! every leaf's preferred size (paint places content inside it). A childless
//! Container has zero intrinsic size (CSS empty-div semantics); text leaves
//! measure from their glyphs.
void measure_leaf(Widget& w, const TextMetrics& metrics) {
  const float side = 0.6F * metrics.line_height;  // checkbox toggle square
  float pref_w = 0.0F;
  if (w.kind == WidgetKind::Container) {
    pref_w = 0.0F;
  } else {
    pref_w = static_cast<float>(w.text.size()) * metrics.char_width;
  }
  if (w.kind == WidgetKind::Checkbox) {
    pref_w += side;
  }
  pref_w += 2.0F * w.padding;
  float pref_h = 2.0F * w.padding;
  if (w.kind != WidgetKind::Container) {
    pref_h += metrics.line_height;
  }
  // fixed_* is a non-negative dimension: 0 means auto.
  w.w = (w.fixed_w > 0.0F) ? w.fixed_w : pref_w;
  w.h = (w.fixed_h > 0.0F) ? w.fixed_h : pref_h;
}

//! Bottom-up: childless widgets measure from content; widgets WITH children
//! (any kind — a Panel with children is a layout parent) wrap them.
void measure(WidgetTree& tree, std::uint32_t handle,
             const TextMetrics& metrics) {
  Widget& w = tree.get(handle);
  if (w.first_child != kInvalidWidget) {
    for (std::uint32_t c = w.first_child; c != kInvalidWidget;
         c = tree.get(c).next_sibling) {
      if (tree.get(c).visible) {
        measure(tree, c, metrics);
      }
    }
    // Wrap: padding + children extent + spacing (either axis). Invisible
    // children are display:none: they consume no space and no spacing slot.
    float main_extent = 0.0F;
    float cross_extent = 0.0F;
    std::size_t n = 0;
    const bool vertical = (w.direction == StackDirection::Vertical);
    for (std::uint32_t c = w.first_child; c != kInvalidWidget;
         c = tree.get(c).next_sibling) {
      const Widget& child = tree.get(c);
      if (!child.visible) {
        continue;
      }
      main_extent += vertical ? child.h : child.w;
      cross_extent = std::max(cross_extent, vertical ? child.w : child.h);
      ++n;
    }
    if (n > 1) {
      main_extent += static_cast<float>(n - 1) * w.spacing;
    }
    const float pref_w = vertical ? cross_extent : main_extent;
    const float pref_h = vertical ? main_extent : cross_extent;
    w.w = (w.fixed_w > 0.0F)
              ? w.fixed_w
              : pref_w + 2.0F * w.padding;
    w.h = (w.fixed_h > 0.0F)
              ? w.fixed_h
              : pref_h + 2.0F * w.padding;
  } else {
    measure_leaf(w, metrics);
  }
}

// ============================================================================
// Arrange sweep
// ============================================================================

//! Assigns final rects to `handle`'s children inside its content box, then
//! recurses. `handle`'s own rect must already be final.
void arrange(WidgetTree& tree, std::uint32_t handle,
             const TextMetrics& metrics) {
  Widget& parent = tree.get(handle);
  if (parent.layout == LayoutMode::Absolute) {
    // Free canvas: children keep their authored rects; recurse only so
    // nested stack containers still lay out their own children.
    for (std::uint32_t c = parent.first_child; c != kInvalidWidget;
         c = tree.get(c).next_sibling) {
      if (tree.get(c).visible) {
        arrange(tree, c, metrics);
      }
    }
    return;
  }
  if (parent.layout != LayoutMode::Stack) {
    return;  // Flow is reserved; nothing to arrange.
  }
  const bool vertical = (parent.direction == StackDirection::Vertical);
  const float content_w = std::max(0.0F, parent.w - 2.0F * parent.padding);
  const float content_h = std::max(0.0F, parent.h - 2.0F * parent.padding);

  // Collect VISIBLE children (insertion order); invisible ones are
  // display:none — no space, no flex share, no spacing slot.
  std::vector<std::uint32_t> kids;
  for (std::uint32_t c = parent.first_child; c != kInvalidWidget;
       c = tree.get(c).next_sibling) {
    if (tree.get(c).visible) {
      kids.push_back(c);
    }
  }
  if (kids.empty()) {
    return;
  }

  float used = static_cast<float>(kids.size() - 1) * parent.spacing;
  for (auto c : kids) {
    const Widget& child = tree.get(c);
    used += vertical ? child.h : child.w;
  }
  const float content_main = vertical ? content_h : content_w;
  const float available = std::max(0.0F, content_main - used);

  float grow_total = 0.0F;
  for (auto c : kids) {
    grow_total += tree.get(c).flex_grow;
  }

  // Final size + position per child.
  const float content_cross = vertical ? content_w : content_h;
  float cursor = parent.padding;
  for (auto c : kids) {
    Widget& child = tree.get(c);

    // Fixed size wins on its own axis; flex applies only to an auto main
    // axis; stretch applies only to an auto cross axis.
    // Dimensions are non-negative, so 0 means "auto" and `> 0.0F` is the
    // same test as `!= 0.0F` without a float equality.
    const bool main_fixed =
        vertical ? (child.fixed_h > 0.0F) : (child.fixed_w > 0.0F);
    const bool cross_fixed =
        vertical ? (child.fixed_w > 0.0F) : (child.fixed_h > 0.0F);

    // Main axis: fixed > flex share > preferred (already in w/h).
    float main = vertical ? child.h : child.w;
    if (!main_fixed && grow_total > 0.0F && child.flex_grow > 0.0F) {
      main += available * (child.flex_grow / grow_total);
    }

    // Cross axis: fixed/stretch/aligned around preferred.
    float cross = vertical ? child.w : child.h;
    float cross_offset = 0.0F;
    switch (child.cross_align) {
      case Align::Stretch:
        if (!cross_fixed) {
          cross = content_cross;
        }
        break;
      case Align::Center:
        cross_offset = (content_cross - cross) * 0.5F;
        break;
      case Align::End:
        cross_offset = content_cross - cross;
        break;
      case Align::Start:
        break;
    }

    if (vertical) {
      child.h = main;
      child.w = cross;
      child.x = parent.x + parent.padding + cross_offset;
      child.y = parent.y + cursor;
      cursor += main + parent.spacing;
    } else {
      child.w = main;
      child.h = cross;
      child.x = parent.x + cursor;
      child.y = parent.y + parent.padding + cross_offset;
      cursor += main + parent.spacing;
    }
  }

  // Recurse with final rects.
  for (auto c : kids) {
    arrange(tree, c, metrics);
  }
}

}  // namespace

// ============================================================================
// WidgetTree
// ============================================================================

WidgetTree::WidgetTree() {
  widgets_.emplace_back();  // slot 0 = root (plain container)
}

std::uint32_t WidgetTree::add(Widget widget, std::uint32_t parent) {
  const auto handle = static_cast<std::uint32_t>(widgets_.size());
  widget.parent = parent;
  widget.first_child = kInvalidWidget;
  widget.next_sibling = kInvalidWidget;
  widget.last_child = kInvalidWidget;
  widgets_.push_back(widget);

  Widget& p = widgets_[parent];
  if (p.first_child == kInvalidWidget) {
    p.first_child = handle;
  } else {
    widgets_[p.last_child].next_sibling = handle;
  }
  p.last_child = handle;
  return handle;
}

bool WidgetTree::remove(std::uint32_t handle) {
  if (handle == kInvalidWidget || handle >= widgets_.size() ||
      handle == root()) {
    return false;
  }
  Widget& w = widgets_[handle];
  const auto parent = w.parent;
  if (parent >= widgets_.size()) {
    return false;  // Already unlinked orphan.
  }
  Widget& p = widgets_[parent];

  if (p.first_child == handle) {
    p.first_child = w.next_sibling;
  } else {
    // Splice out of the middle/end: link the previous sibling past `handle`.
    for (std::uint32_t c = p.first_child; c != kInvalidWidget;
         c = widgets_[c].next_sibling) {
      if (widgets_[c].next_sibling == handle) {
        widgets_[c].next_sibling = w.next_sibling;
        break;
      }
    }
  }
  if (p.last_child == handle) {
    // New last = the previous sibling (none when handle was also first).
    std::uint32_t prev = kInvalidWidget;
    for (std::uint32_t c = p.first_child;
         c != kInvalidWidget && c != handle; c = widgets_[c].next_sibling) {
      prev = c;
    }
    p.last_child = prev;
  }

  // Unlink `handle` itself; its subtree stays attached (first_child/
  // last_child preserved) so it can be re-parented later.
  w.parent = kInvalidWidget;
  w.next_sibling = kInvalidWidget;
  return true;
}

std::size_t WidgetTree::alive_count() const {
  std::size_t n = 0;
  for_each([&n](std::uint32_t, const Widget&) { ++n; });
  return n;
}

std::uint32_t WidgetTree::find_by_name(std::string_view name) const {
  std::uint32_t found = kInvalidWidget;
  for_each([&](std::uint32_t h, const Widget& w) {
    if (found == kInvalidWidget && w.name == name) {
      found = h;
    }
  });
  return found;
}

// ============================================================================
// Layout entry point
// ============================================================================

void compute_layout(WidgetTree& tree, float width, float height,
                    const TextMetrics& metrics) {
  Widget& root = tree.get(tree.root());
  root.x = 0.0F;
  root.y = 0.0F;
  root.w = std::max(0.0F, width);
  root.h = std::max(0.0F, height);
  measure(tree, tree.root(), metrics);
  // The root's size is the viewport (fixed), not the measured wrap size.
  root.w = std::max(0.0F, width);
  root.h = std::max(0.0F, height);
  arrange(tree, tree.root(), metrics);
}

// ============================================================================
// Paint
// ============================================================================

void paint(const WidgetTree& tree, PaintList& out,
           const TextMetrics& metrics) {
  out.clear();
  // Explicit-stack pre-order: parent before children, children in insertion
  // order. An invisible widget culls its whole subtree (children of a hidden
  // widget are never pushed).
  std::vector<std::uint32_t> stack{tree.root()};
  while (!stack.empty()) {
    const std::uint32_t h = stack.back();
    stack.pop_back();
    const Widget& w = tree.get(h);
    if (!w.visible) {
      continue;
    }
    const auto emit_rect = [&](std::uint32_t color) {
      out.rects.push_back(PaintRect{w.x, w.y, w.w, w.h, color,
                                    w.border_color, w.border_width});
    };
    switch (w.kind) {
      case WidgetKind::Panel:
      case WidgetKind::Button:
        emit_rect(w.color);
        break;
      case WidgetKind::Checkbox: {
        emit_rect(w.color);
        // Toggle square at the start edge, vertically centered.
        const float side = 0.6F * metrics.line_height;
        const float cy = w.y + (w.h - side) * 0.5F;
        out.rects.push_back(PaintRect{w.x + w.padding, cy, side, side,
                                      w.checked ? w.color : 0xFF202020U,
                                      w.border_color, w.border_width});
        break;
      }
      case WidgetKind::Container:
      case WidgetKind::Label:
        break;
    }
    // Text: vertical centering in the box; checkbox label after the toggle
    // square. Emitted for any kind that carries text.
    if (!w.text.empty()) {
      float tx = w.x + w.padding;
      if (w.kind == WidgetKind::Checkbox) {
        tx = w.x + w.padding + 0.6F * metrics.line_height + w.padding;
      }
      const float ty = w.y + (w.h - metrics.line_height) * 0.5F;
      out.texts.push_back(PaintText{tx, ty, w.text, w.text_color});
    }
    // Push children in reverse so they pop in insertion order.
    std::vector<std::uint32_t> kids;
    for (std::uint32_t c = w.first_child; c != kInvalidWidget;
         c = tree.get(c).next_sibling) {
      kids.push_back(c);
    }
    for (std::size_t i = kids.size(); i-- > 0;) {
      stack.push_back(kids[i]);
    }
  }
}

}  // namespace warploom::ui
