# warploom T5 plan: camera/light/render tracks + concurrent arms

Status: **gate doc** (spec first, per the §6 survey rule in
`docs/long-term-roadmap.md`). Sibling of `docs/warploom-timeline-plan.md`
(clips core, G3, done). This document is the acceptance authority for T5;
implementation lands against it, one slice at a time, each slice with its
own proof.

Scope in one line: tracks for channels that are **not** an object's Number
or Vec3 axis property (camera pose, light parameters, render settings), plus
the **concurrent arms** contract (record armed while play is armed) stated
precisely enough to test.

## 1. What exists today (measured, not assumed)

- `ClipTrack` is `(object_id, property, samples)` and `TimelineClip` keys
  tracks by `track_key(object_id, property) == "<id>:<property>"`
  (`modules/core/include/warploom/core/document.hpp`).
- `clip_record` **requires** a document object with either a Number
  property or a Vec3 property plus an `.x`/`.y`/`.z` suffix
  (`editor_session.cpp`, `CK::ClipRecord`). Anything else is rejected with
  `"has no recordable"`.
- Playback (`tick_timeline_at`) applies each track to
  `doc_.find(track.object_id)`, optionally writing one Vec3 component.
- Recording samples **after** playback applied, and the code comments say
  so explicitly ("read AFTER playback applied, so clip->same-track chains
  reproduce themselves"). That ordering is a **contract with no executing
  test**: no test arms record and play at the same time (checked: the four
  `ClipRecord`/`ClipPlay` sites in `modules/core/tests/test_timeline.cpp`
  each use one at a time).
- Camera and light are **document objects** with vec3/number properties,
  so camera and light *pose* tracks already work through the object path.
  What does **not** work is anything that is not an object property:
  render settings (exposure, bloom, tonemap/render mode) and scene-level
  camera selection.

So T5 splits cleanly into four slices, ordered by dependency:

| Slice | Channel | Kind | Blocked by |
| --- | --- | --- | --- |
| T5a | Concurrent arms (record + play at once) | contract + test | nothing |
| T5b | Camera/light pose convenience (atomic multi-property record) | session + protocol | T5a |
| T5c | Render-setting tracks (exposure, bloom, render mode) | new channel class + document v4 | T5c design below |
| T5d | Concurrent multi-arm recording (N tracks at once) | session state model | T5a |

## 2. T5a — Concurrent arms (contract, tested)

**Contract.** Record and play may both be armed on the same session. The
tick order is fixed and is the whole contract:

1. `sync_graph` (host, before the tick).
2. Playback applies to the document: for each track in the playing clip
   whose span covers `frame_time`, evaluate and write.
3. Recording samples the **current** document value into the armed track.
4. Physics and other downstream systems observe the result.

Consequences, each of which the test pins:

- A record arm chained on a **different** object/track than the playing
  clip samples values the playback just wrote — i.e. recording can capture
  a performance, which is the point of the feature.
- A record arm on the **same** track as the playing clip re-records the
  played value (sample = played value at that frame), not the value from
  before playback. A clip chained to itself is therefore idempotent, not
  divergent.
- Arming order within a frame is irrelevant: both arms are state, and the
  tick order decides. (There is no "arm came second" special case.)
- Disarm at clip end is per-arm: playback ending does not stop recording,
  and recording ending does not stop playback.

## 3. T5b — Camera/light pose tracks

Camera and lights are objects, so the storage stays object-property shaped;
what is missing is **atomic multi-property record**. Requirements:

- `clip_record_all <clip> <oid>` (protocol v1.12): arm one clip+object pair
  and sample **every** recordable Number property and every Vec3 (as three
  axis tracks) on that object each tick. One arm, N tracks.
- Tracks stay independent (a later single-property re-record edits one
  track, never the set).
- Sample counts per track are equal at equal ticks, but the reader must not
  require it (a track may start later if the property was created later).
- Recording a property that did not exist on the first sampled frame starts
  that track at the first frame it exists, not at offset 0.

## 4. T5c — Render-setting tracks (design)

The honest boundary: render settings are **not** document properties today
(`RendererConfig` lives in the renderer; exposure is written through
`set_exposure`). Two options, and the choice is recorded here so it is not
re-litigated:

- **(chosen) Scene channel object.** Introduce a single reserved document
  object `render_settings` (type id reserved, created on demand by the
  first `clip_record` on a render channel) holding the trackable settings
  as ordinary number properties: `exposure`, `bloom`, `render_mode`,
  `tone_map`. The session maps those properties into the renderer config on
  tick (through the same projection that already carries document state to
  render systems), so tracks need **no new storage, no schema bump, and no
  new evaluator**.
- (rejected) A second channel class in the document format. It buys
  nothing the reserved object does not, and it costs a schema version, a
  reader branch in every parser, and a second `track_key` grammar — three
  places for a future drift, against zero new capability.

Constraints:

- The reserved object must never appear in the outliner projection
  (`document_projection` filters it) and never be selectable.
- Determinism: the reserved object is created by a command, so replay
  reproduces it; it is not created lazily inside a tick.
- Replay compatibility: documents written before T5c have no reserved
  object and read back unchanged; a track referencing it in a document that
  lacks it is a reader error, not a skip.

## 5. T5d — Concurrent multi-arm recording

Replace `recording_/recording_clip_/recording_object_/recording_property_`
with a small `std::vector<RecordArm>` (clip, object, property optional).
Rules that keep it deterministic:

- Arms are kept in **insertion order** and sampled in that order; two arms
  on the same track are rejected at arm time (`"already armed"`), because
  two writers to one track is a race expressed as data.
- Disarm-by-clip removes the matching arms only.
- `clips_info` reports the arm set sorted by (clip id, object id, property)
  — sorted for output, insertion order for sampling.

## 6. Acceptance (what "T5 done" means)

1. T5a contract test green, with a sabotage that reorders the tick (sample
   before playback) failing it.
2. T5b: one arm records a camera object's `eye`, `target` (vec3) and `fov`
   (number) across a clip; playback reproduces the pose bit-identically and
   survives save/load byte-identically.
3. T5c: exposure and bloom tracks play back onto the renderer config with
   the reserved object invisible to the outliner; a document without it
   round-trips byte-identically.
4. T5d: two arms, two objects, one tick — each track gets its own value;
   same-track double arm rejected.
5. Replay: a recording that includes all four surfaces re-applies
   bit-identically (closing checkpoint hash).

## 7. Progress

- T5a **DONE 2026-11-03**: contract stated above and pinned by
  `TimelineSession.ConcurrentRecordAndPlayArmsBoth` (chained-track capture,
  same-track idempotence, playback-end does not disarm recording) plus
  `TimelineSession.ConcurrentArmsSurviveReplay` (record+play armed in one
  run replay bit-identically). Both are GP-tested (no device).
  - 2026-11-04 maintenance: both tests were RED in the working tree (the
    arming ticks sat past the clips' ends, tripping the correct
    end-of-clip auto-disarm; the replay test additionally relied on
    unlogged rewind ticks, which load_replay cannot reproduce). Fixed by
    arming pre-span (test 1) and a strict tick-then-command monotonic
    rewrite (test 2, mirroring the T4b host_tick+Step pattern). The
    session code needed no change — the tests were wrong, not the
    contract. Recorded here so the shape stays legible.
- T5b **DONE 2026-11-04**: `clip_record_all` (protocol v1.15) arms one
  clip+object pair across every recordable property (Number as-is, Vec3
  as three axis tracks, document order so replays arm identically) on
  the T5d vector model; atomic duplicate rejection. Pinned by
  `TimelineSession.RecordAllCapturesCameraPoseAtomically` (10 tracks on
  the environment object, per-tick pose reproduction, save/load
  byte-identical) plus `RecordAllRejectsBadPayloads` and the wire-parity
  table pin (`clip_record_all` recognized, `clip` required).
- T5d **DONE 2026-11-04**: `record_arms_` vector in insertion order (the
  single-arm fields are gone; `recorded_samples_` was write-only and is
  deleted); same-track double arm rejected at arm time; disarm-by-clip
  removes matching arms only (`ClipRecordStop` still disarms all);
  `clips_info` gains an additive per-clip `"armed"` array sorted by
  (object, property) — the top-level array shape `live_proof.py` parses
  is untouched. Pinned by
  `TimelineSession.ConcurrentRecordArmsAreIndependent` (two arms/two
  objects/one tick, duplicate rejection, disarm-by-clip survival,
  survival sampling, exact `armed` payload).
- T5c **DONE 2026-11-04** (session + host halves): the rejected
  second-channel-class option stays rejected; the reserved-object design
  is implemented as specified. `render_settings` type registered LAST
  (four historical type ids unchanged — documents store type_id
  numerically); `clip_record(All)` with oid 0 resolves bare render
  channels to it, creating it on demand through an undoable spawn (same
  id live and on replay); `render_config()` exposes the opaque numbers;
  destroy-while-referenced is loud; pre-T5c documents (no such object)
  round-trip unchanged. Pinned by
  `TimelineSession.RenderChannelsRecordAndPlayOntoRenderConfig`
  (create-on-arm, 2-track record, tick-by-tick `render_config()`
  playback) and `TimelineSession.RenderSettingsGuardsAndDefaults`
  (destroy guard, bad-oid/bad-key rejection).

  HOST HALF DONE: `RenderConfig` carries a per-channel `*_driven` flag,
  recomputed on EVERY tick from the clips actually playing (never sticky,
  so a stopped clip stops overriding), and `get_render_config` (protocol
  v1.16) reports the values plus the driven channel names — session-owned,
  so both hosts answer identically. The viewport maps only driven channels
  onto `set_exposure` / `set_bloom_enabled` / the render-mode transitions
  each frame, and appends `applied_exposure`/`applied_bloom`/
  `applied_mode` so the host half is observable rather than inferred.
  Applying only driven channels is the contract that keeps a render track
  from clobbering the E1/R4 socket path on every frame; `tone_map` records
  and plays session-side but has no renderer knob yet (stated, not
  silent). Pinned by unit assertions on the driven mask (`render_config()`
  after each playback tick, and all-clear after the span) and live by
  `tools/live_proof.py t5c`: 23/23 headless, 24/24 on the viewport where
  `applied_exposure` reads back the driven 2.5.

  Build note found while proving it (worth keeping): the headless proof
  first SEGFAULTED on `scrub_to`, and the stack was a `PhysicsWorld`
  destructor freeing a garbage pointer. The cause was not that code: the
  `warploom_headless_host` build target is named `omnicpp_headless_host`,
  so the rebuild silently failed ("No rule to make target") and the proof
  ran a Nov 3 binary against a Nov 4 `libwarploom_core.so` — two different
  `PhysicsWorld` layouts in one process, i.e. an ODR violation manufactured
  by a half-built tree. After a correct rebuild, `all` is 103/103 and the
  crash does not reproduce. The rule in AGENTS.md paid for itself: a build
  that fails is evidence of nothing, and a grep for `error:` misses
  `No rule to make target`.

  Final verification 2026-11-05 (fresh full-tree rebuild, zero warnings
  from any TU): full unit suite 738/738 PASSED (includes the two T5c
  session tests plus sabotage-checked guards), ctest 6/6,
  `tools/live_proof.py all` 103/103 on fresh headless hosts, and the
  viewport halves e1 58/58 + t5c 24/24 with 0 error/validation lines in
  the viewport log. The 19 warnings the fresh rebuild surfaced
  (float-equal change detection in `vulkan_path_tracer.cpp` — exact
  comparison intended, now under the established pragma push/pop — plus
  two nodiscard discards, one tautological `d_name` check, and three in
  test files: a promotion, an unused variable, a discarded result) are
  paid, not suppressed.

### Incidental fix (T5b test exposure)

- `SceneDocument::to_json` closed the tracks object per track: byte-
  identical output for single-track clips (all existing goldens), but
  malformed JSON for two or more tracks (the second track parsed back
  as `unknown clip key`). Never caught because every round-trip test
  used one track — the one-size verification lesson again. Fixed (close
  samples+track per entry, tracks+clip once after the loop) and pinned
  by the T5b save/load assertion (10 tracks byte-identical).
