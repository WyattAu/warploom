# warploom timeline — G3 dependency analysis and design (gate doc)

Status: **plan** (docs/roadmap.md track G). Gate document for G3, same
discipline as the S-track plans: what the timeline owns, how it composes
with W1/W2/G1, what breaks, and the order of operations. Counts measured.

## Measured starting point

- **No signal store exists.** The session's node outputs are transient
  (`NodeGraph::evaluate` per tick; bindings push into properties); the only
  per-frame history is the W1 scrubber's whole-document checkpoints
  (sparse, explicit). Nothing samples values over time.
- **The viewport's timeline is scrub chrome only** (`TimelinePanel`,
  main.cpp:186–325): a W1 checkpoint strip. It knows nothing about clips.
- **The document has no time-indexed entities**: `SceneDocument` (schema
  v2) = objects + node graph + layout. Commands are reversible
  `apply/undo` edits through `CommandStack`; the byte-deterministic
  serializer is hand-rolled per field (`document.cpp`).
- **W2's recorder is protocol-only**: `CommandRecorder` observes commands;
  headless sessions advance logical frames via `step` (`record_external`
  for host-handled pause/resume/step). The headless host
  (`tools/headless_host.cpp`) has no sim loop — properties change only
  through `set_property` commands.

## Boundary decision

**Clips are document entities** (schema v2 → v3). This is the whole
composition story, and it is free:

- G1: save/load round-trips them (byte-deterministic serializer gains one
  section).
- W1: `scrub_start` checkpoints capture them; `scrub_to` restores them
  (clip definitions return to the warp point; session-side armed states
  disarm — see interplay below).
- W2: clip edit commands join the recorded-kinds set and replay
  re-applies them; recorded track content rides the closing checkpoint
  (see "Replay interplay").
- Undo: clip add/remove/move are normal `Command` classes.

**Recording/playback is session-side, host-ticked, deterministic** — direct
application, NOT commands (same model as graph→scene bindings: driven
values are not undoable; the graph/clip owns them, and W1 scrubbing is the
recovery path). A clip's track stores **property samples** — recording a
bound property IS recording a graph signal as it lands in the scene
(roadmap wording "recorded graph signals" satisfied without a parallel
pin-sampling store; raw pin sampling is a future item, not v1).

**The panel widget lands in `warploom-editor`** (G3 phase B) as a
projection of (document clips, scrubber checkpoints) — that is why G3 was
held until after S4. Phase A (this session) is the core + protocol +
proof; the viewport's existing `TimelinePanel` stays as-is until the
module widget replaces it.

## Data model (document v3)

```cpp
struct ClipSample  { std::uint64_t frame_offset; PropValue value; };
struct ClipTrack   { std::uint64_t object_id; std::string property;
                     std::vector<ClipSample> samples; };  // sorted by offset
struct TimelineClip final {
  std::uint64_t id{0};
  std::string name{};
  std::uint64_t start_frame{0};
  std::uint64_t length_frames{0};
  std::map<std::string, ClipTrack> tracks{};  // key "oid:property"; sorted
};
// SceneDocument: std::vector<TimelineClip> clips{}; std::uint64_t next_clip_id{1};
// kDocumentSchemaVersion 2 -> 3 (from_json accepts <= 3; old files load,
// clips default empty; writer emits clips only when non-empty so v2-era
// documents stay byte-stable).
```

Frames, not seconds — the engine's clock is deterministic ticks; the
scrubber and recorder are already frame-indexed. Samples are
`frame_offset` relative to `start_frame` (clips stay movable without
rewriting samples). Step-hold interpolation (last sample ≤ offset; nothing
before the first sample) — linear interpolation is a documented v1.1
candidate, not v1.

## Protocol v1.8

| Kind | Wire | Payload | Recorded by W2? |
|---|---|---|---|
| `ClipAdd` | `clip_add` | `text`=name, `n0`=start, `n1`=length | yes |
| `ClipRemove` | `clip_remove` | `n0`=clip id | yes |
| `ClipMove` | `clip_move` | `n0`=clip id, `n1`=start | yes |
| `ClipRecord` | `clip_record` | `n0`=clip id, `n1`=object id, `text`=property | yes |
| `ClipRecordStop` | `clip_record_stop` | — | yes |
| `ClipPlay` | `clip_play` | `n0`=clip id, `n1`=start frame (optional) | yes |
| `ClipStop` | `clip_stop` | — | yes |
| `ClipsInfo` | `clips_info` | — (query → JSON array) | no (query) |

