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
- [x] **B3b frame capture through the compose chain** — DONE. The viewport
      creates a SECOND `VulkanComposeChain` when capture is requested and
      compose is on; the capture records the scene into that chain's HDR
      intermediate and the chain tonemaps into the capture's LDR target, then
      readback proceeds unchanged. Depth reads from the chain's scene depth
      (`TRANSFER_SRC` was already in its usage), so color and depth stay
      matched. Measured on the RTX 2060 at 1280x720: frame_30 mean 25.71, max
      188, 15.2% non-black; the LDR path's highlights capped at 110, so the
      composed capture preserves brighter highlights than the old LDR capture
      could -- that is the compose pipeline arriving in the capture, not a
      regression. Depth all-finite 0.0016..1.0. The refusal and the
      `WARPLOOM_NO_HDR=1` workaround are gone. Originally: A first attempt was reverted: pointing the capture at the renderer's
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
        1. DONE — `FullscreenPass`, `record_fullscreen_draw`,
           `record_fullscreen_pass` and `fullscreen_graph_pass` now live in
           `vulkan_fullscreen.hpp/.cpp` as free functions over caller-owned
           handles; the renderer no longer owns them and the nested struct is
           gone. 606 tests green, both compilers, all four presets.
        2. DONE (with 3): everything now lives in `VulkanComposeChain`
           (vulkan_compose_chain.hpp/.cpp) — allocator, descriptor manager,
           HDR/bloom targets, pipelines, samplers, the black fallback, and the
           graph-driven `record()`. Layout tracking was already in
           `compile_graph` (done with B2); the roadmap's `hdr_layout_` note
           predated that. The chain submits on the caller's command buffer, so
           no queue handle of its own was needed.
        3. DONE — see 2. The renderer holds one `std::unique_ptr
           <VulkanComposeChain>` and hands it the presentation pass/format at
           initialize; the chain captures every resource it creates.
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

- [x] **B5b an occlusion consumer** — DONE (increments 1+2 below). Was: the
      pyramid is built and thrown away. There is no occlusion shader that matches the viewport's *split*
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
      wired into the frame.

      Increment 1 DONE: the pyramid is packed after every H-Z build
      (`VulkanRenderer::hiz_packed_pyramid()`), image -> buffer per mip with
      self-contained transitions after the graph, level 0 first at
      monotonically increasing word offsets — exactly the ABI
      `cull_and_draw_lod_occlude.comp` reads. Verified by
      `HiZGraph.PackedPyramidMatchesAbi`: property-based (empty/occupied tiles
      both present, empty parent implies empty children, parent depth >= each
      non-empty child), because recomputing the reduction on the CPU would
      duplicate the shader rather than verify it.

      Increment 2 DONE: the viewport builds the cull-only descriptor layout
      (binding 0 = header counters, NOT the shared vertex-pull buffer;
      binding 4 = the packed pyramid), the 168-byte extended-push pipeline,
      and a `WARPLOOM_NO_MANNEQUIN=1` selector — the bundled asset always
      loaded, so `!has_mannequin` had never opened the gd gate on a machine
      with assets. Live wiring proven decisively: with `near_z` forced to
      0.001 the shader's depth_min clamps to 1.0 and 74.7% of captured pixels
      change versus occlusion off — tiles are populated and culls fire; with
      correct near/far the shipped scene's captured frames are IDENTICAL,
      which is the correct output, not a failure: 400 cubes spread over a
      12x6 m plane, and a conservative max-depth pyramid cannot cull an
      object resting on the same surface its comparison tiles sample. The
      shader-level occluder-present/occludee-culled proof remains
      `test_gpu_driven_occlusion_frame.cpp`; a scene with genuine overlap
      (walls, hills, a dense wall of cubes) is what would show live savings.
      `WARPLOOM_OCCLUSION=0` disables the test at the push for A/B runs.
      Remaining: nothing blocking — an occlusion-heavy demo view is a
      content question, not an engine one. Verification is also not a byte-exact A/B: correct
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
- [x] **C2 one `tick(FrameInput)`** — `EditorSession::tick` runs
      `sync_graph → physics.step → timeline.tick → project`, and the viewport
      and the control host both call exactly that. Neither sequences the stages
      itself any more, so the two cannot drift.

      This fixed a live divergence, not just an organizational one: the control
      host ticked the timeline and the viewport called `sync_graph` only, so a
      clip played in the viewport did nothing while the same document played
      correctly under the host — with a comment in `headless_host.cpp`
      asserting they shared a contract. `TimelineIsDrivenByTheTickNotByTheHost`
      reproduces the old behaviour when the timeline stage is removed.

      The physics stage was deliberately a *counted no-op* when this landed, so
      the ordering would already be fixed and tested before a solver moved into
      it. C3 has since put the solver there. It sits
      after `sync_graph` and is gated on pause, which is why a paused tick still
      applies graph edits and refreshes the projection. Projection runs last so
      the projected store reflects a clip's write rather than the pre-playback
      value.

      Verified: 100.0000% of pixels identical to pre-C1 (mean 78.472, max 110),
      6 tick tests, 562 total, 0 diagnostics, 0 leaks, 6/6 ctest, 64/64 live
      proofs, four viewport configurations clean.
