//! @file inspector.cpp
//! @brief M11 inspector/outliner bodies (see the header).
//!
//! Layout: a fixed-width absolute panel on the right screen edge. The
//! widget tree does the text/stack layout inside; only the outer panel
//! position is authored here. All click resolution reads back computed
//! rects, so the layout pass is authoritative and tests assert on the
//! same geometry the renderer draws.

#include "engine/editor/inspector.hpp"
#include "engine/editor/node_editor.hpp"  // value_to_text (PropValue formatting)

#include <cstdio>
#include <string>

namespace omnicpp::editor {

namespace {

using omnicpp::ui::Widget;
using omnicpp::ui::WidgetKind;

Widget make_label(std::string name, std::string text, std::uint32_t color) {
  Widget w;
  w.kind = WidgetKind::Label;
  w.name = std::move(name);
  w.text = std::move(text);
  w.color = color;
  return w;
}

}  // namespace

void InspectorPanel::rebuild(
    ui::WidgetTree& tree, std::uint32_t parent, const SceneDocument& doc,
    const PropertyRegistry& registry, std::uint64_t selected_id,
    const std::vector<EditorSession::PropertyBinding>& bindings) {
  parent_ = parent;
  // Idempotent: drop the previous subtree.
  if (panel_ != ui::kInvalidWidget && panel_ < tree.size() &&
      tree.get(panel_).parent == parent) {
    tree.remove(panel_);
  }
  panel_ = ui::kInvalidWidget;

  Widget panel;
  panel.kind = WidgetKind::Panel;
  panel.name = "inspector_panel";
  panel.color = 0xCC101418;  // translucent dark
  panel.border_color = 0xFF3A4652;
  panel.layout = ui::LayoutMode::Stack;
  panel.direction = ui::StackDirection::Vertical;
  panel.padding = 6.0F;
  panel.spacing = 2.0F;
  panel.fixed_w = kPanelW;
  // Absolute placement on the right edge, below the toolbar.
  panel.layout = ui::LayoutMode::Absolute;
  panel_ = tree.add(panel, parent);
  auto& panel_ref = tree.get(panel_);
  panel_ref.layout = ui::LayoutMode::Stack;  // children stack inside
  panel_ref.x = 8.0F;
  // y is set by the host (below its toolbar); default top.
  panel_ref.y = 40.0F;

  // ---------------- OUTLINER ----------------
  tree.add(make_label("outliner_title", "OUTLINER", 0xFF9FC4E0), panel_);
  for (const auto& obj : doc.objects) {
    const auto* type = registry.find(obj.type_id);
    const bool selected = obj.id == selected_id;
    Widget row;
    row.kind = WidgetKind::Panel;
    row.name = "obj_row_" + std::to_string(obj.id);
    row.text = obj.name;
    row.color = selected ? 0xFF2E6E4E : 0xFF1A2026;
    row.border_color = selected ? 0xFF66D9A0 : 0x00000000;
    row.fixed_h = 16.0F;
    row.text_color = selected ? 0xFFEFFFFFFF : 0xFFC8D2DA;
    tree.add(row, panel_);
  }

  // ---------------- PROPERTIES (selected object) ----------------
  tree.add(make_label("props_title", "PROPERTIES", 0xFF9FC4E0), panel_);
  const SceneObject* sel = doc.find(selected_id);
  if (sel != nullptr) {
    const auto* type = registry.find(sel->type_id);
    if (type != nullptr) {
      for (const auto& desc : type->properties) {
        const auto it = sel->properties.find(desc.name);
        std::string value_text = "<unset>";
        if (it != sel->properties.end()) {
          value_text = value_to_text(it->second);
        }
        Widget row;
        row.kind = WidgetKind::Panel;
        row.name = "prop_row_" + std::to_string(sel->id) + "_" + desc.name;
        row.text = desc.name + " = " + value_text;
        row.color = 0xFF141A20;
        row.fixed_h = 16.0F;
        row.text_color = 0xFFD8E0E8;
        tree.add(row, panel_);
      }
    }
  } else {
    tree.add(make_label("props_none", "(no selection)", 0xFF66707A), panel_);
  }

  // ---------------- BINDINGS ----------------
  tree.add(make_label("binds_title", "BINDINGS", 0xFFE0B060), panel_);
  binding_count_ = bindings.size();
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    const auto& b = bindings[i];
    Widget chip;
    chip.kind = WidgetKind::Panel;
    chip.name = "bind_row_" + std::to_string(i);
    chip.text = "n" + std::to_string(b.node_id) + "." + b.out_pin + " -> o" +
                std::to_string(b.object_id) + "." + b.property;
    chip.color = 0xFF3A3020;
    chip.border_color = 0xFFE0B060;
    chip.fixed_h = 16.0F;
    chip.text_color = 0xFFF0E0C0;
    tree.add(chip, panel_);
  }
}

