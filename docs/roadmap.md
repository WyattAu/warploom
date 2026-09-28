# Warploom roadmap

Status markers: `[x]` done, `[~]` in progress, `[ ]` planned. Every item
follows the project's verification discipline (bottom of this page) — a
milestone is not done until its proof is machine-checked and documented.

## Where we are

S1 done: `warploom-ui` is a real installed package. S2 phase A done:
`warploom-core` (the determinism substrate: runtime, document/JSON, ECS,
node graph, scrubber/recorder, protocol/session) is the second one. S4 done:
`warploom-editor` (the widget/view layer: node editor, inspector,
graph→animation bridge) is the third.
A verified vertical
slice through render / simulate / edit / verify:
deterministic runtime with replay + state hashing, archetype ECS,
deterministic physics, node graph (20+ node types) with graph→property
bindings driven live, protocol v1.5 editor with unified undo/redo,
PBR/IBL/shadows/bloom/LOD/occlusion + GPU-driven draw path, ray tracing
(shadows, reflections, loop path tracer, animated TLAS) proven on
hardware, glTF/GLB/KTX2 import, 584-test hardware suite plus
Clang/TSan/ASan-UBSan legs, scale benchmark (10k instances, ~100 FPS).

The engine is named Warploom (see `rename-and-modules.md`). Nothing is
published yet; the viewport is one large `main.cpp`.

W1 done: replay scrubbing is real — hash-verified checkpoints of the whole
document, protocol v1.6, a timeline strip in the viewport, and a live
socket proof that a time warp returns state to the checkpoint exactly.

G1 done: documents round-trip losslessly through the protocol — save is
atomic, load restores the saved bytes, and undo cannot cross either a load
or a time-warp boundary.

## S — Module split (committed plan in `rename-and-modules.md`)

- [x] **S1 `warploom-ui` pilot** — extracted `ui/` + `editor/glyphs` as a
      CMake package (`WarploomUIConfig.cmake` + version file), own test
      target, header allowlist, namespace `warploom::ui` (was
      `omnicpp::ui`), includes under `warploom/ui/`. Verified in-tree
      (4/4 suites on every CI leg) AND standalone: install +
      `find_package(WarploomUI CONFIG REQUIRED)` + linked consumer
      binary runs.
- [x] **S2 `warploom-core` (phase A)** — extracted the determinism
      substrate as the second real module (`Warploom::core`, own package
      config + version file, own test target, 22-header allowlist).
      Gate first: `docs/warploom-core-plan.md` (measured audit: 11 TUs,
      84 include sites, zero render/ui leakage; key finding —
      `omnicpp::editor` is NOT a pure core namespace, so the namespace
      move is deferred to phase B with the S-decision pass, and the
      truthful editor split becomes an S4 prerequisite). Phase A kept
      identifiers stable: compat forwarders under `include/engine/core/`
      left all 84 include sites compiling unchanged. Verified: 5/5
      suites on all four CI legs (20 total, incl. the new
      `WarploomCoreTests`), standalone install +
      `find_package(WarploomCore CONFIG REQUIRED)` consumer runs
      (`CORE_CONSUMER_OK`), the S1 ui-consumer proof still passes, and
      37/37 live-proof assertions on hardware.
- [ ] **S2 phase B: identifier migration** — `warploom::core` /
      `warploom::editor` namespaces in core headers + compat aliases,
      wired to the S-decision pass so external adopters see one
      coherent identity change. Phase A gate (green four-leg matrix) is
      already banked.
