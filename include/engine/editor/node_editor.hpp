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

//! Formats a NodeValue compactly for card readouts (number/bool/string/vec3).
[[nodiscard]] std::string value_to_text(const NodeValue& value);

//! Per-node view state: canvas position (view-space pixels, top-left of
//! the node card) and selection.
struct NodeView final {
  std::uint64_t node_id{0};
  float x{0.0F};
  float y{0.0F};
  bool selected{false};
};

//! Wire routing style. Bezier approximates a horizontal cubic bezier
//! (control points at the horizontal midpoint) with small rect segments —
//! deterministic, renderer-agnostic, and rasterizes on every backend.
enum class WireStyle : std::uint8_t {
  Straight,  //!< one band per link
  Bezier,    //!< segment-approximated horizontal S-curve
};

//! A pin reference: one named pin on one node.
struct PinRef final {
  std::uint64_t node_id{0};
  std::string pin_name{};
  bool is_input{false};
  [[nodiscard]] bool valid() const noexcept { return node_id != 0U; }
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
  //! everything (appended). Routing follows `wire_style`.
  void append_wires(ui::PaintList& list) const;

  //! After `graph.evaluate()`: appends one text readout per node output
  //! pin ("name=value") at the card bottom when value readouts are on.
  void set_show_values(bool on) noexcept { show_values_ = on; }
  [[nodiscard]] bool show_values() const noexcept { return show_values_; }

  void set_wire_style(WireStyle style) noexcept { wire_style_ = style; }
  [[nodiscard]] WireStyle wire_style() const noexcept { return wire_style_; }

  //! Pin-level hit-test: the pin square containing (x, y), or an invalid
  //! PinRef. Inputs sit on a card's left edge, outputs on its right.
  [[nodiscard]] PinRef pin_at(float x, float y) const;

  //! Link hit-test: the graph link nearest to (x, y) within `threshold`
  //! pixels (segment distance along the CURRENT routing), or 0. Deterministic:
  //! ties break by lowest link index.
  [[nodiscard]] std::size_t link_at(float x, float y,
                                    float threshold) const;
  //! The link list index -> GraphLink (or nullptr). For unlink commits.
  [[nodiscard]] const GraphLink* link_at_index(std::size_t index) const;

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

  // -- Link dragging (M7) -------------------------------------------------
  //! Begins a link drag from an output pin. Only one drag at a time.
  void begin_link_drag(const PinRef& from);
  //! Updates the rubber-band target (call on mouse motion during a drag).
  void update_link_drag(float x, float y);
  //! Ends the drag; the commit caller resolves `pending_pin` through the
  //! session (LinkNodes) and calls this after. Cancels when `commit` is
  //! false. Returns the pending pin (invalid PinRef when not dragging).
  PinRef end_link_drag(bool commit);
  [[nodiscard]] bool link_drag_active() const noexcept {
    return drag_pin_.valid();
  }
  //! The pin a committed link would land on, or an invalid PinRef.
  [[nodiscard]] PinRef pending_pin() const { return pending_pin_; }
  [[nodiscard]] PinRef drag_source_pin() const { return drag_pin_; }

  //! Test seam: exposes the private endpoint resolver to wire tests so they
  //! compute exact geometry without duplicating layout constants.
  [[nodiscard]] bool link_endpoints_public(const GraphLink& link, float& x0,
                                           float& y0, float& x1,
                                           float& y1) const {
    return link_endpoints(link, x0, y0, x1, y1);
  }

 private:
  //! Pin-center offset within a card (pin index, 0-based, top-down).
  [[nodiscard]] static float pin_offset(int index) noexcept {
    return 18.0F + static_cast<float>(index) * kPinH;
  }
  //! Emits pin squares for all nodes at their CURRENT view positions.
  void append_pins(ui::PaintList& list) const;
  //! Emits per-output value readout texts (post-evaluate).
  void append_value_texts(ui::PaintList& list) const;
  //! Emits one straight band for every link.
  void emit_straight_wires(ui::PaintList& list) const;
  //! Emits bezier-approximated wires (8 segments per link).
  void emit_bezier_wires(ui::PaintList& list) const;
  //! Wire geometry endpoints for one link; false when unresolvable.
  [[nodiscard]] bool link_endpoints(const GraphLink& link, float& x0,
                                    float& y0, float& x1, float& y1) const;
  //! Emits the rubber-band wire from the drag source pin to the current
  //! pointer position (dashed = 3px bands with gaps; distinct from solid
  //! committed wires).
  void emit_pending_wire(ui::PaintList& list) const;
  //! Point-to-segment distance in pixels (wire hit-testing).
  [[nodiscard]] static float segment_distance(float px, float py, float x0,
                                              float y0, float x1, float y1);

  NodeGraph* graph_;
  ui::WidgetTree* tree_ref_{nullptr};  //!< set by rebuild()
  std::vector<NodeView> views_{};
  mutable std::vector<ui::PaintRect> wires_{};  //!< rebuilt by append_wires
  WireStyle wire_style_{WireStyle::Bezier};
  bool show_values_{true};
  std::uint32_t card_root_{ui::kInvalidWidget};  //!< rebuilt subtree root
  std::uint32_t canvas_parent_{ui::kInvalidWidget};
  // Link-drag state (invalid pin = no active drag).
  PinRef drag_pin_{};
  PinRef pending_pin_{};
  float drag_x_{0.0F};
  float drag_y_{0.0F};
};

//! Switches `canvas` to absolute layout (free placement of children).
void pin_canvas(ui::WidgetTree& tree, std::uint32_t canvas);

//! Builds the node-editor toolbar under `toolbar_parent`: one "add <type>"
//! button per registered node type (registration order), then undo/redo.
//! Returns all created button handles (types first, undo, redo last).
[[nodiscard]] std::vector<std::uint32_t> build_node_toolbar(
    ui::WidgetTree& tree, std::uint32_t toolbar_parent,
    const NodeGraph& graph);

//! Toolbar action resolved from a click. `type_index` indexes the
//! REGISTRATION-ORDER type list (same order the buttons were built in).
enum class ToolbarAction : std::uint8_t {
  None,
  AddType,  //!< add a node of types_[type_index]
  Undo,
  Redo,
};
struct ToolbarHit final {
  ToolbarAction action{ToolbarAction::None};
  std::size_t type_index{0};
};

//! Resolves a click at (x, y) against the buttons `build_node_toolbar`
//! created (handles in the same order). Layout must have run. Layout-built
//! widget rects are authoritative — no duplicated geometry tables.
[[nodiscard]] ToolbarHit hit_test_toolbar(
    const ui::WidgetTree& tree,
    const std::vector<std::uint32_t>& buttons, const NodeGraph& graph,
    float x, float y);

}  // namespace omnicpp::editor
