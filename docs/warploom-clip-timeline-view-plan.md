Status: **gate doc**; the widget contract below is the acceptance
extraction). Scope in one line: `ClipTimelineView` in `warploom-editor` -
  document state of its own; only presentation knobs and drag state.
  `warploom/ui/*`); never superproject forwarders. The banked S4 lesson
- `kRowH = 26.0F`; vertical span per clip row.
- `kPadX = 8.0F`; inner horizontal padding of the track.
- `kPlayheadW = 3.0F`; playhead band width.
- `kEmptyGapH = 6.0F`; strip height when there are no clips.
The host positions the track widget (`x/y/w/h`, view-space pixels -
- **Rows**: one per clip, ordered by clip id (document order IS id order -
  `max(2, frame_to_x(start + length) - frame_to_x(start))`; an
(track panel, playhead panel; handles never change); `sync(tree)` is the
when the point is not on a block; ties resolve to the lowest clip id -
through `editor.on_control`; command/undo semantics stay in the session
  remaining legs (headless-debug-clang, tsan, asan-ubsan); 6/6 each.