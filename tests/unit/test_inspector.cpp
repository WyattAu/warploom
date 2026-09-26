//! @file test_inspector.cpp
//! @brief M11 inspector/outliner proofs: the panel is a pure projection of
//!        (document, registry, selection, bindings); clicks resolve against
//!        layout-authoritative rects; binding anchors are exactly the chip
//!        rects; rebuilds are idempotent (no widget leaks).

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "engine/editor/inspector.hpp"
#include "engine/editor/node_editor.hpp"

namespace {

namespace ed = omnicpp::editor;
namespace ui = warploom::ui;

using ed::EditorSession;
using ed::InspectorHit;
using ed::InspectorPanel;

//! Builds a session with the environment (id 1) plus one protocol-spawned
//! cube, and a const node with a graph->scene binding.
struct Fixture {
  EditorSession session;
  std::uint64_t cube_id{0};

  Fixture() {
    omnicpp::core::ControlCommand spawn;
    spawn.kind = omnicpp::core::ControlCommand::Kind::SpawnCube;
    spawn.numbers[0] = 1.0;
    spawn.numbers[1] = 2.0;
    spawn.numbers[2] = 3.0;
    spawn.numbers[3] = 1.5;
    spawn.number_count = 4;
    const auto reply = session.on_control(spawn);
    EXPECT_TRUE(reply.ok) << reply.error;
    // The spawned cube is the highest object id.
    for (const auto& o : session.document().objects) {
      cube_id = std::max(cube_id, o.id);
    }
  }

  [[nodiscard]] const omnicpp::editor::SceneObject* cube() const {
    return session.document().find(cube_id);
  }
};

TEST(InspectorProjection, OutlinerListsEveryObject) {
  Fixture f;
  InspectorPanel panel;
  ui::WidgetTree tree;
  panel.rebuild(tree, tree.root(), f.session.document(),
                f.session.registry(), 0, f.session.bindings());

  // One row per object; names are the stable lookup keys.
  for (const auto& obj : f.session.document().objects) {
    const auto handle =
        tree.find_by_name("obj_row_" + std::to_string(obj.id));
    EXPECT_NE(handle, ui::kInvalidWidget) << obj.id;
    EXPECT_EQ(tree.get(handle).text, obj.name);
  }
}

TEST(InspectorProjection, PropertiesMirrorSelectedObject) {
  Fixture f;
  InspectorPanel panel;
  ui::WidgetTree tree;
  panel.rebuild(tree, tree.root(), f.session.document(),
                f.session.registry(), f.cube_id, f.session.bindings());

  // The cube's scale property renders as a row with the default value.
  const auto handle =
      tree.find_by_name("prop_row_" + std::to_string(f.cube_id) + "_scale");
  ASSERT_NE(handle, ui::kInvalidWidget);
  EXPECT_NE(tree.get(handle).text.find("scale ="), std::string::npos);

  // No selection: the properties section shows the placeholder instead.
  ui::WidgetTree tree2;
  InspectorPanel panel2;
  panel2.rebuild(tree2, tree2.root(), f.session.document(),
                 f.session.registry(), 0, f.session.bindings());
  EXPECT_NE(tree2.find_by_name("props_none"), ui::kInvalidWidget);
}

TEST(InspectorProjection, ClickSelectsObjectThroughLayoutRects) {
  Fixture f;
  InspectorPanel panel;
  ui::WidgetTree tree;
  panel.rebuild(tree, tree.root(), f.session.document(),
                f.session.registry(), 0, f.session.bindings());
  ui::compute_layout(tree, 1280.0F, 720.0F);

  // Click the cube's outliner row (find its computed rect).
  const auto handle = tree.find_by_name("obj_row_" + std::to_string(f.cube_id));
  ASSERT_NE(handle, ui::kInvalidWidget);
  const auto& row = tree.get(handle);
  const auto hit = panel.hit_test(tree, row.x + row.w / 2.0F,
                                  row.y + row.h / 2.0F);
  EXPECT_EQ(hit.kind, InspectorHit::Kind::SelectObject);
  EXPECT_EQ(hit.object_id, f.cube_id);

  // A click far outside the panel resolves to None.
  const auto miss = panel.hit_test(tree, 60.0F, 400.0F);
  EXPECT_EQ(miss.kind, InspectorHit::Kind::None);
}

TEST(InspectorProjection, BindingChipsAndAnchors) {
  Fixture f;
  // Graph: const node -> bind to the cube's scale.x (number -> vec3 axis).
  auto& graph = f.session.document().node_graph;
  const auto cn = graph.add_node(
      "const_number", {{"value", ed::NodeValue::make_number(1.25)}});
  std::string err;
  ASSERT_TRUE(
      f.session.bind_property(cn, "value", f.cube_id, "scale.x", err))
      << err;

  InspectorPanel panel;
  ui::WidgetTree tree;
  panel.rebuild(tree, tree.root(), f.session.document(),
                f.session.registry(), 0, f.session.bindings());
  ui::compute_layout(tree, 1280.0F, 720.0F);

  // One chip row rendered with the binding summary text.
  const auto chip = tree.find_by_name("bind_row_0");
  ASSERT_NE(chip, ui::kInvalidWidget);
  EXPECT_NE(tree.get(chip).text.find("scale.x"), std::string::npos);

  // The anchor is the chip's left-edge center (the wire target).
  float ax = 0.0F;
  float ay = 0.0F;
  ASSERT_TRUE(panel.binding_anchor(tree, 0, ax, ay));
  const auto& chip_w = tree.get(chip);
  EXPECT_FLOAT_EQ(ax, chip_w.x);
  EXPECT_FLOAT_EQ(ay, chip_w.y + chip_w.h * 0.5F);

  // Unknown index -> no anchor.
  float bx = 0.0F;
  float by = 0.0F;
  EXPECT_FALSE(panel.binding_anchor(tree, 7, bx, by));

  // The node editor consumes the same anchors as binding wires.
  ed::NodeEditorView view(graph);
  std::vector<ed::NodeEditorView::BindingWire> wires;
  const auto& bindings = f.session.bindings();
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    float x = 0.0F;
    float y = 0.0F;
    if (panel.binding_anchor(tree, i, x, y)) {
      wires.push_back({bindings[i].node_id, bindings[i].out_pin, x, y});
    }
  }
  view.set_binding_wires(std::move(wires));
  ui::PaintList paint;
  view.rebuild(tree, tree.root());
  view.append_binding_wires(paint);
  // 8 segments, every other dashed out -> exactly 4 rects per wire.
  EXPECT_EQ(paint.rects.size(), 4U);
}

TEST(InspectorProjection, RebuildIsIdempotent) {
  Fixture f;
  InspectorPanel panel;
  ui::WidgetTree tree;
  panel.rebuild(tree, tree.root(), f.session.document(),
                f.session.registry(), 0, f.session.bindings());
  const std::size_t alive_first = tree.alive_count();
  panel.rebuild(tree, tree.root(), f.session.document(),
                f.session.registry(), 0, f.session.bindings());
  const std::size_t alive_second = tree.alive_count();
  EXPECT_EQ(alive_first, alive_second);
  // The panel handle is stable and still in the tree.
  EXPECT_NE(tree.find_by_name("inspector_panel"), ui::kInvalidWidget);
}

}  // namespace