Undo/redo stay generic (`undo`/`redo` already recorded). The wire-parity
test walks `kind_names()`, so the new kinds are parser-covered the moment
they enter the table — the protocol invariant is enforced, not remembered.

## Session semantics (the decisions)

- **Arming**: at most one armed record target and one armed playback clip
  at a time (v1; documented). `clip_record` validates clip, object,
  property exist; `clip_play` validates the clip has at least one track.
- **`tick_timeline(frame)`** — new host-ticked hook, called once per sim
  tick AFTER `sync_graph` (bindings write first; the clip wins that frame
  if both drive the same property — documented order):
  1. Recording armed and `frame ∈ [start, start+length)`: append
     `{frame - start, current property value}` to the target track;
     auto-disarm at clip end (deterministic end, no host bookkeeping).
  2. Playback armed and `frame` in range: apply the step-hold value of
     every track (direct application); auto-stop at clip end.
- **Disarm points**: `scrub_to`, `load_document`, `load_replay`,
  `clip_record_stop`, `clip_stop`, and clip removal of the armed clip.
  Rationale: armed state is session-side intent, and a time warp or
  wholesale replace invalidates the frame context it was armed against.
- **Undo across recording**: samples recorded into a track are document
  mutations made OUTSIDE the command stack (driven values). A subsequent
  undo restores the pre-edit document INCLUDING the track as captured —
  documented honestly: recording is not undoable; W1 scrubbing is the
  recovery path (same answer as bindings).

## Replay interplay (W2)

Clip edit commands replay normally. Sample capture is sim-side and is NOT
re-simulated by `load_replay` (a headless re-applier has no sim loop) —
but the closing checkpoint embedded at `stop_capture` is a whole-document
snapshot INCLUDING tracks recorded up to that point, so a replayed session
lands with the recorded content intact. Documented in
`docs/replay-format.md` (v1.8 section + recorded-kinds list update); no
format version bump (readers ignore unknown command names per the forward-
compatibility rule, and old readers simply never see these).

## Viewport `main.cpp` delta (phase A)

**Zero.** The existing `TimelinePanel` (scrub chrome) is untouched; clip
UI arrives as the G3-B module widget. This is the S4 plan's decomposition
promise holding: new editor features land in the module, not in main.cpp.

## G3 phase B (next session, scoped here so phase A doesn't drift)

`ClipTimelineView` in `warploom-editor`: pure projection of (clips,
scrubber frames) to the widget tree — one row per clip, block per clip
(absolute layout), playhead, `clip_at(x)` hit-test for drag-to-move
(committed by the host via the `clip_move` command, keeping command/undo
semantics in the session layer). Module tests prove geometry + hit-tests;
viewport integration replaces nothing (it ADDS a second strip above the
W1 scrub strip).

## Risks / open questions

1. **Serializer churn**: schema bump touches the most sensitive code in
   the engine (byte-determinism). Mitigation: the hash sentinel — the
   deterministic-runtime benchmark and the W1/W2/G1 round-trip tests must
   stay green; clips are written only when non-empty so existing byte
   fixtures do not move.
2. **Recording growth**: unbounded samples on a long armed record. v1
   bound: a clip records only within `[start, start+length)` and
   auto-disarms at the end — the clip's own length is the bound.
3. **Wire-parity test**: new kinds must appear in `kind_names()` AND have
   parse cases; the existing test fails otherwise (by design).
4. **Headless proof**: `omnicpp_headless_host` must call
   `tick_timeline` per `step` (its only frame source) so the g3 live proof
   can record constant-per-step samples headlessly.

## Sequencing within this session

1. Commit this plan (gate-first discipline).
2. Core: document v3 (structs, commands, serializer), protocol v1.8
   (kinds, parser, kind_names), session arm/tick/disarm, recorder
   recorded-kinds update.
3. Tests: document round-trip + undo/redo + record/playback determinism +
   protocol paths (module test binary), wire-parity stays green.
4. Headless host tick + `live_proof.py` g3 section; run all proofs.
5. 6/6 × 4 legs; roadmap checkbox; replay-format.md + control_server
   comments in the same commit.
