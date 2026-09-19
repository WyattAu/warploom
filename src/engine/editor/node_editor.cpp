//! @file node_editor.cpp
//! @brief Node-editor view bodies (see the header).
//!
//! Card anatomy (fixed for v1): a Panel of kCardW x kCardH with the node
//! type as its label; input pins render as small squares down the left
//! edge, output pins down the right edge. Wires are straight horizontal
//! bands between pin rows. Everything derives from graph state: same
//! graph + same positions => same pixels.
//!
//! Canvas placement: cards are children of the canvas widget but keep
//! rebuild-time absolute rects — `pin_canvas` switches the canvas to
//! absolute mode so layout never repositions them.

#include "engine/editor/node_editor.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "engine/core/contract.hpp"

namespace omnicpp::editor {

namespace {

constexpr float kPinSize = 8.0F;
constexpr std::uint32_t kCardFill = 0xFF2A2A2EU;
constexpr std::uint32_t kCardBorder = 0xFF55555CU;
constexpr std::uint32_t kCardSelected = 0xFFFFC24BU;
constexpr std::uint32_t kPinInColor = 0xFF5B8DEFU;
constexpr std::uint32_t kPinOutColor = 0xFF42B883U;
constexpr std::uint32_t kWireColor = 0xFF9AA0A6U;

}  // namespace

void pin_canvas(ui::WidgetTree& tree, std::uint32_t canvas) {
  OMNICPP_CONTRACT(canvas != ui::kInvalidWidget);
  ui::Widget& w = tree.get(canvas);
  w.layout = ui::LayoutMode::Absolute;
  w.direction = ui::StackDirection::Vertical;  // unused in absolute mode
  w.padding = 0.0F;
  w.spacing = 0.0F;
}

void NodeEditorView::rebuild(ui::WidgetTree& tree,
                             std::uint32_t canvas_parent) {
  OMNICPP_CONTRACT(canvas_parent != ui::kInvalidWidget);
  canvas_parent_ = canvas_parent;
  tree_ref_ = &tree;
  pin_canvas(tree, canvas_parent);

  // Idempotent rebuild: detach the previous card subtree first.
  if (card_root_ != ui::kInvalidWidget &&
      card_root_ < tree.size() &&
      tree.get(card_root_).parent == canvas_parent) {
    tree.remove(card_root_);
  }
  card_root_ = ui::kInvalidWidget;
  views_.clear();

  ui::Widget cards_root;
  cards_root.kind = ui::WidgetKind::Container;
  cards_root.name = "node_editor_cards";
  cards_root.layout = ui::LayoutMode::Absolute;  // cards keep authored rects
  card_root_ = tree.add(cards_root, canvas_parent);

  // Deterministic card order: ascending node id (nodes_ is id-ordered).
  for (const auto& node : graph_->nodes()) {
    const auto* type = graph_->find_type(node.type);
    const int in_count =
        type != nullptr ? static_cast<int>(type->inputs.size()) : 0;
    const int out_count =
        type != nullptr ? static_cast<int>(type->outputs.size()) : 0;

    // Default grid position; session-layer drags override via set_position.
    NodeView view;
    view.node_id = node.id;
    view.x = 40.0F + static_cast<float>(node.id % 5U) * 170.0F;
    view.y = 40.0F + static_cast<float>((node.id / 5U) % 4U) * 100.0F;
    view.selected = false;

    ui::Widget card;
    card.kind = ui::WidgetKind::Panel;
    card.name = "node_" + std::to_string(node.id);
    card.text = node.type;
    card.color = kCardFill;
    card.border_color = kCardBorder;
    card.border_width = 1.0F;
    card.fixed_w = kCardW;
    card.fixed_h = kCardH;
    card.padding = 6.0F;
    card.text_color = 0xFFE8E8E8U;

    const auto card_handle = tree.add(card, card_root_);
    tree.get(card_handle).x = view.x;
    tree.get(card_handle).y = view.y;
    tree.get(card_handle).w = kCardW;
    tree.get(card_handle).h = kCardH;

    views_.push_back(view);
  }
}

void NodeEditorView::sync_widgets() {
  if (card_root_ == ui::kInvalidWidget) {
    return;
  }
  // Views are in the same ascending-id order the cards were added; the
  // card for views_[i] is the (i+1)-th child of card_root_.
  std::size_t i = 0;
  for (std::uint32_t c = tree_ref_->get(card_root_).first_child;
       c != ui::kInvalidWidget && i < views_.size();
       c = tree_ref_->get(c).next_sibling, ++i) {
    ui::Widget& card = tree_ref_->get(c);
    card.x = views_[i].x;
    card.y = views_[i].y;
    card.border_color =
        views_[i].selected ? kCardSelected : kCardBorder;
    card.border_width = views_[i].selected ? 2.0F : 1.0F;
  }
}

void NodeEditorView::append_pins(ui::PaintList& list) const {
  // Re-derive pin positions from the CURRENT views (drag moves them).
  for (const auto& node : graph_->nodes()) {
    const NodeView* v = find_view(node.id);
    if (v == nullptr) {
      continue;
    }
    const auto* type = graph_->find_type(node.type);
    if (type == nullptr) {
      continue;
    }
    for (std::size_t i = 0; i < type->inputs.size(); ++i) {
      list.rects.push_back(ui::PaintRect{
          v->x - kPinSize * 0.5F,
          v->y + pin_offset(static_cast<int>(i)) - kPinSize * 0.5F, kPinSize,
          kPinSize, kPinInColor, 0x00000000U, 1.0F});
    }
    for (std::size_t i = 0; i < type->outputs.size(); ++i) {
      list.rects.push_back(ui::PaintRect{
          v->x + kCardW - kPinSize * 0.5F,
          v->y + pin_offset(static_cast<int>(i)) - kPinSize * 0.5F, kPinSize,
          kPinSize, kPinOutColor, 0x00000000U, 1.0F});
    }
  }
}

void NodeEditorView::append_wires(ui::PaintList& list) const {
  wires_.clear();
  for (const auto& link : graph_->links()) {
    const NodeView* from = find_view(link.from_node);
    const NodeView* to = find_view(link.to_node);
    if (from == nullptr || to == nullptr) {
      continue;
    }
    const auto* from_node = graph_->find(link.from_node);
    const auto* to_node = graph_->find(link.to_node);
    if (from_node == nullptr || to_node == nullptr) {
      continue;
    }
    int out_i = 0;
    int in_i = 0;
    if (const auto* t = graph_->find_type(from_node->type)) {
      for (std::size_t i = 0; i < t->outputs.size(); ++i) {
        if (t->outputs[i].name == link.from_pin) {
          out_i = static_cast<int>(i);
        }
      }
    }
    if (const auto* t = graph_->find_type(to_node->type)) {
      for (std::size_t i = 0; i < t->inputs.size(); ++i) {
        if (t->inputs[i].name == link.to_pin) {
          in_i = static_cast<int>(i);
        }
      }
    }
    const float x0 = from->x + kCardW;
    const float y0 = from->y + pin_offset(out_i);
    const float x1 = to->x;
    const float y1 = to->y + pin_offset(in_i);
    const float left = std::min(x0, x1);
    const float top = std::min(y0, y1);
    const float w = std::max(1.0F, std::abs(x1 - x0));
    const float h = std::max(1.0F, std::abs(y1 - y0));
    wires_.push_back(
        ui::PaintRect{left, top, w, h, kWireColor, 0x00000000U, 1.0F});
  }
  // Wires UNDER the cards: prepend; pins OVER everything: append.
  list.rects.insert(list.rects.begin(), wires_.begin(), wires_.end());
  append_pins(list);
}

NodeView* NodeEditorView::find_view(std::uint64_t node_id) {
  for (auto& v : views_) {
    if (v.node_id == node_id) {
      return &v;
    }
  }
  return nullptr;
}

const NodeView* NodeEditorView::find_view(std::uint64_t node_id) const {
  for (const auto& v : views_) {
    if (v.node_id == node_id) {
      return &v;
    }
  }
  return nullptr;
}

bool NodeEditorView::select(std::uint64_t node_id) {
  if (node_id == 0U) {
    for (auto& v : views_) {
      v.selected = false;
    }
    return true;
  }
  NodeView* v = find_view(node_id);
  if (v == nullptr) {
    return false;
  }
  for (auto& other : views_) {
    other.selected = false;
  }
  v->selected = true;
  return true;
}

bool NodeEditorView::translate(std::uint64_t node_id, float dx, float dy) {
  NodeView* v = find_view(node_id);
  if (v == nullptr) {
    return false;
  }
  v->x += dx;
  v->y += dy;
  return true;
}

void NodeEditorView::set_position(std::uint64_t node_id, float x, float y) {
  if (NodeView* v = find_view(node_id); v != nullptr) {
    v->x = x;
    v->y = y;
  }
}

bool NodeEditorView::node_rect(std::uint64_t node_id, float& x, float& y,
                               float& w, float& h) const {
  const NodeView* v = find_view(node_id);
  if (v == nullptr) {
    return false;
  }
  x = v->x;
  y = v->y;
  w = kCardW;
  h = kCardH;
  return true;
}

std::uint64_t NodeEditorView::hit_test(float x, float y) const {
  // Topmost = last in views_ order (highest id wins; deterministic).
  for (std::size_t i = views_.size(); i-- > 0;) {
    const auto& v = views_[i];
    if (x >= v.x && x < v.x + kCardW && y >= v.y && y < v.y + kCardH) {
      return v.node_id;
    }
  }
  return 0U;
}

std::vector<std::uint32_t> build_node_toolbar(ui::WidgetTree& tree,
                                              std::uint32_t toolbar_parent,
                                              const NodeGraph& graph) {
  std::vector<std::uint32_t> buttons;
  for (const auto& type : graph.types()) {
    ui::Widget btn;
    btn.kind = ui::WidgetKind::Button;
    btn.name = "node_add_" + type.name;
    btn.text = "+ " + type.name;
    btn.fixed_w = 90.0F;
    btn.fixed_h = 24.0F;
    buttons.push_back(tree.add(btn, toolbar_parent));
  }
  ui::Widget undo;
  undo.kind = ui::WidgetKind::Button;
  undo.name = "node_toolbar_undo";
  undo.text = "undo";
  undo.fixed_w = 52.0F;
  undo.fixed_h = 24.0F;
  buttons.push_back(tree.add(undo, toolbar_parent));

  ui::Widget redo;
  redo.kind = ui::WidgetKind::Button;
  redo.name = "node_toolbar_redo";
  redo.text = "redo";
  redo.fixed_w = 52.0F;
  redo.fixed_h = 24.0F;
  buttons.push_back(tree.add(redo, toolbar_parent));
  return buttons;
}

}  // namespace omnicpp::editor
