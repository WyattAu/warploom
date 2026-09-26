# Warploom roadmap

Status markers: `[x]` done, `[~]` in progress, `[ ]` planned. Every item
follows the project's verification discipline (bottom of this page) — a
milestone is not done until its proof is machine-checked and documented.

## Where we are

S1 done: `warploom-ui` is a real installed package. A verified vertical
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
- [ ] **S2 `warploom-core`** — determinism substrate. Largest surface;
      land after S1 validates the process. Starts with a written
      dependency analysis (runtime, replay/scrubber, hashing, ECS,
      document in core; node graph and physics boundaries settled on
      paper first; control server stays out — transport, not
      determinism).
- [ ] **S4 `warploom-editor`** — session, control server/protocol, node
      editor, inspector. **Comes third, before S3** (decided): the hot
      development path (W/G/protocol work) lands in this module, so
      extracting it means future features stop growing the viewport
      `main.cpp`. Headless-testable, hence lower-risk than render.
      *Forces* decomposing the 5.3k-line viewport `main.cpp`; budget a
      session for that alone.
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
- [ ] **W2 protocol record/replay** — `start_capture`/`stop_capture`
      protocol commands producing portable replay files (schema named
      `warploom-replay-v1` now, so the identifier decision doesn't leak
      in late): command log + embedded scrubber checkpoints, composing
      with the G1 document format; scrubber can load them. **Next up,
      before S2.**
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
- [ ] **G3 timeline panel** — clips + recorded graph signals; pairs with
      W1/W2 surfaces. **Held until after S4** so it lands in the extracted
      editor module instead of growing `main.cpp` further.
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
2. **W2 next, then S2** — W2 is short and completes the W-track's story
   arc (record a session, scrub anywhere in it — the product pitch), and
   replay files give S2 another real consumer to validate the core
   boundary against. **S2 `warploom-core` follows immediately**:
   foundation-first, while the monolith is still young enough to move.
   S2 starts with a written dependency analysis (what lives in core:
   runtime, replay/scrubber, hashing, ECS, document — and where the node
   graph and physics land) before any target moves.
3. **S4 before S3** (decided) — extract where development is hottest so
   new features land in the module, not deeper into the monolith; the
   editor is headless-testable, which de-risks extraction.
4. **G3 after S4; then S3 → S5** — the timeline panel ships inside the
   editor module; render extraction runs against a stable editor
   boundary; S5 flips the viewport to `find_package` and the
   S-decision (flags, binaries, repo rename, with shim) rides with it.
5. **P after S5** — platform legs and adoption tooling target published
   modules, not the monolith.

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
