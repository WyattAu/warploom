# Warploom roadmap

Status markers: `[x]` done, `[~]` in progress, `[ ]` planned. Every item
follows the project's verification discipline (bottom of this page) — a
milestone is not done until its proof is machine-checked and documented.

## Read this first

The capability list in `rendering-status.md` separates *in the engine*,
*reachable from the app*, and *what proves it*. Three findings from that
audit shaped everything below; all three are now resolved (recorded here
so the shape of the work stays legible — each was true while every
S-track item was marked done):

1. **The app did not use the renderer module — RESOLVED in Phase B.**
   The viewport records through the module's entry points
   (`record_pbr_scene`, `record_gpu_driven_cull`/`record_gpu_driven_draw`,
   `record_shadow_pre_pass`) and composes through `VulkanComposeChain`
   (live + capture instances). What is still genuinely test-only is
   named as such in the table: the render-graph single-entry path,
   `record_pbr_frame`, `VulkanOffscreenTarget`, the parallel recorder.
2. **There was no single data model — RESOLVED in Phase C/D.**
   `DocumentProjection` carries the authored document into the ECS every
   session tick (idempotent, allocation-free steady-state); the viewport
   builds render objects from the ECS query and physics reads the same
   store through the ECS bridge. The document stays the authored truth
   (ids, undo, serialization); the ECS is the runtime store.
3. **Determinism stopped at physics — RESOLVED.** The viewport and the
   headless host tick the session with a fixed 1/60 dt, never a wall
   clock; pause travels into the tick and step control advances
   identically on both hosts.