- [x] **C3 physics in the tick and in the protocol** — the session owns a
      `PhysicsWorld` and `tick()` steps it, so both hosts simulate the same
      world by construction rather than by convention. Bodies substep at
      `kMaxPhysicsSubstep` (1/240 s), count derived from `fixed_dt` alone and
      capped at 8: a large frame dt cannot tunnel bodies through each other,
      and a pathological one cannot make a tick arbitrarily expensive.

      Determinism shaped it. The substep count is a pure function of `fixed_dt`,
      never wall-clock, so a replay re-ticking the same frames reproduces the
      integration — asserted by stepping two sessions 20 frames and requiring
      bit-identical positions plus identical snapshot bytes.

      State travels in `snapshot_json()` (gravity plus per-body pose, velocity,
      radius, inverse mass, restitution), in object-id order so the bytes are
      reproducible. Floats use `to_chars`' shortest round-trip form, NOT
      `std::to_string`: six decimals is lossy, so a resumed replay would return
      a body very slightly moved and diverge from the original on the next tick.
      The test parses the JSON back and demands exact equality.

      Poses are written into the projected transform *after* `project()`, which
      refreshes from the document and would otherwise snap every simulated
      object back to its authored position each frame.

      Honest limit: `snapshot_json()` is the live protocol query, so physics is
      *reported*. Scrub checkpoints still capture the document only, so a
      time-warp scrub does not yet restore physics. That is the rest of the
      replay coverage.

      Verified: 100.0000% of pixels identical to pre-C1, 14 tick tests (8 new),
      570 total, 0 diagnostics, 0 leaks, 6/6 ctest, 64/64 live proofs. Removing
      the step, the pose write-back, or the substep derivation each fails
      tests.
- [x] **C4 the test-local ECS bridge is gone** — `test_physics_ecs_bridge`
      defined its own `sync_transforms` and a local `SceneTransform`, then
      asserted the copy worked. That proved a bridge existing only in the test:
      production could change freely and the test would still pass.

      Deleting the file outright would have lost real coverage, so the helper
      went and the assertions stayed, retargeted. The two bridge cases now
      drive `EditorSession`, so they test the code the renderer actually reads.
      The determinism-at-scale case stays at the solver level where the
      property lives — `position_fingerprint` over a thousand bodies needs no
      bridge — and a new case runs the same thousand bodies end to end through
      the production bridge to prove that path is deterministic too.

      `ThousandInstancesDeterministic` takes ~25 s, which is D1 showing
      through: the solver is O(n²) over pairs. It was 26 s before this change,
      so nothing regressed — it got marginally faster by dropping the per-step
      ECS sync. A broadphase is what makes it fast, and that is D1.