- [x] **S4 `warploom-editor`** — the widget/view layer over core: node
      editor, inspector, graph→animation bridge (session/control
      server/protocol are already core — the S4 plan re-scopes this).
      **Came third, before S3** (decided): the hot development path
      (W/G/protocol work) lands in this module, so future features stop
      growing the viewport `main.cpp`. Gate first:
      `docs/warploom-editor-plan.md` (measured audit: 3 headers + 2 TUs,
      6 consumers, zero render leakage; the truthful
      `omnicpp::editor` map is 7 core headers vs 3 widget headers — and
      corrects the S2 audit: the parallel recorder never declared
      `omnicpp::editor`). Main.cpp decomposition is deliberately narrow
      (delete nothing yet — the real decompression arrives with G3/G5
      landing as module features). Verified: 6/6 suites on all four CI
      legs (incl. the new `WarploomEditorTests`), standalone install +
      `find_package(WarploomEditor CONFIG REQUIRED)` consumer runs
      (`EDITOR_CONSUMER_OK`: graph→cards→paint + inspector projection;
      core/ui consumer re-proofs pass against the same install tree),
      and 37/37 live-proof assertions. Extraction lesson banked: module
      headers include only the modules' own include roots
      (`warploom/core/*`), never the superproject's forwarders —
      forwarders are superproject-side convenience, not package API.
- [ ] **S3 `warploom-render`** — Vulkan context/renderer, render graph,
      scene structures. Runs after S4, against a stable editor boundary.
- [ ] **S5 `warploom-engine` aggregate** — viewport and tests move to
      `find_package`; one-liner adoption works. The moment the monolith
      stops being load-bearing.
- [ ] **S-decision: identifier migration** — `OMNICPP_*` env/option flags
      and `omnicpp_*` binary names to `WARPLOOM_*`/`warploom_*` *before*
      the first external publish (with a compatibility shim), so early
      adopters never see a breaking rename. Same pass renames the GitHub
      repo (`OmniCPP-template` → `warploom`).

## W — Determinism as product ("time warp")

The differentiator. The name promises steppable, rewoven time; this track
makes it visible.

- [x] **W1 replay scrubber** — `ReplayScrubber` checkpoint ring (default
      4096 frames, same-frame recapture replaces, oldest evicted) + protocol
      v1.6 (`scrub_start`/`scrub_to`/`scrub_info`) + timeline strip in the
      viewport (click a checkpoint to warp). Restore is provably lossless:
      bytes re-hash to the capture-time hash AND the parsed document
      re-serializes byte-identically. Undo cannot cross a time warp.
      Verified: 8 unit tests (incl. graph-bearing-document restore regression
      and wire-payload parser coverage), a 23-assertion live socket proof
      (`scrub_to(999)` rejected; post-warp `list_objects` byte-equal to the
      baseline), 4/4 suites on all four CI legs.
- [x] **W2 protocol record/replay** — format spec first
      (`docs/replay-format.md`, `warploom-replay-v1`): JSONL, header +
      positional-args command log (echoes the parsed command generically,
      so recording cannot drift from the parser) + two-line checkpoint
      records (marker + raw document bytes); density settled as
      capture-start + explicit snapshots (`scrub_start` targets, stop) —
      no periodic auto-capture in v1. Protocol v1.7
      (`start_capture`/`stop_capture`/`capture_status`/`load_replay`);
      load = hash-verified hydration + opening-checkpoint restore +
      seq-order re-apply (not re-recorded; truncated files rejected).
      Verified: 4 recorder unit tests incl. fresh-session byte-identical
      reproduction, wire-parity audit walking the now-PUBLIC kind table
      (`ControlCommand::kind_names`, single source of truth for the
      parser), checked-in `tools/live_proof.py` running w1 (10), g1 (8)
      and the W2 exit proof (19): record on instance A → load into a
      fresh instance B → byte-identical wire state + hydrated timeline →
      B warps back to the opening checkpoint. 4/4 suites on all four CI
      legs. Live proof caught: host-handled `step` was invisible to the
      session recorder (now `record_external`), and a char-vs-string
      ternary corrupted the numbers array in records.
- [ ] **W3 graph-triggered replay events** — node outputs mark scrub
      points / trigger on replay (e.g. pulse node tags "collision frame").

## G — Editor depth

