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
published yet; the viewport is one large `main.cpp`; document save/load
exists over the protocol but the round-trip proof is still pending (G1).

W1 done: replay scrubbing is real — hash-verified checkpoints of the whole
document, protocol v1.6, a timeline strip in the viewport, and a live
socket proof that a time warp returns state to the checkpoint exactly.

## S — Module split (committed plan in `rename-and-modules.md`)

- [x] **S1 `warploom-ui` pilot** — extracted `ui/` + `editor/glyphs` as a
      CMake package (`WarploomUIConfig.cmake` + version file), own test
      target, header allowlist, namespace `warploom::ui` (was
      `omnicpp::ui`), includes under `warploom/ui/`. Verified in-tree
      (4/4 suites on every CI leg) AND standalone: install +
      `find_package(WarploomUI CONFIG REQUIRED)` + linked consumer
      binary runs.
- [ ] **S2 `warploom-core`** — determinism substrate. Largest surface;
      land after S1 validates the process.
- [ ] **S3 `warploom-render`** — Vulkan context/renderer, render graph,
      scene structures.
- [ ] **S4 `warploom-editor`** — session, control server/protocol, node
      editor, inspector. *Forces* decomposing the 5.3k-line viewport
      `main.cpp`; budget a session for that alone.
- [ ] **S5 `warploom-engine` aggregate** — viewport and tests move to
      `find_package`; one-liner adoption works.
- [ ] **S-decision: identifier migration** — `OMNICPP_*` env/option flags
      and `omnicpp_*` binary names to `WARPLOOM_*`/`warploom_*` *before*
      the first external publish (with a compatibility shim), so early
      adopters never see a breaking rename.

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
      protocol commands producing portable replay files; scrubber can
      load them.
- [ ] **W3 graph-triggered replay events** — node outputs mark scrub
      points / trigger on replay (e.g. pulse node tags "collision frame").

## G — Editor depth

- [~] **G1 document save + round-trip** — protocol `save_document`,
      atomic write, schema-versioned; round-trip proof (load → save →
      byte-compare or schema-equal). Implementation already exists (v1.4:
      atomic `save_to_file`, load replaces the document and clears
      history); the remaining work is the machine-checked round-trip proof.
- [ ] **G2 multi-select, copy/paste of node subgraphs** — command-based,
      undo-able, preserves bindings.
- [ ] **G3 timeline panel** — clips + recorded graph signals; pairs with
      W1/W2 surfaces.
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

## Sequencing rationale

1. **S1 now** — packaging machinery while boundaries are freshest; small
   enough to finish in one pass.
2. **W1 next** — the flagship demo and the name's promise; every later
   milestone (G3, W2, W3) builds on its UI + protocol surfaces.
3. **G1 after W1** — persistence is table stakes for real use.
4. Then interleave G2–G5 / R1–R4 by interest; R3 pairs with the scale
   benchmark follow-up, G4 unlocks user extensibility.
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
- Live hardware proof for anything protocol- or viewport-facing.
- `rendering-status.md` claims table updated in the same commit.