## Phase D — the three thin products

- [ ] **D1 real physics** — extract `warploom-physics`; more shapes, joints,
      XPBD substepping, speculative contacts; deterministic by construction.
      Today: ~420 lines in core (deterministic uniform-grid broadphase with
      frozen-snapshot detection, spheres, sequential positional correction,
      single pass).

      **Measured blocker for the broadphase.** A conservative uniform grid was
      written, verified result-preserving at N=1000 (fingerprint identical,
      1.05x), and then found *not* result-preserving at N=4000 and N=8000 —
      which is why it is not committed. Asymptotics were fine (1.05x → 1.67x →
      3.65x as N grew), so the design was sound; the semantics were not.

      The cause is the solver, not the grid. Step 3 resolves contacts in a
      single pass that **mutates positions as it goes**: for each pair it
      applies a positional correction before moving to the next pair. A
      candidate list built from positions at pass start therefore omits pairs
      that only come into overlap *because* an earlier pair pushed a body into
      them — and the all-pairs loop would have caught those, since it re-reads
      every position at the moment it visits the pair. At N=1000 the density is
      low enough that this never triggered, which is exactly why a single-scale
      check would have shipped it.

      So no static broadphase can be made bit-identical to *that* solver; the
      candidate set would have to be "every pair that overlaps at any point
      during the sequential pass", which depends on the resolution order that
      produced it.

- [x] **D1a order-independent contact detection** — step 3 now detects against
      one frozen snapshot of positions, velocities, radii and masses, then
      applies corrections in pair order. Freezing *detection* is what makes the
      pair set a pure function of the snapshot, which in turn is what makes a
      spatial grid exactly conservative. Corrections still apply in pair order
      (Gauss-Seidel, converges better than a simultaneous solve); what is fixed
      is which pairs get solved, not the order their corrections land in.

      The uniform grid then works, and agrees with all-pairs at every scale
      measured — 200, 600, 1200, 1000, 4000 and 8000 bodies — where the
      pre-snapshot version diverged at 4000 and 8000. Speedup over all-pairs
      of the same solver: **2.10x at 1k, 2.89x at 4k, 2.92x at 8k**.

      Note these are *different results* from the old solver, not the same
      results computed faster, and that is the point: this is a better solver,
      not a cheaper old one. Fingerprints are stable run to run, which is what
      replay needs; they are not expected to match a pre-D1 recording.

      `set_force_all_pairs` exists so the equivalence is checkable from a test
      rather than by rebuilding with the threshold edited by hand —
      `BroadphaseAgreesWithAllPairsAtSeveralScales` is exactly the comparison
      whose absence let the first attempt through. Overflow past
      `kBroadphaseMaxCandidates` falls back to the complete pair list rather
      than dropping pairs, which was the old version's failure mode.

      Iterated positional correction landed: `set_solver_iterations(n)` runs
      n Gauss-Seidel passes over the FROZEN contact list (the set stays a pure
      function of the snapshot; later passes re-read live positions, which is
      what pushes out residual overlap earlier passes created). Verified by a
      6-body tower test: more passes -> measurably less sink at the top,
      bit-identical replay fingerprints at any count, and count=1 reproduces
      the historical settle. The viewport's gd physics scene now runs 8
      passes — a deep-stacked pile is exactly where single-pass sink shows.
      The PACKAGE EXTRACTION is done: `modules/physics`
      (Warploom::physics, header-only INTERFACE, own install/package config)
      owns physics_world.hpp; core links it and the include path travels to
      every core consumer. Remaining: spheres-only, no joints or shapes.