Phase A fixed the substrate so those were addressable; Phases B–D used it.

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

      That fix was half the defect. B5 moved which image the graph *transitions*
      but not which image the dispatch *samples*: the reduction descriptor sets
      were written once at H-Z setup with the swapchain depth view, while the
      frame's source under compose is the chain's scene depth. Every level-0
      dispatch in the default viewport sampled a stale attachment the barriers
      never moved -- 10 validation errors per run that the test suite could not
      see, because headless tests run without compose (selection == bound image
      there). The bindings now track the frame's depth source
      (`hiz_bound_depth_view_`, refreshed only on drift, zero updates at steady
      state, and view changes coincide with idle setup/resize points so no
      in-flight set is mutated). Verified: default-config viewport validation
      run 0 diagnostics (was 10), no-HDR 0, 663/663 unit tests, 6/6 ctest,
      asan-ubsan + tsan 581/581. Regression-guarded by
      `HiZGraph.ComposeDepthReductionIsValidationClean`, which runs compose+H-Z
      headlessly over two frames (both ping-pong indices) and pins zero
      diagnostics plus a non-uniform level 0 over a drawn triangle.
      Sabotage-checked both halves: disabling the rebind fails validation with
      the exact viewport VUID signature (error count 10, same image in both
      bindings) and leaves a uniform pyramid, so neither assertion can pass
      by accident.
      (One full-suite run flagged `JobSystem.DispatchOverheadIsSubMicrosecond`,
      a sub-microsecond wall-clock assertion at load 20; it passes in isolation
      and on re-run, and the diff cannot reach job dispatch timing.)

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
      object resting on the same surface its comparison tiles sample. That
      A/B was re-run after the H-Z depth-rebind fix -- the earlier identical
      frames were measured with a pyramid reduced from a stale attachment,
      so the conclusion stood on a broken instrument. With the pyramid
      sampling the composed scene depth, frame_30 captures with and without
      `WARPLOOM_OCCLUSION=0` are still byte-identical (md5 28a48c3e, same
      2,764,816-byte PPM), and the cull reports active (40x23 tiles): the
      occlusion-sparse conclusion now rests on a valid instrument. The
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

      Closed (T4a, 2026-11-03): checkpoints now carry the full solver state
      (T4a physics_snapshot codec: knobs + bodies in id order + joints +
      session map, floats in shortest round-trip form), so a time-warp
      scrub restores physics bit-identically — proven by
      `PhysicsScrub.RingScrubRestoresPhysicsBitIdentically` (checkpoint,
      run on, scrub back to identical fingerprint/snapshot bytes, continue
      to an identical trajectory). File checkpoints carry it too (marker
      `p_hash` + third line; `fixed_dt` in the header), the opening physics
      restores into temporaries, the re-apply loop re-simulates through the
      session's own `step_physics` (pause-tracked), and the closing check
      covers physics — proven by `ReplayFileRoundTripsPhysicsBitIdentically`
      and the paused-span variant. Tamper fails loudly at all three layers
      (ring hash, codec schema with world+map untouched, file load error).
      Pre-T4a files load exactly as before (no `fixed_dt` = no
      re-simulation, empty physics = empty world).

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
      the historical settle. Substeps landed as well:
      `set_substeps(n)` makes step(dt) run n (integrate dt/n, detect, resolve)
      increments — shrinking the increment itself, complementary to the
      iteration knob which operates within each substep; verified the same
      three ways plus an explicit assertion that substeps(2)+iter(4) differs
      from substeps(4), so a future refactor cannot silently conflate the two
      knobs. The viewport's gd physics scene now runs 8
      passes — a deep-stacked pile is exactly where single-pass sink shows.
      The PACKAGE EXTRACTION is done: `modules/physics`
      (Warploom::physics, header-only INTERFACE, own install/package config)
      owns physics_world.hpp; core links it and the include path travels to
      every core consumer. Research pass done
      (docs/research/physics-solver.md — Small Steps SIGGRAPH 2021, MGPBD
      SIGGRAPH 2025, Jolt architecture + determinism contract, Fiedler
      timestep canon). Adopted, in order: contact-compliance opt-in (XPBD
      alpha) + velocity-from-position update (both behind a knob that
      defaults to the current replay output); island sleeping (union-find
      over the frozen contact graph — Jolt's per-step rebuild pattern);
      `WARPLOOM_PHYSICS_DETERMINISM_CHECK` (Jolt's record-rewind-replay
      double-step validator as a debug flag); capsule shape. Deliberately NOT
      adopted: warm-start caches (stored solver state breaks snapshot
      replay), ReSTIR (reservoir stream state), MGPBD (no deformables).
      Remaining: rotation, joints — each a design arc, sized in
      docs/research/physics-solver.md.

      Loop 12 correction (2026-10-26, research loop 1 of 10): the paragraph
      above listed the validator and the compliance opt-in as ADOPTED, but
      `code_search` showed neither existed in `modules/physics/` — the spec
      claimed adoption ahead of the code. Both now exist and are verified:
      `set_determinism_check(bool)` runs the substep sequence twice from a
      saved state per step and counts fingerprint mismatches in
      `determinism_violations()` (Jolt's sample-checkbox pattern, but
      test-pinned rather than sample-only); `set_contact_compliance(alpha)`
      divides the correction by `inv_sum + alpha`, default 0, so existing
      replays reproduce bit-identically. Verified: 3 new Loop12 tests, each
      sabotage-checked in both directions (ignoring the compliance term
      fails Softens; skipping the restore fails the validator test),
      13/13 PhysicsWorld, 99/99 physics-adjacent suites. Lean formalization
      in `proofs/PhysicsDeterminism.lean` (`lake build` clean): detection is
      a pure function of the frozen snapshot, the 1D grid is conservative
      when cell >= r1+r2, compliance 0 is definitionally the stiff
      projection. Still open: velocity-from-position update, island
      sleeping, capsule — sleeping is the scheduled Loop 2 of 10 item.

      Loop 2 (2026-10-31): island sleeping + restitution threshold DONE.
      `set_sleep_threshold(v)` (default 0 = never, bit-identical history)
      rebuilds islands per step by union-find over the frozen pair list +
      grounded flags; an island sleeps when every dynamic member's
      PRE-integration speed is below the threshold and it owns a contact
      edge; sleepers are restored to exact pre-step state and skipped by
      the solve. `set_restitution_threshold(v)` (default 0 = historical)
      makes sub-threshold impacts inelastic so piles can reach true rest.
      Verified: 400-body pile (100 decoupled 4-stacks) all asleep at exact
      rest pose/zero velocity, frozen bit-identical over 60 steps, fresh
      replay identical; validator-clean with sleeping on; 680/680 unit,
      6/6 ctest. Every new test sabotage-checked BOTH directions — and the
      first flip-sabotage PASSED, which caught a real test hole: freezing
      mid-air is also asleep+frozen+replay-identical, so the test now pins
      rest heights and velocities, not just frozenness.
      Four mechanisms the probes forced (all measured, all in-code):
      (1) sleepers count as PINNED (full impulse/correction to the awake
      side) — half-shares leave half the approach behind and jitter the
      layer above forever (~1 m/s sustained); (2) impulse vel_n uses the
      sleeper's PRE-step velocity — the integrated snapshot carries one
      gravity increment of never-real motion that otherwise fixed-points a
      lone body at exactly -g·dt forever; (3) passes 1+ re-derive RESIDUAL
      penetration from live positions — re-applying frozen depth pumps
      energy (4 passes launch bodies at m/s; blinking pairs sustain a
      2-step ~0.4 m/s cycle); iterations=1 output is untouched; (4) island
      edges carry a 1e-5 penetration gate — float dust + zero initial
      velocity otherwise sleeps bodies MID-AIR on step 1. Known limit
      (stated, not hidden): tall (3-4+) stacks dropped as a clump, and
      laterally-jammed piles, find a limit cycle the sequential solver
      cannot leave — measured on the HISTORICAL path too, so a solver-
      convergence limitation, not a sleeper defect; sleeping engages
      wherever the solver actually brings bodies to rest. Fixing clump
      recovery is separate convergence research (cf. Small Steps).

      Loop 3 (2026-10-31): velocity-from-position + capsule + rotation +
      joints DONE (staged XPBD adoption, R2 spec step 2). `set_velocity_
      from_position(bool)` (default off = historical impulse path,
      bit-identical) derives v from corrected positions with restitution
      re-applied from stored approach speeds; joints become
      velocity-consistent under it (positional-only in impulse mode —
      stated limit). Capsule shape (segment narrowphase via Ericson §5.1.8
      closest-points, ground support from axis |up.y|): upright-capsule rest
      + crossed-capsule separation. Rotation: exact rotation-vector
      orientation integration (unit-renormalized) + (r x J)/I contact torque
      (sphere transverse I exact, capsule volume-weighted composite, twist
      uses transverse — stated approx); ground friction spins a sliding
      ball (forward roll measured). Distance joints (insertion-order,
      per-joint compliance, island edges so jointed piles sleep/wake as
      units). Spin gates sleep (rad/s under the same threshold). Verified:
      8 new tests, each sabotage-checked (pair-sphere fallback, ground
      support fallback, zeroed torque x2, dropped angular gate, skipped
      joints, skipped VFP derive — every one fails the guard it targets),
      validator-clean, full suite 688/688, zero warnings. Honest limits in
      the header: spin omits surface-velocity feedback (friction stays the
      historical 0.98 damp), no rolling-resistance/angular-damping model.
      Still open (research-deferred, not regressions): speculative contacts
      (needs a tunneling proof as trigger), MGPBD (no deformables —
      explicitly not adopted), warm-start (breaks snapshot replay).
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
- [ ] **D3 game depth** — ECS as the app's data model, scene management
      (G2 subgraph copy/paste, G4 script node, G5 asset browser landed).

      G2 SUBGRAPH COPY/PASTE DONE (engine half): `copy_subgraph(ids)`
      serializes the selection plus every INTERNAL link as a fragment with
      RENORMALIZED ids (smallest selected id -> 1), so structurally identical
      subgraphs copy to byte-identical fragments regardless of source ids;
      external links are dropped. `paste_subgraph(fragment)` parses it with a
      strict scanner (machine-written shape only — the M5 engine-written
      contract, hand-edits rejected), checks every type before mutating, and
      ROLLS BACK completely on any link failure (cycle, pin mismatch):
      verified by the round-trip test (copy -> paste -> copy is
      byte-identical) and the rollback test (PasteRollsBackOnFailure: node
      count unchanged after both failure shapes). Editor UI wiring DONE:
      Ctrl+C builds the nids payload through the tested editor helper
      `selected_node_nids` (NodeEditorCopy.SelectedNodeNidsJoinsInViewOrder:
      empty when nothing is selected, view-order join otherwise) and
      enqueues the session's CopySubgraph; the returned fragment is stashed
      for Ctrl+V, which enqueues PasteSubgraph — both through the same
      undoable session path the protocol tests cover (CopyPasteUndoRedo-
      RoundTrip, SocketDrivenCopyPasteEscapes). Only the XCB keycode
      mapping stays platform-gated, like all viewport input (stated limit).

      G4 SCRIPT NODE DONE (engine + editor UI + viewport wiring):
      `register_script_node_type`
      (modules/core/include/warploom/core/script_node.hpp) binds a script
      module to a node type named "script:<module>" — Number in/out pins sized
      by the caller, `context_evaluate` calls the module's
      `warploom_module_tick(dt, inputs, n_in, outputs, n_out)` and maps the
      returned count onto the out-pins. dt is derived from the graph context's
      time minus the node's previous evaluation time (0 on first call), never
      a wall clock, so replay re-simulation reproduces the sequence exactly —
      verified by DtFollowsTheTickSequence, which also replays the same
      sequence in a fresh graph and requires byte-identical outputs. Verified
      without any .so via ScriptModule's builtin path (ScriptNode.*) plus
      NodeScriptNodes dispatch/determinism/degrade/round-trip coverage.
      Missing inputs evaluate as 0; a module error return leaves outputs 0 —
      the same failure shape as a math node given garbage. The .so path is
      verified too: SharedObjectModuleDrivesTheNode binds the      node type to the REAL dlopen'd fixture module and checks the tick
      outputs through the graph — the full path a gameplay module takes.

      G5 ASSET BROWSER DONE (engine half + viewport panel):
      `list_scene_files(dir)` (modules/editor/include/warploom/editor/
      asset_browser.hpp) scans a directory for loadable scene documents
      (*.json) and returns byte-deterministic sorted filenames — readdir
      order is filesystem-dependent so the result is always sorted; a
      missing/unreadable dir yields empty, never an exception (an empty
      panel is the honest display of "nothing loadable here"). Verified by
      AssetBrowser.SuffixGating / ListsSortedJsonOnly /
      MissingDirIsEmptyNotAnException (3/3 on the current tree). The
      viewport renders the panel (opt-in via WARPLOOM_SCENE_DIR): one
      load-button per file, rescanned each frame with buttons rebuilt only
      when the scan changes, click enqueues the session's LoadDocument
      command through the same undoable path OMNICPP_DOC uses. The click
      handler itself is XCB-gated like all viewport input (stated limit);
      everything else on the path is session- and engine-tested.
      Remaining in D3: scene management (no spec or exit criteria yet —
      not started, not claimed). G2/G4/G5 are landed. Editor UI: one
      value row per output pin ("name = value") after the param rows, so a
      multi-output node no longer overdraws into one line — verified by
      NodeEditorValues.ReadoutsAppearAfterEvaluate /
      ReadoutsHiddenWhenDisabled / MultiOutputNodeGetsOneRowPerPin (pulse
      on+value get distinct rows) / ScriptNodeGetsOneRowPerDynamicOutput
      (outputs=3 shows out0..out2, default shows one) /
      SyncRefreshesAfterReEvaluate (a test that passes without sync_values
      proves nothing, so the refresh assertion requires the sync) /
      ValueRowsDoNotDisturbParamHitTest. Dynamic script outputs are
      readouts, not link sources (only declared pins get sockets — stated
      limit). Viewport wiring: WARPLOOM_SCRIPT_MODULES="path[:in[:out]];..."
      preloads native modules and registers their bound types with full pin
      rows next to the built-ins; the spec parses through
      ScriptModule::parse_module_spec (ParseModuleSpec: defaults 1-in/1-out,
      counts 1..9, 7 rejection shapes), a missing .so or bad counts skips
      that entry logged-not-fatal, duplicates are skipped, and doc-load
      registers the same type so persisted script nodes parse. Remaining in
      D3: G2 editor UI wiring, G5 asset browser, scene management.

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
- [x] **E2 R1–R4 — exposure, bloom (R4), RT reflections/AO toggles (R2),
      GPU-driven skinned scenes (R3) and stepped path tracing (R1) DONE.**
      Exposure and bloom are DONE:
      `set_exposure` (protocol v1.9) is live end-to-end — validated by
      `HeadlessComposeFrameGoldenHash`'s two-exposure probe (2.0 vs 0.25 must
      produce different tonemapped frames; it originally did not, which caught
      ensure() resetting the live exposure from the frozen renderer config
      every frame — the preserve is now explicit in the chain), plus a live
      socket check that the chain reports the applied value back. The capture
      chain is synced in the same handler, so captured frames cannot silently
      disagree with the live frame. Bloom is DONE (v1.10, below). R3 is DONE:
      `pbr_gpu_driven_skinned.vert` (palette join on payload word 23, skin
      stream at set 0 binding 4, static-first/skinned-second partition — one
      indirect draw per partition, firstInstance stays global) is proven by
      `GpuDrivenSkinnedMatchesCpuSkinnedPixels` (byte-identical hash, two
      sabotage directions, restore check) and live in the viewport as an
      animated two-segment beam prop (always the last driven object, own
      2-joint palette, conservative grown sphere). R1 is DONE: stepped PT
      (protocol v1.13) — `set_pt_sample_target` / `get_pt_samples` /
      `get_pt_mean`; the tracer holds at the target (further rounds no-op
      unless a clear restarts), the viewport skips the submit while held,
      and two stepped 16-sample runs report bitwise-identical mean text
      live over the wire (e1 visual determinism round-trip, 58/58 e1).
      `stepped_target_holds_and_repeats_exactly` pins the engine side
      (hold freezes count + image, reset + re-record reproduces exactly,
      K=6 differs from K=4, sabotage-checked). The spec below was rewritten
      from the research pass
      (docs/research/rendering-and-graphs.md): the compose chain pre-allocates
      ALL resources at initialize (idTech's persistent-feature pattern) so
      runtime toggles only change which passes record — landed, and it is
      what made the bloom toggle landable; fullscreen passes move to dynamic
      rendering (Blade direction, stage 1) so passes stop baking; R1 itself is
      "offline-quality stepped PT" — pause, accumulate K frames into the
      rgba32f accum image via the existing deterministic PCG contract,
      tonemap from accum — deterministic by construction through the
      record/replay machinery, a capability other PT pipelines do not have.
      ReSTIR explicitly deferred: reservoir stream state breaks snapshot
      replay.

      **R1a DONE — the path tracer is an engine object.** Until now the whole
      tracer lived inside `tests/unit/test_path_tracing_real.cpp`: an rgba32f
      accumulation image, params/instance buffers, BLASes, a TLAS, an SBT, a
      descriptor set and the clear-once/add-per-frame round, ~300 lines of a
      rendering feature in a test fixture. That is the same defect class as the
      compose chain living inside VulkanRenderer (B3b), and it is why R1 had no
      engine API at all. `VulkanPathTracer` (modules/render) now owns all of
      it: `create()` (geometry in as device addresses, AS storage, image,
      buffers, SBT, pipeline), `set_camera`, `record_round(frame, clear_first)`,
      `read_mean()`, `sample_count()`, `reset_accumulation()`. The proof that
      the extraction preserved behaviour is the strongest available one: the
      existing determinism test now drives the engine object and still passes
      its `EXPECT_FLOAT_EQ` probes against the independent fp64 reference plus
      its byte-identical repeat — the test shrank 1035 -> ~730 lines and the
      assertions did not move. `create()` is re-entrant for a resize (validated
      by sabotage: removing the guard makes the validation layer report 5
      leaked objects). Geometry buffers stay caller-owned, which is what lets
      the same object serve a test room and, next, a viewport scene.

      Two honest limits. (1) Only the `pt_real` shader variant is engine-owned;
      the nested-recursion `pt_pathtrace` variant (2 misses, recursion depth 2,
      different binding order) is still exercised test-only through
      `VulkanRtPipeline` directly — generalising the object to it is a
      follow-up, not a claim. (2) `read_mean()` divides host-side; the
      in-shader normalization for the tonemap stage is part of R1b. Also fixed
      along the way: the shared test readback helper silently mis-decoded
      rgba32f (it read 4 bytes of each 16-byte texel — the low bytes of an
      IEEE float — and reported confident nonsense); both PT tests had
      bypassed it with private readers, which is why it was never caught.

      **R1b DONE — viewport PT mode.** `set_render_mode("pt")` (with
      `WARPLOOM_PT=1` to start in it) builds the stack on the RENDER thread,
      records one accumulation round per frame in the frame pre-pass, and the
      compose chain tonemaps the accumulation image directly. Verified live on
      the RTX 2060: the mode switch round-trips forward -> pt -> forward over a
      real socket with 0 validation diagnostics, and the stack builds at start-up
      and on demand.

      Four bugs were found and fixed on the way, three of them mine:
      - **The AS-build barrier was emitted only BETWEEN BLAS builds**, never
        after the last one -- so the TLAS build reused the scratch buffer with
        no ordering, and two meshes reproducibly lost the device inside the
        build with NO validation error at all. One mesh appeared to work
        because the hazard was latent there. Found by isolation: the same
        geometry builds headlessly in a test the viewport could not disprove.
      - **The single-buffer-with-offsets geometry trick** ("one upload, many
        BLASes") faults this GPU during the build. One buffer per mesh, which
        is what the RT-shadow stack already does.
      - **The frame counter did not advance on the clearing frame**, so every
        frame re-cleared with the same seed and the viewport showed a
        *converged* image that had never accumulated anything. The symptom
        looks correct: stable, non-black.
      - **The external tonemap source was sampled in SHADER_READ_ONLY** while
        the storage image stays in GENERAL, so the capture chain (a second
        compose instance recording after the live frame) declared a layout the
        image was not in. The source is now sampled in GENERAL by contract,
        which also removes the transition entirely.
      Plus: the accumulation image needed `VK_IMAGE_USAGE_SAMPLED_BIT` (the
      tests only read it back, so the missing flag was invisible until a real
      tonemap sampled it), and `VulkanPathTracer::create` leaked its command
      pool on every failure path -- found by the leak check during the
      debugging, which is exactly what that check is for.

      **How convergence is proven, twice.** Headlessly,
      `VulkanHardware.PathTracedFrameConvergesThroughCompose` drives the tracer
      and the chain directly and asserts the SPATIAL NOISE (std-dev of luma)
      falls from 1 sample to 8 while the mean stays bounded. Mean brightness is
      deliberately not the assertion: ACES + gamma compress a missing 1/n
      divide into a few percent, and the first version of this test passed a
      sabotaged build because of it. Both sabotage directions are caught -- a
      missing divide (std RISES) and a stuck frame index (std stays flat).

      ~~And in the live viewport, by capture: over 12 samples the noise floor
      falls monotonically 36.6 -> 17.5.~~ **That measurement was of the wrong
      image.** The capture chain's `using_external_source()` is FALSE in PT
      mode -- measured directly at the capture site -- so the chain composited
      its own intermediate rather than the PT accumulation, and the falling mean
      was the raster scene under a still-orbiting camera.

      **Getting the live path to converge took three fixes the headless test
      could not have found**, because it drives the objects directly:
      1. **The camera was orbiting.** "Pause to converge" only means something
         if the view is still; each round was tracing a different pose and
         averaging a smear. PT now HOLDS the camera pose from the moment it is
         entered (the sim clock keeps running; pause does that part).
      2. **The PT round's pre-pass was silently overwritten.** The renderer
         has ONE frame-pre-pass hook; the viewport installed the PT round, then
         20 lines later installed the shadow pass over it. Every signal said
         PT was running -- the stack built, the mode switched, zero validation
         diagnostics -- while its callback never fired again. Two owners of one
         hook is an API gap, not an application bug, so the renderer grew
         `add_frame_pre_pass_callback` / `remove_frame_pre_pass_callback`
         (the hook is now primary + a small chain) and PT registers through it.
      3. **The capture chain composited from its own HDR target**, which in PT
         mode still held the raster scene, so every captured frame was a
         perfectly plausible picture of something the viewport was not showing.
         The tell was that the captured mean matched the baseline capture to
         three decimals.

      **Known limits, stated rather than hidden:** a resize rebuilds the PT
      stack at the new extent and correctly re-converges from sample 0
      (accumulation is sized in pixels, so old samples describe an image
      that no longer exists); the tracer renders a static snapshot of the
      live scene (city/mannequin/cube/ground walks, bones read once —
      animation smears an accumulation, so it is frozen by design) with
      grey albedo (the albedo words stay bound but unread); and bloom does
      not layer on the path-traced source (the external set binds the black
      slot, because the source is already divided by n). Stepped PT
      (v1.13, above) is the deterministic interface: reset + target K +
      poll, with wire-level mean equality across runs.

      (SetBloom was briefly added and REVERTED: see below. It has since
      RE-LANDED on the pre-allocated design — see the DONE note.)

      REVERTED attempt — runtime bloom toggle. The full-recreate path
      invalidates the HDR render pass that app scene pipelines bake (12 VUIDs
      and a stale pass in the golden test), so a light path was written:
      create the bloom side in place, rewrite the tonemap's binding-1
      descriptor. Result measured, not assumed: the bloom-ON frame came back
      BLACK with a device leak. Reverted rather than shipped black.

      BLOOM TOGGLE: DONE (protocol v1.10) — and landing it properly exposed
      the real bug under the revert. The light path above was blamed on
      toggle-time ordering, but the same BLACK frame reproduces with bloom
      enabled at START-UP: the chain's "upsample" stage rendered into the HDR
      target's render pass, whose color attachment is LOAD_OP_CLEAR, so it
      erased the scene it was supposed to be adding to (and the pipeline has
      blending disabled, so even without the clear it would have replaced
      rather than added). Bloom-on has looked like this since it shipped;
      nothing caught it because every check was "does it run without VUIDs"
      (the viewport matrix) — never "are the pixels right". The stage is gone:
      the tonemap already samples the half-res bright-pass at binding 1 and
      adds it after the curve, which is the upsample (the sampler is the tent
      filter); `bloom_upsample.frag` survives as a standalone pass exercised
      by test_postprocessing.cpp, just not as part of this chain.

      The re-landed toggle is the pre-allocation the research pass prescribed
      (docs/research/rendering-and-graphs.md, R7): every bloom resource —
      target, pipelines, descriptor sets — exists from the first ensure(),
      and `set_bloom_enabled()` only decides which stages record. The
      tonemap binds one of TWO pre-written descriptor sets (bloom target vs
      1x1 black), so a toggle writes no descriptors, creates nothing, and
      cannot invalidate a pipeline that an app baked against the HDR pass.
      Measured: `compose_generation()` is identical across on and off (the
      observable meaning of "no rebuild"), the bloom-off frame is
      bit-identical before and after a round trip, and the bloom-ON frame is
      PINNED by canonical hash (5278270949640476686, RTX 2060) next to the
      bloom-off pin — because a sabotage that re-corrupted the bright pass
      produced a non-black frame that merely DIFFERED, which the
      difference-only assertion passed. Live: on, reads back on, off, reads
      back off, missing-arg rejected, all with 0 validation diagnostics on
      the RTX 2060. Missing bloom shaders now degrade the chain to bloom-off
      instead of refusing the tonemap; `set_bloom_enabled(true)` is refused
      with a named error rather than silently ignored.

      **R2 DONE — RT reflections/AO sub-toggles (protocol v1.12).**
      `set_rt_reflections` / `set_rt_ao` (+ getters) flip bits 0/1 of the
      push pad1 word the RT shaders read, so a toggle changes the next
      recorded frame and nothing else: with both toggles off the gated
      code does not execute, and the frame matches the pre-R2 render to
      ±1 LSB (measured A/B, 6 captures x default/nohdr/rt: default and
      nohdr byte-identical; RT mode 1 rounding-boundary pixel Δ=1 on
      half the captures — float reassociation from the edit itself,
      proven by restoring byte-equality with the pre-R2 shader).
      Non-RT shaders ignore the word. The
      reflection shades the hit from the instance's material albedo via
      `instanceCustomIndex` — the host writes the material index as the
      custom index, the same index the raster draw uses, so the pairing is
      valid by construction — faded with distance; a miss samples the
      irradiance environment. AO is 4 fixed Duff-ONB hemisphere taps scaled
      so a fully open hemisphere returns exactly 1.0 (only the ambient term
      is scaled; direct light keeps its hard shadow). Verified by
      `rt_sub_toggles.reflect_and_ao_flags` on the reflection proof's floor
      + red-cube scene: flags-0 shows floor gray; reflections on turns the
      mirror red and brightens the miss control; AO darkens the half-blocked
      contact while a fully-open floor pixel is bit-identical; both flags
      compose (contact darker than reflections-only, brighter than AO-only,
      red intact). Three sabotages caught: zeroed albedo, stalled custom
      index, neutered AO. The viewport enters RT mode at runtime via
      `set_render_mode rt` (the startup env only chooses the initial mode)
      and round-trips both toggles live (e1: on, reads back on, off, reads
      back off, missing-arg rejected, 0 diagnostics on the RTX 2060). One
      measurement shaped the test: the reflection addition rides through
      Reinhard + gamma, so the red signal is +6/255 and the thresholds sit at
      +0.01 — tight enough that a neutered stage (bit-identical pixels)
      fails, with ~2x margin. A red-dominance threshold written for a linear
      pipeline (+0.06) fails a CORRECT build here: assert the signal the
      pipeline can actually carry, the R1b noise-floor lesson again.
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
      Adoption DONE: `cmake --install` ships per-module package configs
      and an out-of-tree consumer (`find_package(WarploomCore) +
      find_package(WarploomPhysics)`, links `Warploom::core` +
      `Warploom::physics`, drives the public API only) configures,
      builds, and runs against the install tree — verified 2026-11-01.
      Remaining, each blocked and stated: WASM (no emscripten toolchain on
      this machine), native Wayland surface (headers present, but no
      compositor to verify against), docs rebrand + repo rename (owner
      decisions, not engineering).
- [x] **E7 estate-shared C++ gates** — DONE (warploom side; the kit
      itself lives in the estate repo). The estate runs one reusable workflow
      per language (rust-kit, go-kit, haskell-kit, node-ci, python-kit); C++
      had none, and warploom's 10-job CI is the only instance. Loop 21
      (2026-10-29, first of the all-directions series): `.github/workflows/
      cpp-gates.yml` holds the matrix as `on: workflow_call`, developed and
      executed HERE first — `test.yml` is a thin caller (triggers +
      concurrency only), all 10 job bodies moved byte-identically except
      `persist-credentials: false` on checkouts (estate convention; no job
      pushes, so zero behaviour change). The `contract` job
      (`scripts/check_ci_contract.py`) enforces the caller/callee interface
      and is sabotage-checked both directions. Loop 22 (2026-10-29)
      executed the handoff: `engineering-standards/.github/workflows/
      cpp-kit.yml` is live (tier input `fast|full`, all 22 action refs
      SHA-pinned to the SHAs already in use by sibling kits), the
      contract job is self-contained (estate checkout at `estate/` +
      `scripts/check_cpp_contract.py` — an inline heredoc was tried and
      is un-runnable under YAML block scalars, so it was replaced rather
      than shipped), warploom's `test.yml` calls it and the local files      are deleted. Loop 23 (2026-10-29) closed E7's consumer half:
      `WyattAu/warploom-game` (20-cube tower drop, 600 fixed steps, public
      API only) proves capture→load reproduces the document byte-identically,
      identical setups integrate bit-identically, and the pile fell and rests
      (top 5.9 → 3.49, bottom 0.498). Its CI calls the estate `cpp-kit.yml`
      (warploom-specific legs disabled), making it the second caller that
      proves the kit outside warploom. Dogfooding paid immediately: two
      install defects (physics missing `EXPORT_NAME`, core config missing
      `find_dependency(WarploomPhysics)`) and a real replay defect
      (same-frame open/close checkpoints collide in the scrubber ring, so
      load restored the closing bytes and re-applied the log on top — fixed
      with `restore_bytes`). E7 done; the next all-directions loop is the
      content pipeline (glTF/compressed textures). Loop 24 (2026-10-29)
      opened it: KTX2 BC1/BC3 decode in-tree (`bc_decode`, ~200 lines,
      integer-only, zero dependencies) into the existing `DecodedImage`
      contract, so real-world compressed KTX2 loads instead of failing.
      Hand-computed texel vectors, both BC1 endpoint modes, both BC3
      alpha modes, sabotage A/B precise; default ctest 6/6 with 658/658
      and asan-ubsan 581/0-diagnostics. GPU-native compressed upload
      stays deferred with its trigger (measured VRAM pressure).

## The 0.1 gate

**Warploom 0.1 is publishable when all four hold:**

1. **Clone-and-build** — a fresh clone reaches a working viewport with no
   manual steps. **Met** (A2), and now CI-enforced.
2. **Modules real** — the viewport and tests consume installed packages; no
   monolithic target. **Met** (S5, A3).
3. **Demo story works** — record, replay, scrub, save/load, proven over the
   live protocol. **Met** for the render/document timeline; D1 (real physics)
   remains open.
4. **Signature render feature** — R1 path tracing, toggleable in the
   viewport. **Met.** "Offline-quality stepped PT" runs through the
   deterministic record/replay machinery: pause, accumulate, read back the
   converged mean. Verified rather than asserted:
   - the mode round-trips `forward -> pt -> forward` over a real socket with
     0 validation diagnostics (R1b);
   - convergence is pinned headlessly by the *spatial noise floor* falling
     with sample count (mean brightness is not the assertion -- ACES compresses
     a missing 1/n into a few percent);
   - the linear accumulation mean holds at 0.74895 / 0.74888 / 0.74888 across
     32/128/256 samples, bit-identical across runs;
   - the live capture now matches the accumulation at **0.9994** rank
     correlation with 0% unwritten pixels (the 75%-coverage bug is fixed); the
     coverage property is pinned by a sabotage-validated test.

   What is *not* met is PT usability, and the docs say so: no denoiser (a
   deliberate v1 deferral -- pause-to-converge is the shipped story, see the
   comparison matrix §5), no adaptive sampling, a cubes-only scene, and bloom
   does not layer on the PT source. Those are post-0.1 feature steps, not
   correctness gaps in what 0.1 claims.

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

### R6: frame-graph transient allocation — in progress

R6a has landed: `modules/render/include/warploom/render/vulkan_frame_allocator.hpp`
declares resource lifetimes (in pass indices) and derives allocations from them
with no Vulkan involved. The planner is pure, so the rule it enforces is
testable without a device.

**The rule, and why it is the hard one.** A history resource — the H-Z packed
pyramid, and now the path tracer's accumulation image, which the capture chain
samples *after* the frame boundary — holds the previous frame's contents and
must survive every instant of this frame. Aliasing a transient onto one
produces a wrong picture with no validation diagnostic, which is the exact
failure class this repository keeps meeting. So a history resource spans the
whole timeline and therefore overlaps everything: it can neither own a shared
allocation nor be aliased onto one, and that falls out of the same overlap test
that governs transients rather than being special-cased.

Measured on the compose frame's own shape: 4 of 10 allocations elided, both
history resources kept. 200 deterministic random configurations: zero
violations.

**What R6a deliberately does not do.** It does not allocate. No Vulkan image,
no descriptor, no lifetime wiring. The compose chain still manages its targets
by hand.

**And R6b is not the obvious "wire it up" step.** The honest test of R6a is
whether the compose chain's hand-written target list can be *deleted* and
replaced by declarations, and surveying the code says it cannot — yet. The
compose chain's two targets are fixed-format render passes with framebuffers,
depth attachments and views, whose lifetimes are NOT expressible as pass
indices; and the renderer holds ~8 separate offscreen targets (gbuffer, depth,
shadow, H-Z, HDR) that are structurally independent of the compose sequence.
Mapping "a target with a framebuffer, depth attachment, and a specific final
layout" onto "a resource live on passes [i, j]" is not a small adapter; it is
a second model, and bolting one on now would produce a planner that describes
the graph in words while the allocation stays by hand.

So R6b is gated on a real prerequisite, not scheduled optimistically: the
renderer needs to declare its resources against a *graph* (not a fixed pass
list) first, which is the same unbaking that R3 (GPU-driven) and the
dynamic-rendering work are pushing toward. The planner is ready when that
exists; until then a live caller would be worse than none, because it would
describe an allocation that does not happen.

Loop 13 update (2026-10-26, research loop 2 of 10): the prerequisite now
EXISTS. `derive_frame_resources(nodes, size_of, is_history, tag_of)`
(`modules/render/.../vulkan_graph_resources.{hpp,cpp}`) walks the node
sequence the renderer actually records and emits `FrameResource`s in
first-touch order — lifetimes derived from the graph, with the three
things a graph cannot supply (byte size, cross-frame history, alias tag)
travelling as explicit caller callbacks in the RPS external/persistent
split. Verified: 3 new Loop13 tests over a compose-shaped 5-node frame
(scratch elided, overlapping bloom kept apart, all history Dedicated),
sabotage-checked (never-extend-last_use fails the 2 behavior tests),
10/10 FrameAllocator, plus the Lean model `proofs/FrameAllocator.lean`
(history always Dedicated; every Aliased entry names a compatible,
disjoint owner), plus `planKind_owner_earlier` (an Aliased entry's owner
is declared strictly earlier, so declaration-order allocation materializes
owner memory before any sharer binds it). R6b LANDED: `VulkanTransientStore`
(materializes the plan: per-Dedicated owner allocation, Aliased entries bind
the owner's memory after a desc-equality check that fails loudly on
mismatch) backs the compose chain — the hand-written HDR/bloom/depth image
list is deleted, `derive_compose_plan` builds the superset skeleton over
scene+bloom+tonemap stages, `adopt_color`/`adopt_depth` fit the store images
into the render-pass shells, and `verify_frame_nodes` re-derives lifetimes
from the unified scene+compose graph every frame and fails the frame rather
than rendering wrong when the recorded shape outgrows the plan. Verified:
669/669 unit (3 TransientStore on-device: disjoint elision, fuzz
never-aliases-history, skeleton matches derived lifetimes), 6/6 ctest,
`lake build` clean, 4-of-10 elision holds on-device via owner count.

Two post-land fixes, both exposed by the first non-cubes PT scene -- which
is why no earlier proof caught them: every prior PT proof drove
chain.record directly, never the renderer's unified graph with an external
source. (1) verify compared absolute pass indices against ONE skeleton, but
the tonemap sits at index 1 without the bloom stage and 2 with it, so
PT-with-bloom-off (the default) recorded external [1,1] against planned
[2,2] and failed every frame with no diagnostic. Verify now keeps one
interval map per bloom shape from the same skeleton builder and compares
each frame against the shape it has (sabotage: the wrong-shape map fails the
new test). (2) build_graph_passes rewrote the external descriptor every
frame -- VUID-03047 with frames in flight (10 reports); ensure() +
set_external_source() already cover every transition, so the per-frame write
is deleted. Pinned by
`VulkanHardware.UnifiedFrameWithExternalTonemapSourceRecords` (renderer +
tracer + external through record_commands -- the missing leg).

Process note, kept because it is the third time: the first sabotage of this
invariant **passed**, because three separate guards enforced it and removing
one was masked by the other two. Collapsing to a single mechanism exposed that
the randomized sweep was checking a tautology — it compared a share against a
resource declared later in the list, while the owner is always earlier, so it
could never fire. A probe measured 84 history-aliasing violations across 200
random configurations that the suite passed. Details in
`docs/research/rendering-and-graphs.md`.

### The viewport resizes now (v1.11 `set_viewport_size`)

The window was resizable but nothing downstream of it was: `ConfigureNotify`
events had been arriving since `STRUCTURE_NOTIFY` was selected and were being
dropped, while the capture chain, the capture framebuffer and the UI layout all
read the compile-time `kWidth`/`kHeight`. `apply_resize()` now recreates the
swapchain, the sized depth attachment and framebuffers, calls
`resync_for_swapchain`, rebuilds the capture stack and the path tracer's
accumulation, and relays out the UI -- in that order, device idle, at the top
of the frame loop where nothing is recorded.

The crash this took four attempts to find is the part worth carrying forward.
A resize with capture enabled device-lost the GPU (`VK_ERROR_DEVICE_LOST` on
the next submit) about half the time; the same resize with capture off was
clean, and under the validation layer it was clean *too*. That last fact is
the lesson: a validation layer that says nothing while the device dies means
undefined behaviour, not a spec violation, and it is the same tell as the BLAS
scratch-barrier bug in R1b. Three hypotheses died first -- recreate the render
pass (not required), rebuild the compose chain (necessary, not sufficient),
and "the fence wait should cover it" (`wait_idle` is `vkDeviceWaitIdle`, which
drains every queue). The actual fault was the one object nobody resized: the
capture's own colour target, render pass and framebuffer, sized once at
`FrameCapture::initialize` and left at 1280x720 while the swapchain moved.

Measured after the fix: five consecutive resizes with capture on, zero device
losses, zero validation diagnostics, and the captured PPM headers follow the
live extent exactly. Path tracing rebuilds too and correctly re-converges,
because an accumulation buffer is sized in pixels -- the noise floor restarts
at 33.96 and falls to 18.2 at 1000x600, and 33.70 to 5.16 at 1400x820. The
old samples describe an image that no longer exists.

Two smaller corrections in the same area, both found by looking for the resize
rather than by it being reported:

- The capture recorded the *scene* at `kWidth, kHeight` while composing at the
  live extent -- two different sizes in one captured frame, which is a real
  hazard even before any resize happens.
- The viewport did not set `_NET_WM_PID`, so `xdotool search --pid` matched
  nothing and any tool that needed to know which window belongs to which build
  could not find out. Two viewports share the window title, so title matching
  is not an alternative.

## The intermittent crash the coredump turned into a use-after-free

`ray_query_first_contact` segfaulted the host about one full-suite run in six,
inside `libnvidia-gpucomp.so` via `vkGetAccelerationStructureBuildSizesKHR`.
Verified pre-existing first (reproduced at clean HEAD with the resize work
stashed, 1/8) and never in isolation (0/12) -- which is what turned out to be
the decisive clue rather than a reassurance.

Under `--gtest_repeat=3` the crash rate went to ~2/3 and the crash *moved* to
`VulkanHardware.PathTracedFrameConvergesThroughCompose`, reaching the same
driver offset by the same `query_blas_sizes` frame. Two sites with an identical
top frame are one defect. The cause: `rt_for()` cached the RT entry points in a
process-global keyed by `VkDevice` **handle value**, and Vulkan reuses handle
values after destruction -- so a new device on a freed handle compared equal,
the cache skipped re-resolution, and later calls jumped through pointers
resolved from the destroyed device. The suite builds and tears down ~40
`VulkanContext`s, so reuse is near-certain; a single test builds one device and
reuses no handle, which is exactly why it never reproduced alone.

A second defect surfaced on the way: `cmd_build_blas` set
`vertexData.deviceAddress` and `vertexStride` while `query_blas_sizes` set
neither, so the size query described vertex data at address 0 with stride 0.
A size query may read the vertex data, and reading address 0 is a host fault
in the driver with no validation error to explain it. Both paths now share one
`fill_blas_geometry`.

Measured A/B on one harness: original 1/3 crashed at `--gtest_repeat=3`,
fixed 0/3; then 24 suite passes at `--gtest_repeat=4` and 6 consecutive plain
full-suite runs under validation with zero crashes, where it previously failed
about one in six.

The transferable part: an intermittent crash is a symptom, not a diagnosis.
"It only happens in the full suite" is a clue about *cumulative process state*
-- and specifically about handle reuse -- not an excuse to blame the test.

## Is the visible path-traced image actually converging?

Worth asking, because the capture path looked wrong: the displayed mean fell
monotonically from 11.8 to 2.7 across 2780 samples while the high-frequency
noise fell by the same factor, leaving mean/noise constant at ~14. A converged
image has structure dominating noise, so that ratio should *grow*.

Three hypotheses were tested rather than argued:

1. **A temporal smear** (the moving-camera bug again). Ruled out: PT holds the
   camera, and the geometry buffers are uploaded once at setup and never
   rewritten.
2. **A badly seeded RNG.** The shader seeds an LCG with a linear
   `pix.x + size.x*(pix.y + size.y*frame)` and takes one step from it, which is
   the textbook antipattern. Simulating the shader's exact PCG in Python over
   4096 frames: mean 0.494–0.506 against an ideal 0.5, standard deviation
   matching 1/sqrt(12), lag-1 autocorrelation 0.01–0.03, near-uniform deciles.
   **No bias and no correlation** — the RNG is fine, and the "obvious" bug was
   not there.
3. **The accumulator or the sample counter.** `read_mean()` returns *linear*
   radiance, so it isolates this with no tonemap in the path. Measured at 32,
   128 and 256 samples: **0.74895, 0.74888, 0.74888 — 0.01% drift**, identical
   to five decimals across three runs. The accumulator is correct and the
   Monte Carlo mean converges exactly as it should.

So the engine is right and the *measurement* was the problem, which is the same
conclusion R1b reached about mean brightness: ACES + gamma biases a noisy
estimator hard enough to look like a defect.

**One thing the capture path genuinely cannot do.** At 2780 samples the
measured high-frequency noise is 0.191 in 8-bit units, while the quantization
noise floor of an 8-bit image is 1/sqrt(12) = 0.289. The capture is
quantization-limited — it cannot demonstrate convergence past that, which is a
property of the instrument, not of the tracer.

**Still unexplained, and recorded rather than hidden.** The viewport's
displayed mean falls 4.4x where the engine-level test says it should
stabilise, and the constant mean/noise ratio is not something ACES bias alone
explains. That gap is open. What is *not* open: the accumulator, the counter
and the RNG are correct, and
`path_tracing_real.linear_mean_converges_to_a_constant` now pins that so it
cannot silently regress.

### The test that had to be sabotaged twice to become worth having

The drift check alone was not enough. Over-counting the sample counter halves
every reading — a constant factor — while the drift between 32 and 256 samples
stays at 1.4%, so the first version of this test **passed a broken build**. It
now pins the absolute value as well (~0.7489 +/- 5%) and asserts the counter
directly, and the same sabotage fails it with the number in the message. The
seed and scene are fixed, so the reading is bit-identical run to run here; the
5% band is for other GPUs, whose float precision will not match.

## The live path-traced capture is not showing the path-traced image

Chasing an anomaly in the capture -- the displayed mean falling 4.4x while the
noise fell by the same factor -- turned up a measurement error in an earlier
commit, and the error is worth more than the anomaly.

**What is verified.** The tracer is correct. The LINEAR accumulation mean is
flat in situ, measured inside the running viewport through `read_mean()`:
0.7336 → 0.7357 across 300 to 1800 samples. Headlessly it is flat to 0.01%,
bit-identical across runs, at every exposure and at 1632 samples, and robust to
the scene (two instances, the viewport's camera pose) and to the seed schedule
(monotonic rather than restarting). The RNG is unbiased and uncorrelated over
4096 frames, which mattered because seeding an LCG with a linear index and
taking one step is the textbook antipattern and would have been the obvious
suspect.

**What is not.** The captured image. `capture_chain->using_external_source()` is
**false** at the capture site in PT mode -- measured directly, not inferred --
so the compose chain samples its own intermediate rather than the PT
accumulation. Everything the capture path was used to "prove" was therefore
measured on a different image, including this commit's own earlier claim that
the noise floor falls 36.6 → 17.5 over 12 samples. It did; it was not the
path-traced picture.

The cause is an ordering one: on the `WARPLOOM_PT=1` path the external source
is registered while `capture_chain` is still null (the PT stack is built before
the capture chain), so the registration silently did nothing. The runtime
`set_render_mode("pt")` path registers it later and is unaffected.

**Fixing it is not done, and the next probe is named.** Attaching the source
turns the capture into a *constant* image: with the minimal fix (attach right
after the chain is built, nothing else changed) six captures from 200 to 1200
samples are bit-identical -- mean 182.796, std 105.541 -- while `read_mean()`
in the same process shows the accumulation advancing. 182.796 is plausibly the
right *brightness* for tonemap(0.734) at exposure 1.0, which is what makes this
insidious: the level looks right and nothing after it moves.

Hypotheses tested and refuted by measurement, not by argument:

- A missing `SHADER_WRITE -> SHADER_READ` dependency on the accumulation
  (`record_round` declared only `SHADER_WRITE -> TRANSFER_READ`, and the image
  stays in `GENERAL`, so nothing forced a dependency for a sampled read from a
  later submission). Added; no change in the output.
- An unordered capture submission (the capture waits on its own fence and never
  against the frame that recorded the dispatch). A host `wait_idle()` before the
  capture; no change.
- A descriptor write that is staged rather than applied. Refuted by reading it:
  `write_image` calls `vkUpdateDescriptorSets` immediately.
- The chain not being built at all. Refuted by measurement on the unmodified
  tree: `chain=present ext=0 compose=1` at the capture site.

So the remaining suspect is *which binding the tonemap actually reads*.
`set_external_source` writes binding 0 of `compose_set_external_`, and
`record()` selects that set when `external_image_` is set and puts
`external_image_` in the graph's sampled-image slot -- but if the compiled graph
builds its own image binding from that slot rather than reading the descriptor
set, writing `compose_set_external_` is writing the wrong object, and the
composite would keep sampling whatever the graph captured at compile time.
That is a concrete, cheap next probe, and it is the reason this is left open
rather than guessed at.

Two further measurements narrow it. `compose_set_external_` is allocated
UNCONDITIONALLY at ensure() (so set_external_source's null guard is not the
early return), and `write_image` calls `vkUpdateDescriptorSets` immediately,
so the write is not staged. The recorded graph uses `samples[].image` only to
compute barriers -- sampling comes from the descriptor set -- so the write and
the binding agree in principle. That leaves the value itself: the frozen
brightness is plausibly `tonemap(accumulation)`, i.e. a saturated image of the
raw SUM rather than the mean. With the correct `inverse_samples` the same
quantity should fall as the noise falls, and it does not move at all -- which
would mean the capture chain is compositing with the divide missing rather than
sampling a stale image. Verified NOT to be saturation: at exposure 0.05, where
nothing can clip, the mean is still bit-identical (100.600 / 58.141) across
eight captures from 200 to 1600 samples.

**Missing divide: refuted, and the experiment that refutes it is worth
keeping.** At exposure 1.0 and again at 0.05 the image was pinned, but 0.05 was
still far too bright to rule out saturation. At exposure **0.0005** nothing can
clip: `tonemap(0.734 x 0.0005)` predicts a mean of 25.37, the measurement is
25.3685, and the mean is *constant* across 300 to 1500 samples. A missing
divide would have scaled it 5x with n. So the composite carries the correct
`inverse_samples`, and the value on screen is `tonemap` of a constant -- which
means the sampled image really is stale, not mis-scaled.

Also verified on the way: `accum_image` and `accum_view` are both non-null at
attach time, so `set_external_source`'s "both handles valid" condition holds and
the descriptor is written to the accumulation view rather than left on the
fallback.

**The hypothesis is now REFUTED, and replaced with a proven statement.** The
capture was NOT observing the accumulation before the dispatch: the capture's
command buffer is submitted after the frame's, on the same queue, so ordering
is not the issue. Measured directly:

- The accumulation changes in **75.0% of its pixels** between n=300 and
  n=1500 (2,764,800 of 3,686,400 floats; max per-pixel delta 0.0266).
- The captured PPM changes in **0.02% of its pixels** (171 of 921,600) over the
  same window, by at most 1 LSB.

**So the capture is demonstrably not sampling the accumulation image**, whatever
the descriptor says. Also ruled out along the way:

- The divide is correct (exposure-0.0005 experiment above).
- `accum_image` and `accum_view` are both non-null at attach time, so
  `set_external_source`'s both-handles-valid condition holds.
- A later `ensure()` clobbering the write: re-attaching the source EVERY frame
  changes nothing (173 differing pixels, 0.02%).
- The graph binding its own image: `sampled_images` is consumed only for
  barrier computation (`vulkan_render_graph.cpp:295`); sampling comes from the
  descriptor set, and the set, the layout, the shader (`set = 0, binding = 0`)
  and `set_external_source` all agree.
- The descriptor write being staged: `write_image` calls
  `vkUpdateDescriptorSets` immediately.
- The composite not running: the output responds to exposure (182.796 at
  exposure 1.0, 25.37 at 0.0005), and the second matches
  `tonemap(0.734 x 0.0005)`.

**The external-source path is inert.** With the source attached, the capture is
byte-identical across frames -- and pointing it at the *swapchain image*, which
changes every frame by definition, changes nothing either. Meanwhile the
capture chain's own code is demonstrably correct: the same class, used the same
way headlessly, converges (`VulkanHardware.PathTracedFrameConvergesThroughCompose`
falls the noise floor from 1 to 8 samples through the external source).

So the defect is in how the *viewport* uses the chain, not in the chain. The
measurements that pin it down, all on the unmodified chain:

- `using_external_source()` is **true** at the capture site with the source
  attached (`ext=1 chain=present`), so `record()` selects
  `compose_set_external_`.
- The composite responds to exposure (182.796 at 1.0, 25.37 at 0.0005, the
  latter matching `tonemap(0.734 x 0.0005)`), so the tonemap runs and reads
  *something* whose value is right.
- The captured image does not change when the source is an image that changes
  every frame.

That last pair is the contradiction to resolve: the composite reads a
*correctly-valued but unchanging* image. The one candidate that fits is the
chain's own `hdr_target_` -- the fallback binding -- which in PT mode is never
written, because PT records no raster scene into it. In forward mode it IS
written, which is why the forward capture changes while the PT capture does
not.

**The bisecting probe has been run, and the chain is exonerated.** Driving the
compose chain headlessly with a *second*, differently-valued external source
produces a different composed output (a solid 0.02 source composes to a black
frame; the accumulation composes to mean 137.21). So the chain **does** track
its external source. Three further viewport-specific arrangements were
reproduced headlessly and all of them still work:

| arrangement | result |
|---|---|
| compose in a SEPARATE submission from the dispatch (the capture owns its own command buffer and submit) | still converges |
| a scene pass recorded into the chain's scene target BEFORE the compose (what `FrameCapture` does) | still converges |
| the compose in the same command buffer as the dispatch (the original harness) | still converges |

So the chain, the descriptor path, the divide, the layouts and the submission
ordering are all fine. **The one arrangement not yet reproduced headlessly is
the compose TARGET: the viewport composes into the capture's own framebuffer,
not the swapchain's.** A first attempt at that probe aborted in the readback
helper (a GPU sync hazard in the probe, not an engine fault -- the validation
layer reported only the probe's own leaked objects), so it needs care rather
than improvisation.

**Where that leaves the bug.** Everything on the engine side is now proven
correct, and the fault is somewhere in the viewport's arrangement of a correct
engine. The remaining candidates, in order of how cheaply they can be tested:
(1) the compose target framebuffer; (2) the two chains sharing one render pass
(the renderer's live chain also samples the accumulation in PT mode, and the
capture chain is a second instance of the same class over the same pass); (3)
the capture's own readback path presenting a stale buffer.

**One measurement that would settle (2) immediately** and has not been taken:
does the LIVE chain's output change? The live chain samples the same
accumulation through the same descriptor path. If the live view is also frozen,
the fault is in the shared "sample the accumulation" mechanism and the capture
is a red herring; if the live view tracks, the fault is capture-specific.
Reading the live view back is the obstacle, not the analysis -- the capture
path cannot read the swapchain, and the window system on this machine lists
stale viewports whose frames cannot be attributed to a build.

### The measurement, and four fixes

Reading the live view back needed the window to be attributable to a build, and
it was not: `_NET_WM_PID` was never actually set. The property was written
against an atom interned **later** in a different function, so it went out
against atom 0 and the server dropped it -- `xprop -id <w> _NET_WM_PID` said
"not found" and `xdotool search --pid` found nothing. The earlier conclusion
("this window manager strips it") was wrong; it was our own ordering bug. With
the interning moved before the write, `xprop` reports
`_NET_WM_PID(CARDINAL) = 4135356`, matching the running process.

**That unlocked the measurement, and the live view is frozen too** -- two
window grabs 25 seconds apart, identical mean (6519.7) and identical standard
deviation (3766.66). So the fault is in the shared "sample the accumulation"
mechanism, not capture-specific, and the capture was never the red herring it
looked like.

**Three real defects, all fixed:**

1. **The startup path never pointed the capture chain at the accumulation.**
   The PT stack is built before the capture chain exists, so the registration
   that lived at PT entry ran while `capture_chain` was still null and silently
   did nothing. Measured at the capture site: `using_external_source()` was
   FALSE with a chain present and compose active -- so the capture was
   compositing the RASTER scene, and the "decaying mean" that started this
   whole investigation was the raster scene under a still-orbiting camera, not
   a path-traced image at all. The attach now happens where the chain is
   BUILT.
2. **The compose chain's external-source barrier had a ZERO source scope.**
   `initial_access` and `initial_stage` were 0, so the graph emitted a barrier
   with `srcAccessMask = 0, srcStageMask = 0` -- which orders nothing. The
   write->read dependency the tonemap needs was never established. No
   validation diagnostic, because a zero-scope barrier is legal; it simply does
   nothing.
3. **The tracer's final barrier declared only `SHADER_WRITE -> TRANSFER_READ`**,
   which does not make the write visible to a shader read at all.

With all three fixed, the capture now reports `using_external_source() == true`
at the capture site, and the captured image's statistics match a converged
path-traced image (mean 182.7959, std 105.5407) instead of the raster scene's
(mean 10.48 and falling).

**Still not explained at this point in the investigation** (it is resolved
below, under "The capture defect: RESOLVED"). The captured image is not a
faithful tonemap of the accumulation: the Spearman rank correlation between
the accumulation's per-pixel values and the capture's luma is **+0.31**, where
a monotone tonemap of the same image would give ~1.0. Smoothing the
accumulation first does not help (+0.30), so the capture is not a noisier
version of the same picture -- it is a different picture with a similar mean.

### What looking at the pictures revealed

The capture and the accumulation were dumped and *looked at*, which is worth
more than another brightness argument:

- The accumulation is a correct path-traced image: white sky, a light-grey
  cube, a darker ground slab. (The row-profile statistic that said the bottom
  row was sky was misleading: the slab is a finite quad in the lower-middle,
  not a full-width floor, so the very bottom row is sky.)
- The capture is NOT that image. It is a bright sky at the top with the cube
  barely visible, and a large BLACK WEDGE with a straight diagonal edge across
  the lower half.
- The forward (raster) capture has the same wedge, and so does the **live
  window**: black, with a small dim object and a couple of bright slivers.

So the wedge is the scene's ground plane -- the shared unit cube scaled
(8, 0.1, 8) -- rendered black, and the raster path has no sky at all. The PT
path draws its own sky and lights the same geometry; the raster path does
neither. Disabling the shadow pass (`OMNICPP_NO_SHADOW=1`) changes nothing, so
it is not the shadow map.

**That is the actual defect, and it is bigger than the path-tracing capture.**
Everything measured in this investigation -- the "decaying mean", the frozen
capture, the low contrast -- is the orbit camera moving over a picture that is
almost entirely black, with a bright sliver sweeping across it. The path
tracer was never the subject; it was the only thing in the viewport that was
drawing a lit image, and comparing against it is what made the difference
visible.

### The real defect: three of the unit cube's six faces were wound backwards

Looking at the pictures led to the actual cause. `build_unit_cube` emits six
quads with a stored normal each. Computing the right-hand-rule normal of each
quad's OWN winding and comparing it to the stored normal:

| face | stored normal | winding normal | |
|---|---|---|---|
| +Z | (0,0,1) | (0,0,1) | ok |
| -Z | (0,0,-1) | (0,0,-1) | ok |
| -Y | (0,-1,0) | (0,-1,0) | ok |
| **+X** | (1,0,0) | **(-1,0,0)** | **inverted** |
| **-X** | (-1,0,0) | **(1,0,0)** | **inverted** |
| **+Y** | (0,1,0) | **(0,-1,0)** | **inverted** |

The pipeline's `frontFace` is `VK_FRONT_FACE_COUNTER_CLOCKWISE` and
`enable_backface_cull` defaults to **true**, so a backwards-wound face is not
merely shaded wrongly -- **it is culled and disappears**.

The consequences were silent and large. The ground is the shared unit cube
scaled (8, 0.1, 8), so its +Y face is its TOP: culled, the ground vanished,
and the viewport showed a black void with a lit sliver of the ground's edge.
The path tracer was unaffected -- ray tracing ignores winding -- which is
exactly why the tracer's picture looked right while the raster one was wrong,
and why comparing the two is what finally showed it.

Fixed: the three faces' corner orders are corrected, and each face is now
checked against its own winding's right-hand-rule normal.

**Verified by a controlled A/B at identical frame indices.** The camera is a
pure function of the frame's simulated time, so a capture at frame N is
reproducible, and comparing the SAME frame index across the two builds is
controlled without needing to freeze anything:

| frame | before (winding reverted) | after (winding fixed) |
|---|---|---|
| 30 | mean 25.71, 15.25% lit | mean 7.64, 4.62% lit |
| 60 | mean 21.30, 12.65% lit | mean 3.72, 2.31% lit |
| 90 | mean 17.14, **10.21%** lit | mean 127.75, **75.00%** lit |

And the pictures are unambiguous. Before: a black frame with a few floating
grey shapes -- the ground's top face culled, so the void shows through where
the ground should be. After: the ground is drawn, a lit plane with a clean
diagonal horizon edge against the black beyond it. The lit-pixel count at
frame 90 goes from 10.2% to 75.0%.

The whole suite is unchanged: 628/628, ctest 6/6, ASan 555, clang and GCC 0
warnings.

**A note on how this was nearly got wrong.** Two of the intermediate
conclusions were false because a diagnostic had silently failed to apply: a
`cp` of an older file removed a log statement, and "no log output" was read as
"the code path did not run" when the truth was "the log was not in the
binary". The cure is mechanical and worth keeping: **verify the marker is in
the binary before concluding anything from its absence**
(`strings build/default/bin/warploom_viewport | grep -c MARKER`). It also
surfaced that a *median latency budget* in a unit test is load-sensitive: on a
shared machine at load 17, `JobSystem.DispatchOverheadIsSubMicrosecond`
(500 ns budget) fails about two runs in three, and passes when the other
project's load subsides. That is an environment, not a regression -- but it
means a perf assertion in the suite should be understood as a
canary, not a gate.

**What is still different, and why (corrected).** The earlier claim here --
that the cubes scene has no environment light and that the raster path is
sun-only -- was **wrong**, and it came from reading a vertically inverted
frame. `scene_camera_projection` fed Vulkan an OpenGL-style NDC (`p[5] =
+f`), so world +Y mapped to the bottom of the viewport: the ground slab
appeared above the cubes and the clear-colour sky below them, which reads as
a plausible "backdrop behind the cubes". The orientation fix (`p[5] = -f`) is
committed separately. The environment light was always present and it
contributes measurably: on the cubes scene at frame 30, `WARPLOOM_LEGACY_
LIGHTING=1` (IBL off) lowers the frame mean from 104.3 to 85.8, and the lower
rows of the corrected image have mean luma 166-168. What genuinely differs is
the background: this scene has no sky pass, so the clear colour shows, while
the tracer renders a constant white sky. That is a scene-authoring gap, not a
lighting failure, and it is worth closing -- but it is much smaller than the
orientation bug it was hiding behind.

### The capture defect: RESOLVED -- the fullscreen triangle covered 75% of the target

The "composite" was never a composite. It was a fullscreen pass that did not
cover its target.

`assets/shaders/fullscreen.vert` generates its triangle from `gl_VertexIndex`:

```
float x = float((gl_VertexIndex & 1) << 2) - 1.0;   // -1, 3, -1
float y = float((gl_VertexIndex & 2)) - 1.0;        // -1, -1, 1  <-- wrong
```

Vertex 2 is `(-1, 1)`, not `(-1, 3)`. The triangle
`(-1,-1), (3,-1), (-1,1)` covers the screen only where `x + 2y - 1 <= 0`.
Inside `[-1,1]^2` the uncovered region is a right triangle of area 1 out of 4
-- **exactly 25% of the target** -- bounded by the line from the top-left
corner to the middle of the right edge. With `record_fullscreen_draw` setting
a positive viewport height, Vulkan's y-down framebuffer puts that region in
the **bottom-right**. The result was a smooth diagonal edge fading to the
render pass's clear colour, which is why it read as "the raster scene's ground
against the PT sky" for several rounds.

The fix is one shift: `float y = float((gl_VertexIndex & 2) << 1) - 1.0;`.

Measured on the live viewport, PT capture at 1280x720:

| | before | after |
|---|---|---|
| pixels at the clear colour | 25.0% | 0.0% |
| capture mean | 182.8 | 243.7 |
| Spearman vs. the accumulation | **0.050** (0.31 in an earlier run) | **0.9994** |

The correlation going from ~0 to 0.9994 is the proof: the capture had never
been sampling the wrong image. It was a faithful tonemap of the accumulation
with a quarter of the frame left unwritten.

**This affected every fullscreen pass, not only the capture.** The live
tonemap was also missing its bottom-right quarter; the visual proofs passed
because every assertion was a mean, a tolerance, or a pinned hash, and none of
them asked whether the image was covered. A hash of an image that is 25%
clear colour is a perfectly stable hash.

What the investigation established on the way, each verified in the binary
before its absence was trusted:

- the compose chain's external source is selected correctly (`external=1`
  on both the live and the capture chain, distinct `this` pointers);
- the descriptor is written at record time and the write succeeds;
- the draw executes with a non-null stage and pipeline;
- the capture chain composes into the capture's own LDR framebuffer, not the
  chain's scene target (distinct handles logged);
- the layouts match `save()`'s barrier assumptions;
- the accumulation is a correct path-traced image (dumped and looked at);
- skipping the raster scene pass in the capture changed nothing.

The permanent guards:

- `VulkanHardware.CaptureArrangementScenePassBeforeCompose` now asserts that
  **no composed pixel keeps the clear colour**. Validated by sabotage: with
  the old vertex formula restored it fails, reporting unwritten pixels.
- The two pinned golden hashes were re-pinned, because the old values encoded
  a frame with a quarter missing
  (`9622751058503674142` bloom-on, `12508388375569283214` composed, RTX 2060).

**Second defect, found on the way and fixed in the same pass.** On a resize,
`apply_resize` rebuilds the capture stack BEFORE it rebuilds the path tracer,
and `build_capture_stack` re-registers the OLD accumulation view with the
capture chain. The tracer's `create()` is re-entrant and calls
`destroy_resources()` first, so both compose chains were left holding a
destroyed `VkImageView`; the live chain from PT entry, the capture chain from
the early rebuild. Sampling a destroyed view is undefined and produces no
validation diagnostic once the driver reuses the handle. Both are now
re-pointed at the new accumulation image after the rebuild, and cleared if
the rebuild fails. Verified live: PT + `set_viewport_size(1000,600)` +
capture produced a 1000x600 PPM with 0% clear-colour pixels, minimum luma 234,
and no device loss.

**Third: the viewport rejected its own advertised clip commands.** The wire
table parsed `clip_add`/`clip_remove`/`clip_move`/`clip_record`/… but the
viewport's dispatch switch had no case for any of them, so a socket asked for
a timeline clip and got `{"ok":false,"error":"unhandled command"}`. The live
proof passed because `g3` runs against the headless host, which drives the
session directly; the drift was only visible on the viewport host. All eight
kinds are now delegated to the embedded `EditorSession`, with the structural
ones (add/remove/move) flagging a node-view rebuild.

One finding this left open is now fixed rather than papered over: the viewport
called `editor.tick()` ONLY inside `tick_node_editor()`, so with the node editor
off the session's animation frame never advanced. The session tick now lives in
`tick_session()`, called on every rendered frame whether or not the panel is
visible; `tick_node_editor()` keeps only the UI work (graph evaluate, widget
sync, layout, paint). A second half hid behind the first: the viewport passed
its live pause flag into the tick, while the headless host ticks steps
unpaused -- so stepped frames gated the timeline stage and record/playback
stayed inert under step control even once the tick ran. A step-released frame
now ticks as running, matching the host. Verified live on the RTX 2060 with
the editor off: 17/17 (record 4 samples across a mid-take edit, replay holds
the tail); the same script on the pre-fix tree gives 12/17 with empty tracks.
Plus 662/662 unit tests, 6/6 ctest, viewport build warning-free.

The second question above -- "why does the capture show the PT sky AND the
raster ground?" -- dissolved into the first. There was no second writer and no
composite: the "raster ground" was the *unwritten* quarter showing the clear
colour, and once the triangle covered the target the capture was the
accumulation alone. The raster path's own blackness is the separate,
still-open environment-light gap recorded above.

Honest state now: the tracer is proven correct in situ and headlessly, and
the capture path is verified end-to-end (0.9994 rank correlation with the
accumulation, 0% unwritten pixels). What remains for PT is usability rather
than correctness: no denoiser, no adaptive sampling, and the cubes-only scene.
