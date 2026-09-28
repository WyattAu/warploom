//! @file clip_timeline_view.cpp
//! @brief G3-B implementation of the clip timeline strip projection
//!        (contract: docs/warploom-clip-timeline-view-plan.md).

#include "warploom/editor/clip_timeline_view.hpp"

#include <algorithm>
#include <cmath>

namespace omnicpp::editor {

void ClipTimelineView::rebuild(ui::WidgetTree& tree,
                               std::uint32_t track_parent) {
  track_parent_ = track_parent;
  if (root_ != ui::kInvalidWidget) {
    tree.remove(root_);  // idempotent rebuild: drop the previous skeleton
    root_ = ui::kInvalidWidget;
    playhead_ = ui::kInvalidWidget;
    rows_.clear();
  }

  ui::Widget track;
  track.kind = ui::WidgetKind::Panel;
  track.layout = ui::LayoutMode::Absolute;
  track.name = "clip_track_root";
  track.color = 0xFF171A20;
  track.border_color = 0xFF2A2F38;
  root_ = tree.add(track, track_parent);

  ui::Widget head;
  head.kind = ui::WidgetKind::Panel;
  head.layout = ui::LayoutMode::Absolute;
  head.name = "clip_playhead";
  head.color = 0xFFFFB000;
  playhead_ = tree.add(head, root_);
}

float ClipTimelineView::frame_to_x(std::uint64_t frame) const noexcept {
  const float inner_w = std::max(1.0F, tw_ - 2.0F * kPadX);
  const double t = static_cast<double>(frame - std::min(frame, view_start_)) /
                   static_cast<double>(std::max<std::uint64_t>(1, view_frames_));
  const float clamped =
      std::clamp(static_cast<float>(t), 0.0F, 1.0F) * inner_w;
  return tx_ + kPadX + clamped;
}

std::uint64_t ClipTimelineView::x_to_frame(float px) const noexcept {
  const float inner_w = std::max(1.0F, tw_ - 2.0F * kPadX);
  const float frac =
      std::clamp((px - (tx_ + kPadX)) / inner_w, 0.0F, 1.0F);
  const double f = static_cast<double>(view_start_) +
                   static_cast<double>(frac) *
                       static_cast<double>(view_frames_);
  if (f <= 0.0) {
    return 0;
  }
  return static_cast<std::uint64_t>(std::llround(f));
}

float ClipTimelineView::strip_height() const noexcept {
  return rows_.empty() ? kEmptyGapH
                       : 4.0F + static_cast<float>(rows_.size()) * kRowH;
}

void ClipTimelineView::rebuild_blocks(ui::WidgetTree& tree) {
  for (const Row& r : rows_) {
    if (r.block != ui::kInvalidWidget) {
      tree.remove(r.block);  // unlinks block + its name-label child
    }
  }
  rows_.clear();

  // Document order IS id order (id-ordered storage), so rows render
  // top-down by ascending id.
  for (const auto& clip : doc_->clips) {
    Row row;
    row.clip_id = clip.id;
    row.start = clip.start_frame;
    row.length = clip.length_frames;

    ui::Widget block;
    block.kind = ui::WidgetKind::Panel;
    block.layout = ui::LayoutMode::Absolute;
    block.name = "clip_block_" + std::to_string(clip.id);
    block.color =
        kClipColors[(rows_.size()) % kClipColorCount];
    row.block = tree.add(block, root_);

    ui::Widget label;
    label.kind = ui::WidgetKind::Label;
    label.layout = ui::LayoutMode::Absolute;
    label.name = "clip_name_label";
    label.text = clip.name.substr(0, 8);
    label.text_color = 0xFFC8CDD6;
    row.label = tree.add(label, row.block);

    rows_.push_back(std::move(row));
  }
}

void ClipTimelineView::sync(ui::WidgetTree& tree,
                            std::uint64_t playhead_frame) {
  if (root_ == ui::kInvalidWidget || track_parent_ == ui::kInvalidWidget) {
    return;
  }
  const ui::Widget& parent = tree.get(track_parent_);
  tx_ = parent.x;
  ty_ = parent.y;
  tw_ = parent.w;
  th_ = parent.h;

  // Diff: rebuild blocks only when the rendered set (id + span) changed.
  bool changed = rows_.size() != doc_->clips.size();
  if (!changed) {
    std::size_t i = 0;
    for (const auto& clip : doc_->clips) {
      const Row& r = rows_[i];
      if (r.clip_id != clip.id || r.start != clip.start_frame ||
          r.length != clip.length_frames) {
        changed = true;
        break;
      }
      ++i;
    }
  }
  if (changed) {
    rebuild_blocks(tree);
  }

  playhead_frame_ = playhead_frame;
  std::size_t i = 0;
  for (auto& r : rows_) {
    const TimelineClip* clip = doc_->find_clip(r.clip_id);
    if (clip == nullptr) {
      ++i;
      continue;  // cannot happen (rebuild on change); defensive
    }
    ui::Widget& b = tree.get(r.block);
    b.x = frame_to_x(clip->start_frame);
    b.y = ty_ + 4.0F + static_cast<float>(i) * kRowH;
    b.w = std::max(2.0F, frame_to_x(clip->start_frame + clip->length_frames) -
                            frame_to_x(clip->start_frame));
    b.h = kRowH - 2.0F;
    ui::Widget& l = tree.get(r.label);
    l.x = 3.0F;
    l.y = 4.0F;
    l.w = std::max(0.0F, b.w - 6.0F);
    l.h = 16.0F;
    ++i;
  }

  // Track self-sizes to its rows (host sized the parent; we size the
  // strip so viewport layout stays stable with/without clips).
  ui::Widget& tr = tree.get(root_);
  tr.x = tx_;
  tr.y = ty_;
  tr.w = tw_;
  tr.h = strip_height();

  ui::Widget& ph = tree.get(playhead_);
  ph.x = frame_to_x(playhead_frame) - kPlayheadW * 0.5F;
  ph.y = ty_;
  ph.w = kPlayheadW;
  ph.h = tr.h;
}

ClipTimelineView::ClipHit ClipTimelineView::clip_at(float px,
                                                    float py) const {
  ClipHit hit;
  if (rows_.empty() || tw_ == 0.0F) {
    return hit;
  }
  // One block per row, rows in ascending id order with disjoint y-bands —
  // so the first (lowest-id) row whose band AND block x-range contain the
  // point is the hit; the gate doc's lowest-id tie rule holds trivially.
  const float row_f = (py - (ty_ + 4.0F)) / kRowH;
  const int row_index = static_cast<int>(std::floor(row_f));
  if (row_index < 0 || row_index >= static_cast<int>(rows_.size())) {
    return hit;
  }
  const Row& r = rows_[static_cast<std::size_t>(row_index)];
  const float bx = frame_to_x(r.start);
  const float bw =
      std::max(2.0F, frame_to_x(r.start + r.length) - bx);
  if (px < bx || px >= bx + bw) {
    return hit;
  }
  hit.clip_id = r.clip_id;
  const std::uint64_t frame = x_to_frame(px);
  hit.grab_offset_frames = frame > r.start ? frame - r.start : 0;
  return hit;
}

bool ClipTimelineView::begin_clip_drag(const ClipHit& hit) {
  if (!hit.valid()) {
    return false;
  }
  const TimelineClip* clip = doc_->find_clip(hit.clip_id);
  if (clip == nullptr) {
    return false;
  }
  dragging_ = true;
  drag_clip_ = hit.clip_id;
  grab_offset_ = hit.grab_offset_frames;
  drag_pending_start_ = clip->start_frame;
  return true;
}

void ClipTimelineView::update_clip_drag(float px) {
  if (!dragging_) {
    return;
  }
  const std::uint64_t frame = x_to_frame(px);
  drag_pending_start_ =
      frame > grab_offset_ ? frame - grab_offset_ : 0;  // clamp at 0
}

bool ClipTimelineView::end_clip_drag(
    bool commit, omnicpp::core::ControlCommand& out) {
  if (!dragging_) {
    return false;
  }
  const std::uint64_t clip_id = drag_clip_;
  const std::uint64_t new_start = drag_pending_start_;
  dragging_ = false;
  drag_clip_ = 0;
  grab_offset_ = 0;
  drag_pending_start_ = 0;
  if (!commit) {
    return false;
  }
  out.kind = omnicpp::core::ControlCommand::Kind::ClipMove;
  out.numbers[0] = static_cast<double>(clip_id);
  out.numbers[1] = static_cast<double>(new_start);
  out.number_count = 2;
  return true;
}

bool ClipTimelineView::block_rect(std::uint64_t clip_id, float& x, float& y,
                                  float& w, float& h) const {
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const Row& r = rows_[i];
    if (r.clip_id != clip_id) {
      continue;
    }
    const TimelineClip* clip = doc_->find_clip(clip_id);
    if (clip == nullptr) {
      return false;
    }
    x = frame_to_x(clip->start_frame);
    y = ty_ + 4.0F + static_cast<float>(i) * kRowH;
    w = std::max(2.0F, frame_to_x(clip->start_frame +
                                  clip->length_frames) -
                          frame_to_x(clip->start_frame));
    h = kRowH - 2.0F;
    return true;
  }
  return false;
}

bool ClipTimelineView::playhead_rect(float& x, float& y, float& w,
                                     float& h) const {
  if (root_ == ui::kInvalidWidget || tw_ == 0.0F) {
    return false;
  }
  x = frame_to_x(playhead_frame_) - kPlayheadW * 0.5F;
  y = ty_;
  w = kPlayheadW;
  h = strip_height();
  return true;
}

}  // namespace omnicpp::editor