- [x] **D2a interpolated, eased track evaluation** — `TimelineClip::evaluate_at`
      takes a fractional frame and an `Easing`, and interpolates Number and
      Vec3 between the bracketing samples. Bool and String hold the earlier
      sample, because there is no meaningful midpoint between `true` and
      `false`, or between two strings. `apply_easing` clamps so endpoints are
      exact: a clip lands precisely on its last recorded sample rather than
      asymptotically approaching it.

      Additive, not a replacement. `evaluate()` is untouched and Step remains
      the default, because a whole frame in Step mode must reproduce the old
      result *exactly* — a replay of an existing recording has to re-run
      identically to its original take, and a float-ULP drift would break
      that. `StepModeAtWholeFramesReproducesEvaluateExactly` walks 45 frames
      across the clip and demands `EXPECT_DOUBLE_EQ`, not a tolerance.

      Found and fixed an out-of-bounds read while testing this: a fractional
      query past a track's last key fell through to `samples[upper]` with
      `upper == samples.size()`. It asserted in debug and would have been a
      heap overread in release, so "past the last key holds" is now decided
      before the interpolation branch rather than inside it.

- [x] **D2b sub-frame tick position reaches playback** — `FrameInput` carries a
      `sub_frame` in [0, 1) and `tick()` routes it to interpolated playback, so
      the D2a interpolation has a caller rather than being a capability nothing
      reaches. Step-hold and interpolated playback now share one body
      (`tick_timeline_at`) instead of being two implementations free to drift.

      **Both shipped hosts still pass 0**, deliberately. A wall-clock-derived
      alpha would make playback depend on when a frame happened to be drawn, so
      a replay could not reproduce its original take unless the fraction were
      recorded in the protocol. That recording is the open part, and having the
      parameter exist first means the two can be done in the right order rather
      than being entangled. Zero behaviour change today, verified 100.0000% of
      pixels identical.

      **This surfaced a real bug.** A paused tick stopped physics but kept
      advancing clip playback — the physics stage checked `paused` and the
      timeline stage did not. At whole frames that was easy to miss, because
      playback mostly holds; a sub-frame tick moves the sample point and made it
      obvious. Both stages now obey the same gate, and time being stopped means
      all of it. Removing the gate fails 2 tests.

      Also fixed while threading it through: recording sample offsets are whole
      frames by contract, so a fractional sample point floors rather than
      producing a fractional offset the sample ordering and on-disk format both
      forbid. And adding a field to `FrameInput` silently reinterpreted every
      existing three-element `tick({frame, dt, paused})` call site — `true`
      became `sub_frame = 1.0` and `paused` defaulted to false. Six tests caught
      it; the lesson is that adding a positional field to a widely-used aggregate
      wants designated initialisers at the call sites.

      Still open in D2: recording the sub-frame fraction in the protocol so
      playback can be smooth *and* replayable; concurrent record-and-play;
      camera/light/render tracks; and `ClipTimelineView`, which still has no
      caller in the app.

- [x] **O1 structured diagnostics** — `warploom/core/diagnostics.hpp`. One
      filterable channel for app, engine and viewport: severity, subsystem tag,
      frame number, env-configured level (`WARPLOOM_LOG_LEVEL`), and a sink
      function pointer so the viewport can tee into telemetry while tests
      capture output.

      No new dependency, deliberately: the whole point of Phase 0 was one
      package manager and one library, and adding spdlog to get logging would
      undo that. It is also not a logging framework -- no sinks list, no async,
      no formatting library. One function pointer is enough.

      The gap it closes was measured, not assumed: **116 ad-hoc
      `fprintf(stderr)` calls in the viewport and exactly 1 in the entire
      renderer**, no severity, no timestamps, nothing greppable, and 66 bare
      `RuntimeError::` returns where a failure gave you a code and no
      indication of which stage produced it.

      Bug found while testing it: `WARPLOOM_LOG_LEVEL=off` *emitted*. `Off` is
      the highest level and the threshold test is `<=`, so with the threshold at
      Off every real severity passed. Both the threshold and the message level
      must rule out Off. `OffSilencesEverything` pins it.