InspectorHit InspectorPanel::hit_test(const ui::WidgetTree& tree, float x,
                                      float y) const {
  InspectorHit hit;
  if (panel_ == ui::kInvalidWidget || panel_ >= tree.size()) {
    return hit;
  }
  const auto& panel = tree.get(panel_);
  if (x < panel.x || y < panel.y || x >= panel.x + panel.w ||
      y >= panel.y + panel.h) {
    return hit;
  }
  // Precise containment: iterate the panel subtree in reverse paint order
  // (pre-order = paint order, so reverse pre-order = topmost first).
  std::uint32_t best = ui::kInvalidWidget;
  std::vector<std::uint32_t> order;
  tree.for_each([&](std::uint32_t handle, const ui::Widget&) {
    order.push_back(handle);
  });
  // Pre-order indices of panel descendants, reversed.
  auto is_descendant = [&](std::uint32_t h) {
    for (std::uint32_t p = h; p != ui::kInvalidWidget && p < tree.size();
         p = tree.get(p).parent) {
      if (p == panel_) return true;
    }
    return false;
  };
  best = ui::kInvalidWidget;
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    if (!is_descendant(*it) || *it == panel_) continue;
    const auto& w = tree.get(*it);
    if (x >= w.x && y >= w.y && x < w.x + w.w && y < w.y + w.h) {
      best = *it;
      break;
    }
  }
  if (best == ui::kInvalidWidget) {
    return hit;
  }
  const auto& w = tree.get(best);
  if (w.name.rfind("obj_row_", 0) == 0) {
    hit.kind = InspectorHit::Kind::SelectObject;
    hit.object_id = std::stoull(w.name.substr(8));
    return hit;
  }
  if (w.name.rfind("prop_row_", 0) == 0) {
    hit.kind = InspectorHit::Kind::Property;
    const std::string rest = w.name.substr(9);
    const auto underscore = rest.find('_');
    hit.object_id = std::stoull(rest.substr(0, underscore));
    hit.property = rest.substr(underscore + 1);
    return hit;
  }
  if (w.name.rfind("bind_row_", 0) == 0) {
    hit.kind = InspectorHit::Kind::Binding;
    hit.index = static_cast<std::size_t>(std::stoull(w.name.substr(9)));
    return hit;
  }
  return hit;
}

bool InspectorPanel::binding_anchor(const ui::WidgetTree& tree,
                                    std::size_t index, float& x,
                                    float& y) const {
  const std::string name = "bind_row_" + std::to_string(index);
  const std::uint32_t handle = tree.find_by_name(name);
  if (handle == ui::kInvalidWidget) {
    return false;
  }
  const auto& w = tree.get(handle);
  x = w.x;
  y = w.y + w.h * 0.5F;
  return true;
}

std::vector<BindingAnchor> InspectorPanel::binding_anchors(
    const ui::WidgetTree& tree,
    const std::vector<EditorSession::PropertyBinding>& bindings) const {
  std::vector<BindingAnchor> out;
  out.reserve(bindings.size());
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    BindingAnchor a;
    a.node_id = bindings[i].node_id;
    a.pin = bindings[i].out_pin;
    if (binding_anchor(tree, i, a.x, a.y)) {
      out.push_back(a);
    }
  }
  return out;
}

}  // namespace omnicpp::editor
