# warploom-clip-timeline-view plan (G3-B)

Status: **gate doc** — the widget contract below is the acceptance
authority for G3-B, per the established discipline (design first, commit,
then implementation against it). Sibling of `docs/warploom-timeline-plan.md`
(clips core, G3, done) and `docs/warploom-editor-plan.md` (S4 module
extraction). Scope in one line: `ClipTimelineView` in `warploom-editor` —
a pure projection of `(document clips, playhead frame)` to the widget
tree, with a deterministic `clip_at` hit-test whose drag commits go
through the session's `clip_move` command.

## Ownership / layering

- Lives in `modules/editor` (`warploom/editor/clip_timeline_view.hpp`,
  `src/warploom/editor/clip_timeline_view.cpp`), consumed like
  `NodeEditorView`: the view is a **function of the document**, holds no
  document state of its own — only presentation knobs and drag state.
- Includes only module include roots (`warploom/core/*`,
  `warploom/ui/*`) — never superproject forwarders. The banked S4 lesson
  applies to this header/TU pair like every other.
- Zero render/asset dependencies. The viewport ADDs the strip above the
  W1 scrub panel (`TimelinePanel` stays untouched); no replacement, no
  chrome changes.

## Visual contract (deterministic geometry)

Constants (single source of truth, tests read them):

- `kRowH = 26.0F` — vertical span per clip row.
- `kPadX = 8.0F` — inner horizontal padding of the track.
- `kPlayheadW = 3.0F` — playhead band width.
- `kEmptyGapH = 6.0F` — strip height when there are no clips.

The host positions the track widget (`x/y/w/h`, view-space pixels —
Absolute-layout children keep view-space rects, same contract as the W1
strip). Projection maps frames to pixels **linearly across the track's
inner span** (`[x + kPadX, x + w - kPadX]`) over the frame window
`[view_start, view_start + view_frames)`:

- **Rows**: one per clip, ordered by clip id (document order IS id order —
  `SceneDocument::clips` is id-ordered). Row `i` occupies
  `y + 4 + i * kRowH`, height `kRowH - 2`. Color picks by index
  (`kClipColors[3]` cycle: teal, indigo, amber) so adjacent clips stay
  distinguishable without per-clip style state.
- **Block**: a clip's block starts at `frame_to_x(start)` with width
  `max(2, frame_to_x(start + length) - frame_to_x(start))` — an
  out-of-window or degenerate clip stays a visible 2px sliver instead of
  vanishing. Frame→x clamps to the inner span edges (blocks clip; they
  do not distort the scale).
- **Playhead**: one band at `frame_to_x(playhead)` (clamped), full track
  height, painted last (over blocks).
- **Row label**: the clip name, clamped to 8 characters (`clip_name_label`
  text), inside each block.

`rebuild(tree, track_parent)` builds the stable widget skeleton once
(track panel, playhead panel — handles never change); `sync(tree)` is the
per-frame projection: it diffs the current clip set against the previously
synced set (id + start + length equality per row) and rebuilds only the
block widgets when the set changed. Playhead-only updates are pure field
writes. Widget names: `clip_track_root`, `clip_playhead`,
`clip_block_<id>`, `clip_name_label`.

## Hit-testing / interaction contract

`clip_at(px, py)` → `ClipHit { clip_id, grab_offset_frames }` (invalid
when the point is not on a block; ties resolve to the lowest clip id —
overlaps are transient during drags and id order is deterministic).

Drag-to-move is a **view-model state machine** that never mutates the
document: `begin_clip_drag(hit, doc)` captures
`grab_offset_frames = playhead_at_grab - clip.start_frame`;
`update_clip_drag(px)` moves the pending start;
`end_clip_drag(commit)` produces the exact `ControlCommand`
(`ClipMove`, `numbers[0] = clip_id`, `numbers[1] = new_start`,
`number_count = 2`, `take("frame")` wire form) for the host to enqueue
through `editor.on_control` — command/undo semantics stay in the session
layer, mirroring `NodeEditorView::end_param_edit`'s out-parameter commit
contract. New start clamps at 0 (the session rejects negatives
structurally; the view never produces them).

## Frame window (v1 zoom/pan)

The strip maps `view_frames` starting at `view_start` (both settable:
`set_window(view_start, view_frames)`). Default window: `[0, 240)`.
Auto-fit is deliberately deferred: once clips exist, `view_frames` stays
host-owned until G3-C/D revisit it. Hit-testing inverts the same mapping,
so a drag never re-anchors under the pointer.

## Tests (acceptance)

New `modules/editor/tests/test_clip_timeline_view.cpp` in
`warploom_editor_tests`:

1. **Projection geometry**: fixed track rect + clips at authored spans →
   exact block rects (frame_to_x edges, clamped sliver for a
   partially-out-of-window clip), row y-ordering by id, playhead x at the
   mapped frame.
2. **Sync idempotence**: two syncs with unchanged clips keep widget
   handles stable (names `clip_block_<id>` findable, same handles);
   adding/removing a clip adds/removes exactly its block.
3. **Hit-test + drag commit**: `clip_at` on/off blocks (incl. tie → lower
   id); drag then commit produces the exact ClipMove ControlCommand
   payload at the expected new start (clamped at 0 case included).
4. **Empty document**: zero blocks, playhead still renders, strip uses
   `kEmptyGapH`.

## Verification discipline

- `warploom_editor_tests` green (new tests + existing three files).
- Full 6-suite ctest on the vulkan-validation leg; then the three
  remaining legs (headless-debug-clang, tsan, asan-ubsan) — 6/6 each.
- Standalone module consumer re-proof: rebuild `/tmp/editor_install` from
  this tree, re-run the S4 editor consumer (`/tmp/editor_consumer`) so the
  new header is proven part of the installable package surface
  (EDITOR_CONSUMER_OK).
- `check_docs_links.py` clean before doc commits.

## Non-goals

- No clips editing beyond move (add/remove/record stay protocol-only in
  G3-B; the block context menu arrives with G3-C if wanted).
- No sample/track visualization inside blocks (playhead + block extents
  only; per-track lanes are a later surface).
- No zoom/pan UI chrome (window is host-set; scrub strip keeps its own
  checkpoint model).
- Viewport integration beyond ADDing the strip (host-side wiring is a
  consumer concern, proven by the existing pattern, not gate-blocking).