- [x] **G1 document save + round-trip** — protocol `save_document`/
      `load_document` (v1.4): byte-deterministic payload, atomic
      tmp+rename write, load replaces the document and clears history.
      Verified: new protocol-path unit test (save → mutate → load restores
      the saved bytes; selection resets; undo fails across the load
      boundary; missing file leaves state untouched) plus a 16-assertion
      live socket proof ending in load → save → sha256 byte-compare;
      4/4 suites on all four CI legs.
- [ ] **G2 multi-select, copy/paste of node subgraphs** — command-based,
      undo-able, preserves bindings.
- [x] **G3 timeline clips (core)** — clips as document entities: schema
      v3 (`clips` + `next_clip_id`, paired-key and cross-ref validation,
      v2 documents load untouched), protocol v1.8 (`clip_add`/
      `clip_remove`/`clip_move` document edits on the undo stack;
      `clip_record`/`clip_record_stop`/`clip_play`/`clip_stop` session
      arm state; `clips_info` query). Session tick contract: one tick =
      `sync_graph()` then `tick_timeline(frame)` — playback first (step-
      hold every playing track), then record (sample the current doc
      value, so same-track chains compose); both auto-disarm at the clip
      end and every warp/disarm point is tested. Axis-suffix properties
      (`position.x`) record and play one vec3 component. Replay stays
      `warploom-replay-v1`: clip commands are recorded, and the loader
      re-executes the tick contract per recorded `step` (documented in
      `docs/replay-format.md`). Panel widget: G3-B below. Verified: 8 new timeline
      unit tests (byte round-trips, reader strictness, undo/redo identity,
      record→move→play determinism incl. replay round-trip, recorded
      clip edits), full core-suite sanity (`warploom_core_tests` 10/10,
      wire-parity/document/replay/scrub filters 32/32), `all` live proof
      extended to 64 assertions (w1 10 + g1 8 + w2 19 + g3 27: record
      through step ticks, mid-recording edit captured, play-silence
      window, auto-disarm at clip end, clip_move + step-hold replay,
      warp disarms both arms; standalone `g3` on a fresh host = 27/27),
      6/6 suites on all four CI legs. Live proof caught: the harness (not
      the engine) assumed spawn ticks the host frame — only `step` does.
- [x] **G3-B clip timeline panel widget** — `ClipTimelineView` in
      warploom-editor (gate doc `docs/warploom-clip-timeline-view-plan.md`):
      pure projection of (document clips, playhead) to the widget tree,
      one block per id-ordered clip over a host-owned frame window,
      clamped slivers for out-of-window spans, playhead painted last,
      sync diffs by span so playhead-only updates are field writes.
      `clip_at` hit-test + drag state machine commits the exact ClipMove
      command payload (offset-preserving, start clamped at 0); the view
      never mutates the document. Verified: 5 new module tests (39/39 in
      `warploom_editor_tests`), 6/6 suites on all four CI legs, and the
      standalone `find_package(WarploomEditor)` consumer re-proof now
      also drives the strip through a session `clip_add` and a block
      hit-test (EDITOR_CONSUMER_OK + G3B_CONSUMER_OK against the freshly
      installed package).
- [ ] **G4 script node** — the script-module C ABI as a node type:
      extend the graph without recompiling.
- [ ] **G5 asset browser** — browse/drop glTF assets; import path is
      already proven end-to-end.

## R — Rendering app-facing completion

- [ ] **R1 viewport path-tracing mode** — the proven loop-PT as an
      optional render mode: progressive accumulation, reset on camera
      move, quality/time budget control. Offscreen proof exists (`E4/E5`).
- [ ] **R2 RT reflections / AO toggles** — reflections proven offscreen;
      wire into the composed path alongside `OMNICPP_RT_MODE=1` shadows.
- [ ] **R3 GPU-driven for skinned scenes** — driven path is cubes-only;
      extend to mannequin, and measure the per-draw vs. driven crossover
      that the E-phase benchmark scenarios promised.
- [ ] **R4 exposure/bloom protocol controls** — finish the remote-tuning
      surface for post-processing.

## P — Platform & packaging

