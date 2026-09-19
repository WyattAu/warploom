#pragma once

//! @file node_editor.hpp
//! @brief M6 node-graph editor view: pure projection from the M5 graph
//!        document to the M2 UI widget tree, plus wire routing.
//!
//! Design: the editor view is a *function of the document*. `rebuild()`
//! discards and re-derives the whole widget subtree from `NodeGraph`
//! state — no incremental widget patching, no hidden view state beyond
//! per-node positions. The deterministic layout/paint contract then
//! applies unchanged: the same graph + positions always produce the same
//! widget tree, paint list, and rasterized pixels.
//!
//! Canvas placement: the canvas widget is switched to
//! `LayoutMode::Absolute` by `pin_canvas()`, so cards keep their authored
//! rects (free 2-D placement) while everything else on screen keeps
//! stack semantics.
//!
//! Interactions (select / drag / link) are pure view-model operations
//! the host (viewport, tests, protocol handlers) invokes; the view never
//! mutates the graph itself — command/undo semantics stay in the session
//! layer.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/node_graph.hpp"
#include "engine/ui/widget.hpp"

namespace omnicpp::editor {

//! Per-node view state: canvas position (view-space pixels, top-left of
//! the node card) and selection.
struct NodeView final {
  std::uint64_t node_id{0};
  float x{0.0F};
  float y{0.0F};
  bool selected{false};
};

//! The node editor view: owns ONLY view state (positions, selection);
//! the graph itself stays the caller's (session-owned) document.
class NodeEditorView final {
 public:
  //! Card styling constants (single source of truth for layout + tests).
  static constexpr float kCardW = 140.0F;
  static constexpr float kCardH = 56.0F;
  static constexpr float kPinH = 14.0F;  //!< vertical span per pin row

  explicit NodeEditorView(NodeGraph& graph) : graph_(&graph) {}

  //! Re-derives the widget subtree under `canvas_parent` from graph state.
  //! Idempotent: stale widgets from a previous rebuild are removed first.
  void rebuild(ui::WidgetTree& tree, std::uint32_t canvas_parent);

  //! Re-applies view state (positions, selection borders) to the widget
  //! cards built by rebuild(). Call after drag/select, before paint.
  void sync_widgets();

  //! Overlays wire geometry onto a paint list (after `paint(tree, ...)`).
  //! Wires render UNDER cards (prepended rects); pins render OVER
  //! everything (appended). One wire per link, from the source node's
  //! right edge to the target node's left edge at the linked pins'
  //! vertical offsets.
  void append_wires(ui::PaintList& list) const;

  [[nodiscard]] const std::vector<NodeView>& views() const noexcept {
    return views_;
  }
  [[nodiscard]] std::size_t view_count() const noexcept {
    return views_.size();
  }
  [[nodiscard]] NodeView* find_view(std::uint64_t node_id);
  [[nodiscard]] const NodeView* find_view(std::uint64_t node_id) const;

  //! Selects one node (0 deselects all; false when unknown).
  [[nodiscard]] bool select(std::uint64_t node_id);
  //! Moves a node by a delta (accumulates; false when unknown).
  [[nodiscard]] bool translate(std::uint64_t node_id, float dx, float dy);
  //! Sets absolute position (drag commit path).
  void set_position(std::uint64_t node_id, float x, float y);

  //! Canvas-space rect of a node's card, or false when unknown.
  [[nodiscard]] bool node_rect(std::uint64_t node_id, float& x, float& y,
                               float& w, float& h) const;

  //! Hit-test: topmost node whose card contains the point, else 0.
  //! Ties resolve to the highest node id (deterministic).
  [[nodiscard]] std::uint64_t hit_test(float x, float y) const;

 private:
  //! Pin-center offset within a card (pin index, 0-based, top-down).
  [[nodiscard]] static float pin_offset(int index) noexcept {
    return 18.0F + static_cast<float>(index) * kPinH;
  }
  //! Emits pin squares for all nodes at their CURRENT view positions.
  void append_pins(ui::PaintList& list) const;

  NodeGraph* graph_;
  ui::WidgetTree* tree_ref_{nullptr};  //!< set by rebuild()
  std::vector<NodeView> views_{};
  mutable std::vector<ui::PaintRect> wires_{};  //!< rebuilt by append_wires
  std::uint32_t card_root_{ui::kInvalidWidget};  //!< rebuilt subtree root
  std::uint32_t canvas_parent_{ui::kInvalidWidget};
};

//! Switches `canvas` to absolute layout (free placement of children).
void pin_canvas(ui::WidgetTree& tree, std::uint32_t canvas);

//! Builds the node-editor toolbar under `toolbar_parent`: one "add <type>"
//! button per registered node type (registration order), then undo/redo.
//! Returns all created button handles (types first, undo, redo last).
[[nodiscard]] std::vector<std::uint32_t> build_node_toolbar(
    ui::WidgetTree& tree, std::uint32_t toolbar_parent,
    const NodeGraph& graph);

}  // namespace omnicpp::editor
