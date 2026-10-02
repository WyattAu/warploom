//! @file test_clip_timeline_view.cpp
//! @brief G3-B proofs for the clip timeline strip projection: deterministic
//!        block geometry over a host-owned frame window, sync idempotence,
//!        clip_at hit-testing, and drag-to-move committed as the exact
//!        session ClipMove command payload.

#include <gtest/gtest.h>

#include <cstdint>

#include "warploom/editor/clip_timeline_view.hpp"

namespace {

namespace ed = warploom::editor;
using ed::ClipTimelineView;
namespace core = ::warploom::core;

//! Adds a clip directly to the document (view tests do not go through the
//! session; the command path is proven in warploom_core_tests).
std::uint64_t add_clip(ed::SceneDocument& doc, const char* name,
                       std::uint64_t start, std::uint64_t length) {
  ed::TimelineClip clip;
  clip.id = doc.next_clip_id++;
  clip.name = name;
  clip.start_frame = start;
  clip.length_frames = length;
  const auto id = clip.id;
  doc.clips.push_back(std::move(clip));
  return id;
}

//! Standard fixture: 200px-wide track at (10, 20); default window [0, 240).
struct ViewFixture {
  ed::SceneDocument doc;
  warploom::ui::WidgetTree tree;
  std::uint32_t track_parent{warploom::ui::kInvalidWidget};
  ClipTimelineView view{doc};

  ViewFixture() {
    warploom::ui::Widget host;
    host.kind = warploom::ui::WidgetKind::Panel;
    host.layout = warploom::ui::LayoutMode::Absolute;
    host.name = "clip_strip_host";
    track_parent = tree.add(host, tree.root());
    tree.get(track_parent).x = 10.0F;
    tree.get(track_parent).y = 20.0F;
    tree.get(track_parent).w = 200.0F;
    tree.get(track_parent).h = 32.0F;
    view.rebuild(tree, track_parent);
  }

