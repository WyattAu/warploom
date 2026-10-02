#pragma once

//! @file clip_timeline_view.hpp
//! @brief G3-B clip timeline strip: pure projection from the document's
//!        timeline clips to the UI widget tree (docs/warploom-clip-timeline-view-plan.md).
//!
//! Design: like NodeEditorView, the strip is a *function of the document*.
//! `rebuild()` derives the stable widget skeleton once (track panel +
//! playhead panel — handles never change); `sync(tree, playhead_frame)` is
//! the per-frame projection: it diffs the current clip set against the
//! previously synced one and rebuilds block widgets only when the set
//! changed, so playhead-only updates are pure field writes.
//!
//! Frames map linearly across the track's inner span
//! (`[x + kPadX, x + w - kPadX]`) over the host-owned frame window
//! `[view_start, view_start + view_frames)`. Blocks clamp to the inner
//! span edges (they clip; they never distort the scale), and a degenerate
//! or fully out-of-window clip stays a visible 2px sliver.
//!
//! Interactions are pure view-model operations: `clip_at` hit-tests,
//! the drag state machine tracks a pending start, and `end_clip_drag`
//! produces the exact `ControlCommand` (ClipMove) the host enqueues
//! through the session — command/undo semantics stay in the session
//! layer. The view never mutates the document.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "warploom/core/control_server.hpp"
#include "warploom/core/document.hpp"
#include "warploom/ui/widget.hpp"

namespace warploom::editor {
namespace ui = ::warploom::ui;  // S1: ui module moved to the warploom namespace

//! The clip timeline strip view: owns ONLY presentation + drag state; the
//! clips stay the caller's (session-owned) document.
class ClipTimelineView final {
 public:
  //! Styling/layout constants (single source of truth for layout + tests).
  static constexpr float kRowH = 26.0F;       //!< vertical span per clip row
  static constexpr float kPadX = 8.0F;        //!< inner horizontal padding
  static constexpr float kPlayheadW = 3.0F;   //!< playhead band width
  static constexpr float kEmptyGapH = 6.0F;   //!< strip height with no clips
  static constexpr std::size_t kClipColorCount = 3;
  //! Block fills cycle by row index (teal, indigo, amber).
  static constexpr std::array<std::uint32_t, kClipColorCount> kClipColors = {
      0xFF2C5D46, 0xFF3D4E8A, 0xFF8A6D2F};

  //! Default frame window: [0, 240).
  static constexpr std::uint64_t kDefaultViewFrames = 240;

  explicit ClipTimelineView(const SceneDocument& doc) : doc_(&doc) {}

  //! Sets the visible frame window (host-owned; hit-testing inverts the
  //! same mapping, so a drag never re-anchors under the pointer).
  void set_window(std::uint64_t view_start, std::uint64_t view_frames) {
    view_start_ = view_start;
    view_frames_ = view_frames;
  }
  [[nodiscard]] std::uint64_t view_start() const noexcept {
    return view_start_;
  }
  [[nodiscard]] std::uint64_t view_frames() const noexcept {
    return view_frames_;
  }

  //! Builds the stable widget skeleton under the host-positioned
  //! `track_parent` (an Absolute-layout panel the host sizes each frame).
  //! Idempotent: a previous skeleton is removed first; block widgets are
  //! (re)derived by sync().
  void rebuild(ui::WidgetTree& tree, std::uint32_t track_parent);

  //! Per-frame projection: takes the track rect from `track_parent`'s
  //! computed widget rect, diffs the clip set, updates block/label/playhead
  //! rects. Cheap when only the playhead moved.
  void sync(ui::WidgetTree& tree, std::uint64_t playhead_frame);

  //! A block hit: the clip and how far into it the grab was (frames).
  struct ClipHit final {
    std::uint64_t clip_id{0};
    std::uint64_t grab_offset_frames{0};
    [[nodiscard]] bool valid() const noexcept { return clip_id != 0U; }
  };

