# Loop 23 — first dogfooding consumer (E7, second of the all-directions series)

## Sources

No new technique was adopted, so per AGENTS.md there is no research section
beyond the mechanics. The estate sources are the reusable-workflow vendor
docs (read in Loop 21) and the estate's own kit files (read raw in Loops
21–22). The domain facts below were established by measurement, not by
citation.

## What was built

`WyattAu/warploom-game` (new repo): a minimal headless game — 20 cubes drop
onto the ground plane over 600 fixed steps — depending on warploom as a
package (`FetchContent`, `main`, `WARPLOOM_BUILD_TESTS/EXAMPLES=OFF`,
`WARPLOOM_USE_VULKAN=OFF`) and touching the public API only. The `drop`
binary proves three claims and exits 0 iff all hold: (1) capture → fresh
load reproduces the document byte-identically; (2) two identically built
sessions ticked identically hold bit-identical body positions; (3) the pile
fell (top 5.9 → 3.49) and rests on the plane (bottom 0.498 ≈ radius 0.5) —
a no-op sim passes 1 and 2 vacuously, so 3 is the guard. Its CI calls the
estate `cpp-kit.yml` with the warploom-specific legs disabled and the
examples leg pointed at `drop`.

## Findings (all from the game's probe stage, before the repo existed)

1. **Two install defects** — the first external `find_package` attempt
   failed. `WarploomCoreConfig` referenced the target under its INTERNAL
   name (`Warploom::warploom_physics`): `modules/physics` never set
   `EXPORT_NAME`, the one module split out after the S5 install pass, so it
   never went through it. And the core package config never declared its
   transitive dependency (`find_dependency(WarploomPhysics)`), while every
   sibling config does. Sequential A/B: neither fix → internal-name error;
   EXPORT_NAME only → correct name, still missing; both → configures,
   builds, byte-identical YES. Both fixed in warploom (1 + 4 lines).
2. **A real replay defect: same-frame open/close.** The probe's capture
   drove ticks directly with no Step commands, so the recorder frame never
   advanced and opening + closing checkpoints shared frame 0. Same-frame
   ring insertion replaces newest-wins (documented scrubber semantics), so
   `load_replay`'s ring lookup by frame restored the CLOSING bytes and the
   spawn re-applied on top: 2 objects became 3, then the end-state check
   failed. In-tree tests never hit it because they always Step between
   start and stop. Fix: `ReplayScrubber::restore_bytes` (same two-layer
   verification, no ring lookup) + `load_replay` restores the opening from
   the hydrated bytes. Regression test
   `LoadSucceedsWhenOpenAndCloseShareAFrame`; sabotage (revert call-site)
   fails it. `docs/replay-format.md` load point 2 records the semantics.
3. **Session-level Step is ack-only, by design.** `session.on_control(Step)`
   acknowledges + records; the HOST executes ticks (headless_host runs
   `tick()` per Step, the viewport queues them). The game IS the host: it
   owns a frame counter, executes Steps as ticks, forwards the rest. The
   first probe version called `on_control(Step)` on the session directly
   and "passed" with a sim that never ran — claim 3 exists because of it.

## Novelty assessment

Nothing is novel. A dogfooding consumer per AGENTS.md's rust-in-cpp pattern,
a kit parameterization (12 inputs, warploom's values as defaults), and two
packaging fixes plus one replay fix. The value is evidential: three defects
no in-tree test could find, because in-tree tests never consume the install
and never capture without stepping.

## What was measured

- `drop` from outside the tree: pile top 3.489 (from 5.9), bottom 0.498,
  DROP-OK; replay document byte-identical; C/D positions bit-identical;
  capture perturbs nothing (A == C per-body).
- Game matrix locally: all six presets (default/debug/clang/release/tsan/
  asan-ubsan) configure + build + ctest green; clang-format clean; estate
  contract check agrees on the game caller.
- Warploom full baseline at the loop boundary (user pace): default /
  asan-ubsan / tsan / vulkan-validation ctest 6/6 each, headless live
  proofs 76/76, viewport e1 28/28, `lake build` clean.
- Game CI (GitHub, real external run): the first run failed ONLY on a
  warploom-specific benchmark tail the kit ran unconditionally
  (`warploom_deterministic_runtime_benchmark`, exit 127) — 8/9 other legs
  green. Parameterized as `enable_benchmark` + `benchmark_command`
  (warploom's block is the default); the second run is 9/9 green with 3
  correctly skipped. A kit defect only a second caller could expose.
