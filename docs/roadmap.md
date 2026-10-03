# Warploom roadmap

Status markers: `[x]` done, `[~]` in progress, `[ ]` planned. Every item
follows the project's verification discipline (bottom of this page) — a
milestone is not done until its proof is machine-checked and documented.

## Read this first

The capability list in `rendering-status.md` separates *in the engine*,
*reachable from the app*, and *what proves it*. Three findings from that
audit shape everything below, and they were true while every S-track item
was marked done:

1. **The app does not use the renderer module.** `record_pbr_frame`, the
   render graph, H-Z occlusion, offscreen targets and the whole
   bloom/tonemap chain are implemented and tested, and have no production
   caller. `examples/viewport/main.cpp` hand-rolls its own Vulkan.
2. **There is no single data model.** The ECS has no production consumer;
   the document, `VulkanScene` and `PhysicsWorld` are three private worlds.
3. **Determinism stops at physics.** The viewport steps physics in the
   wall-clock frame loop, so replay cannot reproduce it.

Phase A fixes the substrate so those are addressable; Phases B–D use it.

## Phase A — substrate (done)

- [x] **A1 shaders are a build product** — moved out of `tests/` to a
      root-scope `warploom_shaders` target driven by a `CONFIGURE_DEPENDS`
      glob; the viewport depends on it and receives its output directory as
      a compile definition. 690 lines of hand-written rules deleted. Fixed a
      real latent bug: the RT shaders could not compile under the
      `glslangValidator` fallback, which is what CI installs.
- [x] **A2 the clone-and-build gate** — `cmake --preset default` aborted on a
      hardcoded `/nix/store` `Qt6_DIR`. Fixed, and CI now runs it. The
      viewport builds on the RTX 2060 and renders with no manual step.
- [x] **A3 identity debt closed** — 339 include sites re-badged off the 58
      `engine/*` compat forwarders, which are deleted. S5-B phase 3
      dual-exports the script-module C ABI.
- [x] **A4 validation is actually clean** — three defects the status page
      claimed were absent: an inert shadow depth bias (a VUID and a no-op),
      a fragment output the shadow pass does not have, and
      `VulkanOffscreenTarget` leaking every handle it created. Now 0
      diagnostics and 0 leaks on hardware.
- [x] **A5 quality gates run** — the lint and format targets passed
      nonexistent globs to clang-tidy and covered only directories that no
      longer hold code; `.clang-format` could not be parsed at all, so
      `format-check` had never run. Both fixed. 780 unknown-warning-option
      warnings eliminated by probing each flag against the real compiler.
- [x] **A6 dependencies** — six declared packages, one real. Five phantom
      packages are gone, along with Conan.
- [x] **A7 packaging and release** — `cpack` aborted on a template
      placeholder GUID and three missing artwork files. It now produces a
      353-file package; `release.yml` drives CMake instead of the retired
      Python controller.
- [x] **A8 CI builds the viewport** — every job previously set
      `WARPLOOM_BUILD_EXAMPLES=OFF`.

Remaining known debt, deliberately not hidden: `format-check` fails,
because 194 of 195 files do not match `.clang-format`; and
`WARPLOOM_WARNINGS_AS_ERRORS` defaults OFF against ~270 real warnings.
Both are the next campaigns, not this one.

## Phase B — one render path

The largest single engineering task, and the one that makes every
"test-only" row in `rendering-status.md` a shipped feature instead of a
test artifact.

- [ ] **B1 viewport through `record_pbr_frame`** — move the shadow pre-pass
      and main pass off the hand-rolled `vkCmd*` calls, keeping pixel-parity
      tests as the safety net.
- [ ] **B2 render graph in the app** — computed barriers instead of manual
      ones.
- [ ] **B3 HDR target + post chain** — the app currently renders straight to
      the swapchain, so highlights clip. Brings ACES, FXAA and bloom from
      test-only to shipped.
- [ ] **B4 the module's GPU-driven frame** — the app has a parallel
      hand-rolled copy; converge on `record_pbr_frame_gpu_driven`.
- [ ] **B5 H-Z occlusion on** — `enable_hiz` is currently only ever set in
      tests.
- [ ] **B6 no `vkCmd` outside `modules/render`** — the gate.

## Phase C — one data model, one tick

- [ ] **C1 document → ECS projection** — the document stays the authored
      truth; the ECS becomes its runtime projection, so renderer and physics
      read the same store.
- [ ] **C2 a single `tick(FrameInput)`** — `sync_graph → physics.step →
      timeline.tick → record`, called identically by the app and by the
      headless host.
- [ ] **C3 physics in the tick, and in the protocol** — so replay captures
      it.
- [ ] **C4 delete the test-local ECS bridge** — `test_physics_ecs_bridge`
      currently proves a helper that exists only in that file.

## Phase D — the three thin products

- [ ] **D1 real physics** — extract `warploom-physics`; broadphase, shapes,
      joints, XPBD substepping, speculative contacts; deterministic by
      construction. Today: 204 lines, spheres, O(n²).
- [ ] **D2 real timeline** — interpolation and easing, concurrent record and
      play, camera/light/render tracks, and `ClipTimelineView` actually in
      the app. Today playback steps between keys and the widget has no
      caller.
- [ ] **D3 game depth** — ECS as the app's data model, scene management,
      **G4 script node**, G2 subgraph copy/paste, G5 asset browser.

## Phase E — release

- [ ] **E1 render modes as protocol commands** — `WARPLOOM_RT_MODE` is read
      once at start-up, which is the whole reason R1/R2 are unstarted.
- [ ] **E2 R1–R4** — viewport path tracing, RT reflections/AO toggles,
      GPU-driven skinned scenes, exposure/bloom controls.
- [ ] **E3 hardware CI runner** — every RT test skips on lavapipe, so the
      best subsystem has the least regression protection.
- [ ] **E4 warning debt, then `-Werror`** — ~270 warnings, then
      `WARPLOOM_WARNINGS_AS_ERRORS=ON` in CI.
- [ ] **E5 formatting** — apply `.clang-format`, then `format-check` gates.
- [ ] **E6 P1–P4** — WASM leg, native Wayland surface, adoption
      (FetchContent/vcpkg), docs rebrand, GitHub repo rename.

## The 0.1 gate

**Warploom 0.1 is publishable when all four hold:**

1. **Clone-and-build** — a fresh clone reaches a working viewport with no
   manual steps. **Met** (A2), and now CI-enforced.
2. **Modules real** — the viewport and tests consume installed packages; no
   monolithic target. **Met** (S5, A3).
3. **Demo story works** — record, replay, scrub, save/load, proven over the
   live protocol. **Met** for everything except physics (C3).
4. **Signature render feature** — R1 path tracing, toggleable in the
   viewport. **Not met** (E1, E2).

It commits to a scope, not a date.

## Non-goals (for now)

Multiplayer/rollback netcode; audio device backends; mesh-shader/bindless
experiments beyond current hardware; mobile; ABI stability promises.

## Verification discipline (every item, every track)

- Four-leg matrix green (hardware GCC/Vulkan, Clang headless, TSan,
  ASan-UBSan) plus the `presets` and `examples` jobs.
- New feature ⇒ new unit/GPU test + scenario coverage where it touches the
  viewport; `scripts/analyze_telemetry.py` gates extended, not bypassed.
- Live hardware proof for anything protocol- or viewport-facing, via the
  checked-in `tools/live_proof.py`, extended per feature rather than
  rewritten.
- Full suite under `VK_LAYER_KHRONOS_validation` must report 0 diagnostics
  and 0 leaked objects. `rendering-status.md` claims table updated in the
  same commit.