  //! Hit-test: the clip whose block contains (px, py). Ties (overlapping
  //! blocks) resolve to the LOWEST clip id (deterministic). Invalid hit
  //! when the point is not on a block.
  [[nodiscard]] ClipHit clip_at(float px, float py) const;

  //! Begins a drag from a hit (one drag at a time). The grab offset from
  //! the hit is kept, so the block moves exactly under the pointer.
  bool begin_clip_drag(const ClipHit& hit);
  //! Updates the pending start from a pointer x (inverted frame mapping);
  //! clamps at 0. No-op when not dragging.
  void update_clip_drag(float px);
  //! Ends the drag. On commit, fills `out` with the exact ClipMove
  //! ControlCommand payload (numbers[0] = clip id, numbers[1] = new start,
  //! number_count = 2) for the host to enqueue through the session.
  //! Returns false when not dragging; a cancel (commit=false) returns
  //! false and clears the drag.
  [[nodiscard]] bool end_clip_drag(bool commit,
                                   ::warploom::core::ControlCommand& out);
  [[nodiscard]] bool drag_active() const noexcept { return dragging_; }

  //! Test seams: exact geometry of the last sync without duplicating the
  //! frame->pixel mapping in tests. False when the id is not rendered
  //! (block) or the track rect is unset (playhead).
  [[nodiscard]] bool block_rect(std::uint64_t clip_id, float& x, float& y,
                                float& w, float& h) const;
  [[nodiscard]] bool playhead_rect(float& x, float& y, float& w,
                                   float& h) const;

 private:
  //! Frame -> view-space x over the inner span (clamped to its edges).
  [[nodiscard]] float frame_to_x(std::uint64_t frame) const noexcept;
  //! Inverse mapping (pointer x -> frame, rounded to nearest, clamped).
  [[nodiscard]] std::uint64_t x_to_frame(float px) const noexcept;
  //! Strip height for the current clip count (empty gap vs full rows).
  [[nodiscard]] float strip_height() const noexcept;
  //! Drops and re-derives the per-clip block widgets (id order).
  void rebuild_blocks(ui::WidgetTree& tree);

  //! One rendered row: a snapshot of the clip's identity + span, plus the
  //! block widget handle (the name label is a child of the block).
  struct Row final {
    std::uint64_t clip_id{0};
    std::uint64_t start{0};
    std::uint64_t length{0};
    std::uint32_t block{ui::kInvalidWidget};
    std::uint32_t label{ui::kInvalidWidget};
  };

  const SceneDocument* doc_;
  std::uint32_t track_parent_{ui::kInvalidWidget};
  std::uint32_t root_{ui::kInvalidWidget};
  std::uint32_t playhead_{ui::kInvalidWidget};
  std::vector<Row> rows_{};
  //! Frame window (host-owned).
  std::uint64_t view_start_{0};
  std::uint64_t view_frames_{kDefaultViewFrames};
  //! Track rect captured by the last sync (x_to_frame/clip_at use it).
  float tx_{0.0F};
  float ty_{0.0F};
  float tw_{0.0F};
  float th_{0.0F};
  std::uint64_t playhead_frame_{0};
  //! Drag state (no active drag when dragging_ is false).
  bool dragging_{false};
  std::uint64_t drag_clip_{0};
  std::uint64_t grab_offset_{0};
  std::uint64_t drag_pending_start_{0};
};

}  // namespace warploom::editor

// S5-B compat footer: legacy `omnicpp::editor` spellings keep resolving
// during the transition (docs/warploom-identity-plan.md, phase 1b). This
// is the SAME guarded directive core's headers carry (the editor
// namespace hosts both core session types and module widgets - extension
// blocks merge), so one include of either family suffices.
#ifndef WARPLOOM_COMPAT_EDITOR_NS
#define WARPLOOM_COMPAT_EDITOR_NS
namespace omnicpp::editor {
    using namespace ::warploom::editor;
}
#endif  // WARPLOOM_COMPAT_EDITOR_NS
