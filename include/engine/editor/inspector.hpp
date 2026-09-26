#pragma once

//! @file inspector.hpp
//! @brief M11 inspector/outliner: document projection to the M2 UI tree.
//!
//! Design matches the node editor: the panel is a pure function of
//! (document, registry, selection). `rebuild()` discards and re-derives the
//! widget subtree; `hit_test()` resolves clicks against layout-authoritative
//! rects (no duplicated geometry). Rows are named widgets
//! (`obj_row_<id>`, `prop_row_<oid>_<prop>`, `bind_row_<i>`) so tests and
//! hosts can locate them without fragile handles.
//!
//! Sections:
//!   OUTLINER    — one row per document object (click -> select)
//!   PROPERTIES  — the selected object's properties (read-only this mile;
//!                 M12 turns rows into editors)
//!   BINDINGS    — graph->scene bindings, rendered as chips the node editor
//!                 can wire to (the binding-wire anchor is the chip rect)

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/document.hpp"
#include "engine/core/editor_session.hpp"
#include "engine/core/property_registry.hpp"
#include "warploom/ui/widget.hpp"

namespace omnicpp::editor {
namespace ui = ::warploom::ui;  // S1: ui module moved to the warploom namespace

//! What a click on the inspector resolved to.
struct InspectorHit final {
  enum class Kind : std::uint8_t {
    None,
    SelectObject,  //!< numbers[0] = object id -> `select` command
    Property,      //!< property row (M12: editing); carries oid + property
    Binding,       //!< binding chip row; carries the binding index
  };
  Kind kind{Kind::None};
  std::uint64_t object_id{0};
  std::string property{};
  std::size_t index{0};  //!< binding index for Kind::Binding
};

//! One binding->chip anchor the node editor wires can target.
struct BindingAnchor final {
  std::uint64_t node_id{0};
  std::string pin{};
  float x{0.0F};
  float y{0.0F};
};

class InspectorPanel final {
 public:
  static constexpr float kPanelW = 240.0F;

  //! Re-derives the widget subtree under `parent` from document state.
  //! Idempotent: the previous subtree is removed first. The panel widget
  //! itself is created here (absolute layout, fixed width) and its handle
  //! remembered for hit-testing and binding anchors.
  void rebuild(ui::WidgetTree& tree, std::uint32_t parent,
               const SceneDocument& doc, const PropertyRegistry& registry,
               std::uint64_t selected_id,
               const std::vector<EditorSession::PropertyBinding>& bindings);

  //! Resolves a click at (x, y) against layout-authoritative rects.
  //! Topmost containing widget wins (last in paint order). None when the
  //! click falls outside the panel or on chrome.
  [[nodiscard]] InspectorHit hit_test(const ui::WidgetTree& tree, float x,
                                      float y) const;

  //! Screen-space left-edge center of the chip for binding `index`
  //! (the anchor a binding wire connects to). False when unknown.
  [[nodiscard]] bool binding_anchor(const ui::WidgetTree& tree,
                                    std::size_t index, float& x,
                                    float& y) const;

  //! Collects anchors for every current binding (missing chips skipped).
  [[nodiscard]] std::vector<BindingAnchor> binding_anchors(
      const ui::WidgetTree& tree,
      const std::vector<EditorSession::PropertyBinding>& bindings) const;

  [[nodiscard]] std::uint32_t panel_handle() const noexcept {
    return panel_;
  }

 private:
  std::uint32_t panel_{ui::kInvalidWidget};
  std::uint32_t parent_{ui::kInvalidWidget};
  std::size_t binding_count_{0};
};

}  // namespace omnicpp::editor
