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
- [x] **B2 compose chain runs on the render graph** — `record_compose_chain`
      declared each stage as a `GraphPass` and let `compile_graph` compute the
      barriers, replacing ~90 lines of hand-written layout tracking. That
      tracking needed a member (`hdr_layout_`) re-seeded in `record_commands`
      after every scene pass, because the compiler had no way to know an image
      was produced outside the graph. Fixed properly: `GraphSampledImage` now
      carries `initial_layout`/`initial_access`/`initial_stage`, mirroring
      `GraphImageUse`, so an external producer is a declared fact rather than a
      hand-maintained guess. `record_fullscreen_draw` became `static` so the
      chain can record from a capture-free lambda.

      Verified: identical no-HDR capture (mean 78.601, max 110), 0 diagnostics
      in five configurations (hdr, bloom, rt, rt+bloom, no-hdr), 546 tests, 0
      leaks, 64 live proofs.

      Not a defect, to be clear about the division of labour: the renderer
      opening the scene pass and the application supplying only scene data is
      the *goal* of engine/app separation, not work left over. `record_pbr_frame`
      is the single-entry-point variant for callers that want to own pass
      framing themselves; the viewport deliberately does not use it, because it
      needs a shadow pass between the graph's halves and per-scene work
      (node-editor paint, city actors, document cubes) inside the main pass.

      The one genuine leftover is that the scene pass still hand-writes its own
      UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL barrier for the HDR target, so the
      frame is two graphs with a manual seam between them rather than one.
      Closing it means making the scene pass a node in the same sequence as the
      compose stages, which also retires the `initial_layout` seeding -- once
      the producer is in-graph the compiler tracks the image itself. Left for a
      separate change: it touches the frame loop's pass framing, and it deserves
      its own verification run rather than riding along on B2.
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
- [x] **B5 H-Z depth source** — the reduction read
      `render_pass_resource_->depth_image()` unconditionally. Under compose
      that is the swapchain's depth, which the scene never wrote: the scene
      renders into the HDR intermediate's own depth. So H-Z under compose
      would have reduced a stale attachment and published a pyramid that does
      not describe the frame. `select_hiz_depth_source` now picks the depth the
      scene actually wrote, and reports unavailable rather than falling back to
      the stale one. Covered by `SelectsTheDepthTheSceneWrote`, which fails if
      the fallback comes back.

- [ ] **B5b an occlusion consumer** — the pyramid is still built and thrown
      away. There is no occlusion shader that matches the viewport's *split*
      cull ABI: `cull_and_draw_lod.comp` (what the app uses, via
      `record_gpu_driven_cull`) has no pyramid binding, and the two shaders
      that do read occlusion do not fit —
      `cull_and_draw_lod_occlude.comp` is fused cull+draw with the pyramid in a
      buffer at set 0 binding 4, and `cull_hiz_sampled.comp` is a standalone
      test ABI (binding 0 instance spheres, 1 pyramid sampler, 2 draws) used
      only by `test_hiz_sampled_pingpong.cpp`.

      Closer than it looks, though. `cull_and_draw_lod_occlude.comp` is
      compute-only (no drawIndirect), so it is already a cull shader, and its
      payload ABI matches the app's: both read object 0's sphere at word 18 with
      24 words per object, and the app's `cull_and_draw_lod.comp` selects LOD
      from that same sphere. The one real mismatch is binding 0: the occlude
      shader declares it as a `Header` of counters (`[0] draw_word,
      [1] visible_word`), while the viewport shares ONE set layout between cull
      and draw, in which binding 0 is the shared vertex-pull buffer. The app
      would need a separate cull-only layout -- binding 0 counters, binding 4
      pyramid -- rather than sharing.

      So B5b is roughly: a second cull descriptor layout, binding the previous
      frame's pyramid at binding 4, `enable_hiz` on, and the pyramid build
      wired into the frame. Verification is also not a byte-exact A/B: correct
      occlusion legitimately removes hidden geometry, so the evidence has to
      be the existing `test_gpu_driven_occlusion_frame.cpp` approach (occluder
      present -> occludee culled; occluder removed -> occludee drawn) rather
      than pixel equality against the non-occluded frame.
- [x] **B6 no duplicated Vulkan in the app** — raw `vkCmd*` calls in the
      viewport fell from 31 to 10. All ten are legitimate application
      orchestration: ray-tracing acceleration-structure build barriers, the
      one-time neutral shadow-map clear, capture readback barriers, and the
      begin/endRenderPass framing around the engine's calls. The stricter
      reading of the gate — no `vkCmd` at all outside `modules/render` —
      would move the pass framing and the capture path into the engine too.

## Phase C — one data model, one tick

- [x] **C1 document → ECS projection** — `DocumentProjection`
      (`modules/core/include/warploom/core/document_projection.hpp`) projects a
      `SceneDocument` into a `World`: one entity per transformable object,
      carrying `DocumentTransform`, a `DocumentRef` back to the authored
      object, and `TimelineDriven` when a clip writes one of its transform
      channels. The document stays authoritative — undo/redo/save all still go
      through it — and `project()` is what carries edits into the runtime store.
      It is idempotent, preserves entity identity across edits, destroys
      exactly the entities whose objects left, and reports unprojectable
      objects rather than dropping them silently.

      The viewport now builds its scene list from the ECS instead of
      re-deriving a matrix per object per frame, so the ECS has its first
      production consumer and the document has exactly one path to the screen.
      Verified: 100.0000% of pixels byte-identical against the pre-C1 renderer
      (mean 78.472, max 110), 9 projection tests, 556 total, 0 diagnostics, 0
      leaks, 6/6 ctest, 64/64 live proofs, five viewport configurations clean.
      Removing the identity store, the orphan sweep, or the transform refresh
      each fail tests; so does restoring the stale-depth fallback from B5.
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