- [x] **O2 GPU timing actually resolves, per pass** — it never had. Capability
      was detected and announced, but every `vkGetQueryPoolResults` returned
      `VK_NOT_READY`, so `last_total_ns` was permanently 0 and the viewport
      logged that 0 into telemetry every frame.

      Root cause: **a range query returns `VK_NOT_READY` if *any* query in the
      range is unavailable**, and two of the six stamps are legitimately never
      written — the H-Z boundary when occlusion is off, and any boundary for a
      skipped stage. So the call could not ever succeed. Now queried one stamp
      at a time, and a segment is emitted only when both of its endpoints are
      present and ordered.

      Rejected along the way, with reasons: `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT`
      is the tidier API but needs Vulkan 1.2, which this engine does not
      target; `VK_QUERY_RESULT_WAIT_BIT` **hung the app**, because a query whose
      submit never happened blocks forever.

      Measured on the RTX 2060, per frame, and cross-validating against
      configurations that must differ:

        bloom off        total 0.18ms | pre 0.03 scene 0.06 compose 0.09
        bloom on         total 0.34ms | pre 0.04 scene 0.08 compose 0.22
        no HDR           total 0.28ms | pre 0.09 scene 0.19 compose 0.00

      Compose more than doubles with bloom on (two extra fullscreen passes) and
      is exactly 0 with no HDR (the chain does not run). Those two facts are
      what make the numbers trustworthy rather than merely present. Telemetry
      now carries real per-frame values — 1991 distinct readings across a run,
      ~69us/frame — and still reports `-1` before the first resolve instead of
      asserting a zero.

      Compose is now the largest segment, which is the first real perf signal
      this engine has produced and was not available before this fix.

- [x] **Intermittent segfault in the threaded scheduler test** — FIXED.
      ThreadSanitizer (runnable once the tsan preset built) reproduced it with
      full stacks: `run_parallel` incremented its completion counter outside
      the mutex and locked only to `notify_one`, so the main thread could wake
      and destroy the mutex/condvar while a worker was inside `notify_one`.
      Replaced with C++20 atomic `wait`/`notify_one`, where the notify is part
      of the atomic operation. Honest evidence note in AGENTS.md: the A/B is
      not statistically conclusive on this loaded machine; the fix is certain
      structurally — the old ordering was undefined by the standard, the new
      one has no window.
- [ ] **D3 game depth** — ECS as the app's data model, scene management,
      **G4 script node**, G2 subgraph copy/paste, G5 asset browser.

      G2 SUBGRAPH COPY/PASTE DONE (engine half): `copy_subgraph(ids)`
      serializes the selection plus every INTERNAL link as a fragment with
      RENORMALIZED ids (smallest selected id -> 1), so structurally identical
      subgraphs copy to byte-identical fragments regardless of source ids;
      external links are dropped. `paste_subgraph(fragment)` parses it with a
      strict scanner (machine-written shape only — the M5 engine-written
      contract, hand-edits rejected), checks every type before mutating, and
      ROLLS BACK completely on any link failure (cycle, pin mismatch):
      verified by the round-trip test (copy -> paste -> copy is
      byte-identical) and the rollback test (node count unchanged after both
      failure shapes). Editor UI wiring remains.

      G4 ENGINE HALF DONE: `register_script_node_type`
      (modules/core/include/warploom/core/script_node.hpp) binds a script
      module to a node type named "script:<module>" — Number in/out pins sized
      by the caller, `context_evaluate` calls the module's
      `warploom_module_tick(dt, inputs, n_in, outputs, n_out)` and maps the
      returned count onto the out-pins. dt is derived from the graph context's
      time minus the node's previous evaluation time (0 on first call), never
      a wall clock, so replay re-simulation reproduces the sequence exactly —
      verified by DtFollowsTheTickSequence, which also replays the same
      sequence in a fresh graph and requires byte-identical outputs. Verified
      without any .so via ScriptModule's builtin path (three tests,
      ScriptNode.*). Missing inputs evaluate as 0; a module error return
      leaves outputs 0 — the same failure shape as a math node given garbage.
      The .so path is verified too: SharedObjectModuleDrivesTheNode binds the
      node type to the REAL dlopen'd fixture module and checks the tick
      outputs through the graph — the full path a gameplay module takes.
      Remaining: editor UI (a row per out-pin) and viewport wiring.