- [x] **P0 CI live proofs** — REVISED after audit: the Actions matrix
      already existed and covered all four presets headlessly (Clang, TSan,
      ASan-UBSan, and the Vulkan leg on lavapipe software Vulkan) plus
      docs. Landed: a headless control host (`omnicpp_headless_host`,
      EditorSession + ControlServer, no GPU/window, built on every leg)
      and a `live-proof` CI job running the full checked-in harness
      (`tools/live_proof.py`: w1 10 + g1 8 + w2 19 assertions) over real
      sockets — 37/37. Remaining option: a self-hosted hardware runner
      for true-NVIDIA proof (nice-to-have, not gating).
- [ ] **P1 WASM leg green** — software rasterizer is deterministic;
      headless WASM CI target.
- [ ] **P2 native Wayland surface** — XCB today; surface creation is
      already platform-split.
- [ ] **P3 one-liner adoption** — FetchContent/vcpkg registration after
      S5; CPack release artifacts per module.
- [ ] **P4 docs site rebrand** — mkdocs tree under the new name.

## The 0.1 gate

**Warploom 0.1 is publishable when all four hold:**

1. **Clone-and-build** — from a fresh clone, the README's build steps
   produce a working viewport on this hardware with no manual steps.
2. **Modules real** — S5 done: the viewport and tests consume
   `find_package(Warploom …)`; no monolithic engine target required.
3. **Demo story works** — record a session (W2), replay it and scrub
   anywhere in it (W1), save/load documents losslessly (G1), all proven
   over the live protocol.
4. **Signature render feature** — R1 path-tracing toggleable in the
   viewport.

It commits to a scope, not a date. Everything else (G2, G4, G5, R2–R4,
P1–P4) is post-0.1 by definition.

## Sequencing rationale

1. **S1 done** — packaging machinery while boundaries were freshest; the
   process is validated (in-tree + standalone consumer proof).
2. **W2 done; S2 next** — W2 completed the W-track's story arc (record
   a session, scrub anywhere in it) and its replay file gave the module
   boundary another real consumer. **S2 `warploom-core` follows now**:
   foundation-first, while the monolith is still young enough to move.
   S2 starts with a written dependency analysis (what lives in core:
   runtime, replay/scrubber/recorder, hashing, ECS, document — and where
   the node graph and physics land) before any target moves.
3. **S4 before S3** (decided) — extract where development is hottest so
   new features land in the module, not deeper into the monolith; the
   editor is headless-testable, which de-risks extraction.
4. **G3 after S4; then S3 → S5** — the timeline core (clips, protocol,
   session tick contract) landed in the editor module post-S4, as
   planned, and the visual panel (G3-B: ClipTimelineView) followed in
   the same module. Render extraction runs against a stable editor
   boundary; S5 flips the viewport to `find_package` and the
   S-decision (flags, binaries, repo rename, with shim) rides with it.
5. **P0 (CI) alongside W2** — the proof harness and automation land
   together; the remaining P items (WASM, Wayland, adoption, docs)
   come after S5, targeting published modules, not the monolith.

## Non-goals (for now)

Multiplayer/rollback netcode; audio device backends (the audio manager
stays as-is); mesh-shader / bindless experiments beyond current hardware;
mobile; ABI stability promises before S5 completes.

## Verification discipline (every item, every track)

- Four-leg matrix green (hardware GCC/Vulkan, Clang headless, TSan,
  ASan-UBSan) with `TMPDIR=/home/wyatt/.cache/omnicpp-tmp`.
- New feature ⇒ new unit/GPU test + scenario-runner coverage where it
  touches the viewport; telemetry-analyzer gates extended rather than
  bypassed.
- Live hardware proof for anything protocol- or viewport-facing, via a
  checked-in socket harness (`tools/live_proof.py`) extended per feature
  rather than rewritten — proofs are repeatable, not throwaway scripts.
- Protocol invariant: every command `Kind` in the mapping table has a
  `parse_command` case and round-trips over the wire — the generic form
  of the W1 parser gap, enforced by a test, not by memory.
- `rendering-status.md` claims table updated in the same commit.