  //! Inner span: [10 + 8, 210 - 8) = [18, 202), 184px over 240 frames.
  [[nodiscard]] float x_at(std::uint64_t frame) const {
    return 18.0F + static_cast<float>(frame) * (184.0F / 240.0F);
  }
};

TEST(ClipTimelineView, ProjectsBlocksWithClampedSliver) {
  ViewFixture f;
  add_clip(f.doc, "move", 24, 72);  // fully inside the window
  // Fully out of the window (window is [0, 240)); must stay a sliver.
  add_clip(f.doc, "ghost", 400, 10);

  f.view.sync(f.tree, 0);

  float x = 0.0F;
  float y = 0.0F;
  float w = 0.0F;
  float h = 0.0F;
  ASSERT_TRUE(f.view.block_rect(1, x, y, w, h));
  EXPECT_FLOAT_EQ(x, f.x_at(24));
  EXPECT_FLOAT_EQ(w, f.x_at(96) - f.x_at(24));
  EXPECT_FLOAT_EQ(y, 20.0F + 4.0F);              // row 0
  EXPECT_FLOAT_EQ(h, ClipTimelineView::kRowH - 2.0F);

  // Out-of-window block clamps to the inner span's right edge: a 2px
  // sliver ending at the edge (frame_to_x saturates at tx+padX+inner_w).
  ASSERT_TRUE(f.view.block_rect(2, x, y, w, h));
  EXPECT_FLOAT_EQ(w, 2.0F);
  EXPECT_FLOAT_EQ(y, 20.0F + 4.0F + ClipTimelineView::kRowH);  // row 1

  // Playhead at frame 120 maps to the inner span's midpoint.
  f.view.sync(f.tree, 120);
  float px = 0.0F;
  float py = 0.0F;
  float pw = 0.0F;
  float ph = 0.0F;
  ASSERT_TRUE(f.view.playhead_rect(px, py, pw, ph));
  EXPECT_FLOAT_EQ(px + pw * 0.5F, f.x_at(120));
  EXPECT_FLOAT_EQ(pw, ClipTimelineView::kPlayheadW);
}

TEST(ClipTimelineView, SyncIsIdempotentAndDiffsBySpan) {
  ViewFixture f;
  add_clip(f.doc, "a", 0, 48);
  f.view.sync(f.tree, 0);

  const std::uint32_t block_before =
      f.tree.find_by_name("clip_block_1");
  const std::uint32_t label_before =
      f.tree.find_by_name("clip_name_label");
  ASSERT_NE(block_before, warploom::ui::kInvalidWidget);
  ASSERT_NE(label_before, warploom::ui::kInvalidWidget);

  // Same set: no widget churn, handles stable.
  f.view.sync(f.tree, 5);
  EXPECT_EQ(f.tree.find_by_name("clip_block_1"), block_before);
  EXPECT_EQ(f.tree.find_by_name("clip_name_label"), label_before);

  // Span change: that clip's block is re-derived (findable by name).
  f.doc.find_clip(1)->start_frame = 12;
  f.view.sync(f.tree, 5);
  const std::uint32_t block_after =
      f.tree.find_by_name("clip_block_1");
  EXPECT_NE(block_after, warploom::ui::kInvalidWidget);
  float x = 0.0F;
  float y = 0.0F;
  float w = 0.0F;
  float h = 0.0F;
  ASSERT_TRUE(f.view.block_rect(1, x, y, w, h));
  EXPECT_FLOAT_EQ(x, f.x_at(12));
}

TEST(ClipTimelineView, EmptyDocumentKeepsSkeletonAndGap) {
  ViewFixture f;
  f.view.sync(f.tree, 7);

  float x = 0.0F;
  float y = 0.0F;
  float w = 0.0F;
  float h = 0.0F;
  EXPECT_FALSE(f.view.block_rect(1, x, y, w, h));
  // Playhead still renders over the empty gap-height strip.
  ASSERT_TRUE(f.view.playhead_rect(x, y, w, h));
  EXPECT_FLOAT_EQ(h, ClipTimelineView::kEmptyGapH);
  EXPECT_FLOAT_EQ(x + w * 0.5F, f.x_at(7));

  // Adding a clip later grows the strip and renders the block.
  add_clip(f.doc, "late", 10, 20);
  f.view.sync(f.tree, 7);
  ASSERT_TRUE(f.view.block_rect(1, x, y, w, h));
  EXPECT_FLOAT_EQ(y, 20.0F + 4.0F);
}

TEST(ClipTimelineView, HitTestAndDragCommitProduceClipMove) {
  ViewFixture f;
  add_clip(f.doc, "one", 24, 48);  // frames [24, 72), row 0
  add_clip(f.doc, "two", 100, 48);
  f.view.sync(f.tree, 0);

  const float mid_x = f.x_at(48);  // middle of clip 1
  const float row_y = 20.0F + 4.0F + ClipTimelineView::kRowH * 0.5F;

  const auto hit = f.view.clip_at(mid_x, row_y);
  ASSERT_TRUE(hit.valid());
  EXPECT_EQ(hit.clip_id, 1U);
  EXPECT_EQ(hit.grab_offset_frames, 24U);  // grabbed at frame 48 of [24, 72)

  // Off-block and off-row points miss.
  EXPECT_FALSE(f.view.clip_at(f.x_at(0), row_y).valid());
  EXPECT_FALSE(f.view.clip_at(mid_x, 20.0F + 4.0F + 3.0F * ClipTimelineView::kRowH)
                   .valid());

  // Drag the grabbed block left by 12 frames: pending start = 48 - 24 -
  // 12 = 12 (offset-preserving), commit -> exact ClipMove payload.
  ASSERT_TRUE(f.view.begin_clip_drag(hit));
  f.view.update_clip_drag(f.x_at(36));
  core::ControlCommand cmd;
  ASSERT_TRUE(f.view.end_clip_drag(true, cmd));
  EXPECT_EQ(cmd.kind, core::ControlCommand::Kind::ClipMove);
  EXPECT_EQ(cmd.number_count, 2U);
  EXPECT_DOUBLE_EQ(cmd.numbers[0], 1.0);
  EXPECT_DOUBLE_EQ(cmd.numbers[1], 12.0);
  EXPECT_FALSE(f.view.drag_active());

  // Clamp at 0: grab at the block's first frame, drag far left.
  const auto hit2 = f.view.clip_at(f.x_at(24), row_y);
  ASSERT_TRUE(hit2.valid());
  ASSERT_TRUE(f.view.begin_clip_drag(hit2));
  f.view.update_clip_drag(f.x_at(0) - 50.0F);  // saturates below 0
  core::ControlCommand cmd2;
  ASSERT_TRUE(f.view.end_clip_drag(true, cmd2));
  EXPECT_DOUBLE_EQ(cmd2.numbers[1], 0.0);  // never negative
}

TEST(ClipTimelineView, CancelledDragCommitsNothing) {
  ViewFixture f;
  add_clip(f.doc, "one", 24, 48);
  f.view.sync(f.tree, 0);

  const auto hit =
      f.view.clip_at(f.x_at(30), 20.0F + 4.0F + ClipTimelineView::kRowH * 0.5F);
  ASSERT_TRUE(hit.valid());
  ASSERT_TRUE(f.view.begin_clip_drag(hit));
  f.view.update_clip_drag(f.x_at(100));

  core::ControlCommand cmd;
  EXPECT_FALSE(f.view.end_clip_drag(false, cmd));
  // The document is untouched by the view (no command, no mutation).
  EXPECT_EQ(f.doc.find_clip(1)->start_frame, 24U);
  EXPECT_FALSE(f.view.drag_active());
}

}  // namespace