## Phase E — release

- [x] **E1 render modes as protocol commands** — DONE (protocol v1.9).
      `set_render_mode` (`text` = "forward" | "rt") and `get_render_mode`
      (`detail` = {"mode": ...}); the welcome snapshot carries `render_mode`.
      Modes are host-owned visuals, so the session stores nothing and the
      recorder deliberately does not record them (same class as
      set_camera/set_sun). The host builds the RT stack on FIRST switch and
      caches it, so neither switch direction can leave a half-built stack.
      Verified over a real socket on the RTX 2060: forward -> rt builds the
      TLAS stack live and switches, rt -> forward switches back, an unknown
      mode is rejected with a named error, and the whole exchange runs with 0
      validation diagnostics. R1/R2 are now unblocked: a mode toggle plus its
      pipeline work is all each one needs.
- [ ] **E2 R1–R4 — exposure/bloom controls DONE (see below); remaining:
      path tracing (R1), RT reflections/AO toggles (R2), GPU-driven skinned
      scenes (R3).** Exposure is DONE:
      `set_exposure` (protocol v1.9) is live end-to-end — validated by
      `HeadlessComposeFrameGoldenHash`'s two-exposure probe (2.0 vs 0.25 must
      produce different tonemapped frames; it originally did not, which caught
      ensure() resetting the live exposure from the frozen renderer config
      every frame — the preserve is now explicit in the chain), plus a live
      socket check that the chain reports the applied value back. The capture
      chain is synced in the same handler, so captured frames cannot silently
      disagree with the live frame. Remaining: path tracing, RT
      reflections/AO, skinned gd scenes.

      (SetBloom was briefly added and REVERTED: see below.)

      REVERTED attempt — runtime bloom toggle. The full-recreate path
      invalidates the HDR render pass that app scene pipelines bake (12 VUIDs
      and a stale pass in the golden test), so a light path was written:
      create the bloom side in place, rewrite the tonemap's binding-1
      descriptor. Result measured, not assumed: the bloom-ON frame came back
      BLACK with a device leak — the light path's rebuild sequence does not
      yet interleave correctly with the graph's cross-frame layout tracking
      for the down/up stages (the up stage writes the HDR image the tonemap
      is about to sample; ordering that against the scene pass at toggle time
      is the actual problem, and it is the same shape as the original B3b
      revert). Reverted rather than shipped black. The static config remains
      correct: bloom at start-up works (the viewport matrix runs bloom clean),
      and the toggle needs a design pass on its ordering contract first.
- [ ] **E3 hardware CI runner** — every RT test skips on lavapipe, so the
      best subsystem has the least regression protection.
- [x] **E4 warning debt, then `-Werror`** — DONE. All four presets build
      warning-free on BOTH clang and GCC (the GCC gap held the only
      memory-safety findings: dangling-pointer and null-dereference classes),
      and the sanitizer presets already build with
      `WARPLOOM_WARNINGS_AS_ERRORS=ON`. Paying the debt found real bugs, not
      style: see AGENTS.md's inventory.
- [x] **E5 formatting** — DONE. The tree already conforms: clang-format
      `--dry-run --Werror` reports 0 warnings across all 213 sources under
      modules/examples/tests/tools (checked in a throwaway worktree, not on
      the working tree). The missing half was the gate: a `format-check` job
      now runs that exact command in CI.
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
   viewport. **Not met (R1)**. The E1/E2 unblocks landed: render mode is a
   protocol command with a build-once RT stack, the live-proof harness covers
   the mode/exposure/bloom wire contract, and the RT pipeline + PT shaders are
   test-proven — R1 is now a viewport integration, not an engine gap.

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
