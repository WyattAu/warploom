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

The largest engineering task, and the one that makes every "test-only" row
in `rendering-status.md` a shipped feature instead of a test artifact.

Enabling change, landed first: the renderer could bind only one pipeline per
pass, so a scene mixing a rigged actor with static geometry — which is what
the application does — could not be expressed. `ScenePbrObject::skinned`
plus optional skinned counterparts in `VulkanPbrScene` fixed that, with
descriptor rebinding keyed on the pipeline layout (Vulkan drops bound sets
when the layout changes) and the bone set bound only where the layout
declares a slot 3.

- [x] **B1a shadow pre-pass through `record_shadow_pre_pass`** — the
      hand-rolled copy is deleted. shadow.vert's push block was widened to
      144 bytes to match shadow_skinned.vert so one layout serves both.
      Verified: identical shadow-map texel occupancy (1,889,907/4,194,304),
      a uniform depth shift from converging on the engine's bias values, and
      99.99% of frame-30 pixels byte-identical with differences confined to a
      24x85 region at the shadow boundary.
- [x] **B1b lit pass** — already delegated to `record_pbr_scene`; the only
      raw Vulkan left in it was the GPU-driven block.
- [x] **B4 GPU-driven path** — `record_gpu_driven_cull` and
      `record_gpu_driven_draw` added, because the application renders a
      shadow pass between the two halves and cannot use the single-graph
      entry point. GpuDrivenFrame gained an explicit descriptor-slot map
      because the driven path binds 0,1,2 then 4,5. Verified: 921,600 of
      921,600 pixels byte-identical against the previous commit, 0 VUIDs.
- [ ] **B2 render graph in the app** — the app still records into a render
      pass the renderer opened, so the graph's computed barriers do not apply.
      The compose chain now hand-writes its own layout barriers (tracked per
      target); folding those into `execute_graph` is what this item means.
- [x] **B3 HDR target + post chain** — the renderer now owns an HDR
      intermediate plus a bloom and tonemap/FXAA chain
      (`RendererConfig::enable_hdr_compose`), and the application's scene
      pipelines are built for that intermediate rather than the swapchain. The
      app no longer presents clipped linear radiance. Verified under the
      validation layer in eight configurations (default, no-HDR, bloom, RT,
      RT+bloom, GPU-driven, node editor, city), 0 diagnostics. Exposure is
      `WARPLOOM_EXPOSURE`.
- [ ] **B3b frame capture through the compose chain** — the capture
      re-records the scene into its own 8-bit target, which cannot sample a
      float intermediate, so it refuses with a clear message while compose is
      on. A first attempt was reverted: pointing the capture at the renderer's
      intermediates corrupts the live frame, because the bloom upsample writes
      the HDR image. The fix is to make the compose chain instantiable per
      target rather than per renderer (its own intermediate set), plus a
      format-aware readback. Until then, capture runs with `WARPLOOM_NO_HDR=1`.

      Scope measured while attempting it, so the next attempt is sized right.
      The chain is not three self-contained methods: `record_commands()` also
      re-seeds `hdr_layout_` after the scene pass (layout bookkeeping lives
      there, not in the chain), the chain owns its own `VulkanMemoryAllocator`
      and `VulkanDescriptorManager`, and its bloom stages call the renderer's
      private static `create_command_pool`/`allocate_command_buffer` plus the
      nested `FullscreenPass` and `record_fullscreen_draw`. So extraction means
      four things, not one:
        1. Promote `FullscreenPass` + `record_fullscreen_draw` into shared
           infrastructure (both the graph callbacks and the chain need them,
           and neither should own the other's helpers).
        2. Give the chain a queue handle and its own layout tracker, and move
           the post-scene `hdr_layout_` re-seed into `record()`.
        3. Move the chain's allocator/descriptor-manager ownership wholesale
           rather than sharing the renderer's.
        4. Then a second instance is instantiable and B3b follows.

      Note for whoever does it: the project defines `OMNICPP_HAS_VULKAN` with
      no value, so `#if OMNICPP_HAS_VULKAN` silently evaluates false. Use
      `#ifdef`.
- [ ] **B5 H-Z occlusion on** — `enable_hiz` is currently only ever set in
      tests.
- [x] **B6 no duplicated Vulkan in the app** — raw `vkCmd*` calls in the
      viewport fell from 31 to 10. All ten are legitimate application
      orchestration: ray-tracing acceleration-structure build barriers, the
      one-time neutral shadow-map clear, capture readback barriers, and the
      begin/endRenderPass framing around the engine's calls. The stricter
      reading of the gate — no `vkCmd` at all outside `modules/render` —
      would move the pass framing and the capture path into the engine too.

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
