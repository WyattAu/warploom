# Rendering status: what is in the engine, what is in the app

Every row below is separated into three independent questions, because
conflating them is how this page used to mislead:

- **Engine** — is it implemented in `modules/render`?
- **App** — can the shipped `warploom_viewport` actually reach it?
- **Proof** — what test exercises it, and where does that test run?

A feature can be real, tested and still unreachable from the product.
Several are, and they are marked **test-only** below. That is the single
most important thing on this page: the renderer module's most impressive
APIs are not used by the application.

Proof runs under the Khronos validation layer. The full suite
(664 tests, `warploom_unit_tests`) passes on an RTX 2060 with **0
validation diagnostics and 0 leaked objects**; on CI's Mesa lavapipe
software Vulkan, every ray-tracing test skips because the extensions are
absent. asan-ubsan (581 tests, 0 diagnostics, 0 leaks) and tsan (581
tests, 0 data races) build and run on the Vulkan-off Debug tree.

Two verification milestones worth knowing when reading the Proof column:

- The renderer's whole frame loop (`begin_frame`/`record_commands`/
  `submit_frame`/`present_frame`) runs headlessly
  (`VulkanRenderer::initialize_headless`), so tests exercise it without a
  window system. Before that, every test using it was XCB-gated and
  preprocessed out: the loop had no executing coverage at all.
- The HDR compose chain is `VulkanComposeChain`, instantiable per target;
  the viewport creates a second instance so frame capture works with
  compose on (the old capture refused under compose). Equivalence across
  the extraction is pinned: the LDR capture is byte-identical old-vs-new,
  and `VulkanHardware.HeadlessComposeFrameGoldenHash` pins the composed
  frame's canonical hash (16601212335261142594, RTX 2060) — plus, since the
  bloom re-land, the bloom-ON frame's hash (5278270949640476686) and the
  toggle's no-rebuild contract (`compose_generation()` identical across
  on/off; off→on→off returns bit-identically). All bloom resources are
  pre-allocated at initialize; a toggle only changes which stages record.

## Legend

| Mark | Meaning |
|---|---|
| yes | implemented and reachable from the viewport |
| **test-only** | implemented and tested, but the viewport never calls it |
| partial | reachable, but less than the name suggests |
| no | not implemented |

## The renderer module

| Feature | Engine | App | Proof |
|---|---|---|---|
| Vulkan 1.2/1.3 context, sync2, timeline semaphores, descriptor indexing, bufferDeviceAddress | yes | yes | `VulkanContext` feature negotiation; headless degrade path |
| Render graph: computed barriers, layout transitions, queue-family release/acquire | yes | **test-only** | `test_render_graph*`, `test_shadow_mapping`, `test_postprocessing` |
| `record_pbr_frame` (the module's own PBR frame) | yes | no — the app records its lit pass via `record_pbr_scene` inside the renderer's own render pass | `test_shadow_mapping`, `test_rt_shadows`, `test_rt_reflections` |
| `record_pbr_frame_gpu_driven` (one-submission GPU-driven frame) | yes | partial — the app drives the two halves separately (`record_gpu_driven_cull`, `record_gpu_driven_draw`) because it renders a shadow pass between them | `test_gpu_driven_frame`, `test_gpu_driven_occlusion_frame`; app A/B is 100% byte-identical |
| `VulkanOffscreenTarget` | yes | **test-only** | 28 test files |
| Post: fullscreen pass, ACES tonemap, FXAA | yes | yes — `RendererConfig::enable_hdr_compose`; the app renders into a float intermediate and tonemaps into the swapchain | `test_postprocessing`; app verified under validation in 8 configurations |
| Bloom (Karis bright-pass downsample; the tonemap's binding-1 add IS the upsample) | yes | yes — opt-in via `RendererConfig::enable_bloom` (`WARPLOOM_BLOOM=1`), runtime-togglable via `set_bloom` (v1.10) | `test_postprocessing`; the chain's bloom-on frame pinned by hash, toggle round-trip bit-identical, live wire proof on the RTX 2060 |
| PBR (Cook-Torrance, metallic-roughness, bindless, tangent-space normals, emissive) | yes | yes | `test_pbr_scene`; the app's own fragment path |
| IBL: prefiltered env + irradiance + BRDF LUT bake | yes | yes | `test_pbr_ibl`; `VulkanIblBaker` is called by the app |
| Analytic sky (Rayleigh + Mie, sun disc) | yes | yes — the viewport draws it as the background pre-draw (cubes, mannequin and city branches) and runs the tracer in sky mode 1 with the baked light's sun | `test_sky_integration`; `test_pt_sky_parity` proves the raster pass and the PT miss branch evaluate one shared model (pixel parity max 0.0137, orientation pinned); `VulkanSkyPass.*` pins the one owned pipeline+UBO construction (sabotage-checked); live on the RTX 2060 the two backgrounds agree — raster top-band (212,227,235) vs PT (208,225,235) at identical views, PT sky-patch std 0.66 at 120 samples. Caveat: the GPU-driven draw path bypasses `record_pbr_scene`, so gd mode keeps the clear colour |
| Shadow map: depth pre-pass, PCF, bias | yes | yes — via `record_shadow_pre_pass`, the hand-rolled copy deleted | `test_shadow_mapping`, `test_rt_shadows`; app A/B: identical texel occupancy, uniform bias shift |
| Mesh LOD selection (GPU projected-size) | yes | yes | `test_lod_integration`, `test_gpu_driven_cull` |
| Per-object static/rigged pipeline switch in one pass | yes | yes | `test_pbr_frame_variants` |
| Mesh simplification (decimation / LOD mesh generation) | no | no | selection exists; nothing generates lower-LOD meshes |
| H-Z depth pyramid (built every frame) | yes | yes — `RendererConfig::enable_hiz`; contract checked on real frames by `HiZGraph.ContractHoldsOnARealFrame` (headless) and the swapchain golden test; `ComposeDepthReductionIsValidationClean` runs the viewport's compose+H-Z configuration headlessly and pins 0 diagnostics plus a non-uniform level 0 over a drawn triangle (sabotage-checked both halves: disabling the depth rebind fails validation with the exact viewport VUID signature and leaves a uniform pyramid) | tested |
| H-Z occlusion culling (consumer) | yes | yes — gd mode (`WARPLOOM_GPU_DRIVEN=1`, cubes scene via `WARPLOOM_NO_MANNEQUIN=1`); `WARPLOOM_OCCLUSION=0` disables at the push | `HiZGraph.PackedPyramidMatchesAbi` (packed ABI), `test_gpu_driven_occlusion_frame` (occluder-present/occludee-culled shader proof); live wiring proven by a forced-depth_min probe (74.7% of pixels change). The shipped demo scene is occlusion-sparse — frame_30 captures on/off byte-identical, re-measured after the depth-rebind fix on a pyramid sampling the real scene depth |
| GPU-driven draw: mesh table, vertex pull, compute cull → indirect | yes | yes | `test_lod_integration`, `test_gpu_driven_cull`; app A/B 921,600/921,600 pixels byte-identical |
| GPU skinning (bone SSBO) | yes | yes | `test_gpu_skinning`, `test_gpu_mannequin` |
| GPU timestamps / frame latency percentiles | yes | yes | `GpuTiming`; telemetry `gpu_ns` |
| Parallel secondary command-buffer recording | yes | **test-only** | `test_parallel_recorder` |
| Async-compute queue submission | no | no | queue family discovered, never used for submission |
| HDR intermediate + exposure | yes | yes | `WARPLOOM_EXPOSURE=<float>` at start-up and `set_exposure` over the control socket (v1.9); the configured value is pinned by `ComposeChainHonoursConfiguredExposure` |
| Deferred / G-buffer, MSAA | no | no | every image is `VK_SAMPLE_COUNT_1_BIT` |
| CSM / cascaded shadow maps | no | no | single 2048² map |
| SSAO, SSR, TAA, denoiser, volumetrics | no | no | — |
| Compressed textures beyond RGBA8 | partial | no | KTX2 BC1/BC3 decode in-tree to RGBA8 (loop 24); GPU-native compressed upload deferred |

## Ray tracing

Genuinely implemented, and the strongest part of the repository.

| Feature | Engine | App | Proof |
|---|---|---|---|
| BLAS build + per-frame TLAS on caller command buffers | yes | yes | `test_ray_query_first_contact` |
| RT pipeline: SBT, `vkCmdTraceRaysKHR`, device-adaptive alignment | yes | no | `test_path_tracing` vs a bit-exact CPU simulator |
| `VulkanPathTracer` (R1a/R1b): rgba32f accumulation, BLAS/TLAS, SBT, clear-once/add-per-frame, `read_mean` | yes | yes — `set_render_mode("pt")` / `WARPLOOM_PT=1`, renders the cube + ground slab scene | compose exposure — `ensure()` adopts the caller's configured exposure on the first call and preserves the live one afterwards; preserving on the first call too silently discarded the configured value, which only a 20x exposure change producing a byte-identical frame revealed; pinned by `ComposeChainHonoursConfiguredExposure`, configured at 0.37 and 2.75 because a test using the default value passes a broken build | RT dispatch — `rt_for()` resolves entry points per call instead of caching them in a global keyed by `VkDevice` handle value; handle reuse after destruction made stale pointers callable, which surfaced as a SIGSEGV in the NVIDIA driver from `vkGetAccelerationStructureBuildSizesKHR` in ~1 full-suite run in 6 (never in isolation). BLAS build and size query now share one `fill_blas_geometry`, which had drifted to describe vertex data at address 0 | viewport resize — `set_viewport_size` (v1.11) drives the full recreate path on demand: swapchain, depth + framebuffers, `resync_for_swapchain`, capture stack (target AND chain — the chain alone is not sufficient), path tracer accumulation, UI layout; measured across five consecutive resizes with capture on: 0 device losses, 0 VUIDs, PPM headers follow the live extent, and the PT accumulation rebuilds at the new extent | `frame_allocator` — pure frame-graph lifetime planner (no Vulkan): disjoint compatible transients share, overlapping ones never do, and a history resource (H-Z pyramid, PT accumulation) is never aliased in either direction; the rule is enforced by interval arithmetic alone and validated by sabotage, because three redundant guards once made every sabotage a no-op; 200-configuration deterministic sweep; 4 of 10 allocations elided on a compose-shaped frame | `capture_arrangement` — the viewport's capture arrangement (scene pass into the chain's scene target, compose into a different framebuffer) reproduced headlessly and pinned: the scene pass does not leak into the compose output | `path_tracing_real` — CPU-mirrored `EXPECT_FLOAT_EQ` probes + byte-identical repeat through the engine object, viewport-geometry build, resize + sample count; `VulkanHardware.PathTracedFrameConvergesThroughCompose` proves the noise floor falls from 1 to 8 samples; ~~live capture showed 36.6 → 17.5 std-dev over 12 samples~~ — CORRECTED: the capture chain's `using_external_source()` is FALSE in PT mode, so that measured the chain's own intermediate, not the PT accumulation. What is verified is the LINEAR accumulation: flat in situ (0.7336 → 0.7357 over 300–1800 samples inside the running viewport) and headlessly (0.01% drift). The captured PT image is now verified: 0.9994 rank correlation with the accumulation and 0% unwritten pixels, after the `fullscreen.vert` coverage fix (it had been 0.05) |
| Compose chain tonemap from an external source (PT accumulation, sampled in GENERAL) | yes | yes | `PathTracedFrameConvergesThroughCompose`; two sabotage directions (missing 1/n, stuck frame index) both caught |
| Loop path tracer (N-bounce, per-pixel PCG streams, temporal accumulation) | yes | no | `test_path_tracing_real`, validated against an independent fp64 MC integrator (262k samples); wall probes 0.585/0.579 vs 0.612/0.562, slab 0.6414 vs 0.6073 (GPU vs fp64, 8 bounces, 64 frames). Two correctness fixes with sabotage-checked guards: (1) the rgen comment claimed Duff 2017 but implemented a naive projection that degenerates for ±Z normals (zero vector, normalized) — both shader and all three CPU mirrors now use true Duff (`rotated_box` uses tilted geometry; walls use the fixed path too); (2) the position-fetch rchit reported INWARD normals (this repo's winding makes the raw cross point into the solid — measured live via normal-as-color: slab top read (0,−1,0)) and is now flipped toward the incident ray (PBRT Faceforward). The old wall agreement (both sides exactly albedo 0.8) was two different physics coinciding in tolerance — GPU rays went straight through the thin wall out the back, CPU rays bounced back into the room — and the old rotated-box pass was consistency-only (both sides shared the inward sign). GPU-side anti-vacuity guards pin the image itself: interior probes > 0.15 (trapping reads exactly 0), z-wall probes < 0.79 (lobe collapse reads exactly 0.8); both sabotages fail. Plus `faceforward_normals_face_outward`: a committed dims.w = 2 normal-visualization mode (`PathTracerConfig::visualize_normals`) pinning orientation with no MC tolerance — the negated-flip sabotage fails all three probes. Closed in `docs/research/loop25-duff-onb-faceforward.md` |
| Ray-query shadows vs PCF, A/B | yes | yes | `test_rt_shadows`; app needs `WARPLOOM_RT_MODE=1` |
| Ray-query reflections | yes | no | `test_rt_reflections` |
| Animated TLAS for skinned parts | partial | partial | one rigid transform per part from its dominant joint — not vertex-level skinning in the acceleration structure |

Two honest caveats on the ray tracing:

- **Every ray-tracing test skips in CI.** The CI Vulkan leg is Mesa
  lavapipe, which has no RT extensions. There is no hardware runner, so the
  subsystem with the most capability has the least automated regression
  protection.
- **RT *shadows* are an environment variable read once at start-up**
  (`WARPLOOM_RT_MODE`)**, but that is no longer the whole story: the
  render-mode SWITCH is a protocol command (`set_render_mode`, v1.9), so an
  RT stack is built on first request and cached, and the viewport can be
  driven into RT and back over a live socket. Path tracing joined it as a
  third mode (`"pt"`). What still cannot be toggled is *within* the RT stack:
  there are no RT reflection / AO sub-toggles, which is what R2 is about.

- **Path tracing is a third render mode, and it is narrow.** It traces the
  cubes-scene cube + ground slab at the swapchain extent captured when the
  stack was built, accumulates one sample per rendered frame, and tonemaps the
  accumulation image directly. It also **holds the camera** while active (a
  moving view averages a smear rather than converging). A resize rebuilds the
  accumulation and re-points both compose chains, so the mode survives
  `set_viewport_size`. What is still narrow: no app-scene integration (one cube
  + slab), and bloom does not layer on the PT source (the source is already
  divided by n).

## Determinism

The one subsystem that needs no caveat.

| Feature | Engine | App | Proof |
|---|---|---|---|
| Fixed-step scheduler, state hash, replay | yes | yes | 8 scrubber tests + 23-assertion socket proof |
| Document save/load, atomic write, undo boundaries | yes | yes | protocol-path test + 16-assertion proof |
| Protocol record/replay (`warploom-replay-v1`) | yes | yes | `test_command_recorder`, 19-assertion two-instance proof |
| Timeline clips as document entities | yes | no | 8 tests + 27-assertion proof; the widget has no app caller |
| Live socket proof harness  + `e1` (render mode/exposure/bloom wire contract)| — | — | `tools/live_proof.py all` → **71/71 assertions** headless; the visual-host half (payload validation + bloom read-back) passed on the RTX 2060 viewport |

**The determinism story stops at physics.** The viewport steps physics in
the wall-clock frame loop rather than the fixed tick, the headless host
never calls it, and the protocol has no physics commands — so replay
cannot capture or reproduce physics state.

## Physics

| Feature | Status |
|---|---|
| Semi-implicit Euler integration | yes |
| Sphere–sphere and sphere–ground contacts | yes |
| Broadphase | no — an O(n²) double loop every step |
| Collider shapes beyond sphere | no |
| Joints / constraints | no |
| Continuous collision | no |
| ECS integration | no — `physics_world.hpp` documents an ECS bridge that does not exist; `test_physics_ecs_bridge.cpp` tests a helper that exists only in that test file |
| Replayable | no |

`modules/core/include/warploom/core/physics_world.hpp` is 204 lines,
about 90 of them logic. Treat "physics engine" in the project's pitch as
unimplemented.

## Timeline ("movie engine")

| Feature | Status |
|---|---|
| Clips as document entities, schema v3 | yes |
| Undoable clip add / remove / move, protocol v1.8 | yes |
| Record and playback with auto-disarm, axis-suffix properties | yes |
| **Interpolation** | no — `TimelineClip::evaluate` takes the last sample at or before the frame, so playback steps between keys |
| Easing / curves | no |
| Concurrent record and play tracks | no — both arms are single scalars |
| Camera, light and render-property tracks | no |
| Clip strip widget in the app | yes — `ClipTimelineView` rebuild/sync above the W1 scrub strip; block drag commits ClipMove through the session queue, live-proven 60→84 with undo/redo |

## What closing the gaps means

In priority order, and each is a roadmap item rather than a footnote:

The first five items this list used to name are now done: the viewport
records through `record_pbr_frame` + the compose chain + an HDR target, the
document projects into one ECS driven by one `tick(FrameInput)`, timeline
evaluation is interpolated and eased, and render mode is a protocol command.
What remains, in priority order:

1. Background parity between the two render paths — CLOSED. The earlier
   "no environment light" diagnosis was wrong and is retracted: the cubes
   scene's `ibl_set` is assigned and the IBL bake contributes (frame mean
   104.3 with IBL, 85.8 with `WARPLOOM_LEGACY_LIGHTING=1` at frame 30).
   The image looked unlit because it was vertically inverted --
   `scene_camera_projection` now negates its y row (Vulkan NDC y is down).
   The remaining gap was scene-authoring (no sky wired into the scene),
   not model divergence, and it is now wired: `VulkanSkyPass` owns the one
   raster-sky construction (pipeline + SkyParams UBO against the scene
   render pass, per-frame view from the recorded camera, fixed
   surface-anchor origin because the unit-scale eye sits inside the shell),
   applied in `record_scene_into` for the cubes, mannequin and city
   branches, and the tracer runs in sky mode 1 with the baked light's sun
   and the parity harness's canonical coefficients. Measured live on the
   RTX 2060: 91% blue-dominant raster frame with a bright-zenith-to-horizon
   gradient, PT sky-patch std 0.66 at 120 samples, and cross-path top-band
   means within 4 counts ((212,227,235) vs (208,225,235)). The "20
   pre-existing HiZ dispatch VUIDs" seen in that A/B were a real bug, not
   background noise, and are now fixed: the H-Z reduction sets were written
   once with the swapchain depth while the graph transitioned the compose
   chain's scene depth (the viewport default), so every level-0 dispatch
   sampled a stale attachment the barriers never moved. The bindings now
   track the frame's depth source (`hiz_bound_depth_view_`, zero updates at
   steady state); a default-config viewport validation run reports 0
   diagnostics where it reported 10, and 0 in no-HDR mode too. The packed
   pyramid now varies across a drawn triangle where it was uniform before.
   Honest
   remainder: the GPU-driven draw path bypasses `record_pbr_scene`, so gd
   mode still shows the clear colour.
2. PT scene coverage. The tracer builds its AS from one cube + slab. The
   skinned, mannequin and city scenes have no PT geometry path, and bloom does
   not layer on the PT source. First cut landed: the rchit reports true
   geometric face normals (position fetch + inverse-transpose) instead of
   the dominant-axis guess, which is exact only for axis-aligned boxes --
   the prerequisite for any rotated or scaled instance. CORRECTED after
   landing: that cut shipped two stacked defects the probes could not see.
   (a) The rgen comment claimed Duff 2017 but implemented a naive
   projection degenerating for ±Z normals (both shader and CPU mirrors).
   (b) Worse, the position-fetch cross points INWARD for this repo's box
   winding (measured live: slab top reported (0,−1,0)), so the old wall
   agreement was two different physics coinciding in tolerance (GPU rays
   went through the thin wall out the back and read exactly albedo; CPU
   rays bounced back into the room) and the rotated-box pass was
   consistency-only (both sides shared the inward sign and trapped
   together). Fixed: true Duff ONB everywhere plus Faceforward flip toward
   the incident ray in both rchit and the fp64 soup reference. Now wall
   probes read 0.585/0.579 vs 0.612/0.562 and slab 0.6414 vs 0.6073, with
   GPU-side anti-vacuity guards (trapping reads 0, collapse reads 0.8;
   both sabotages fail), plus a committed normal-visualization mode
   (dims.w = 2) with `faceforward_normals_face_outward` pinning orientation
   with no MC tolerance — the negated-flip sabotage fails all three probes
   (see loop25). Engine 1:1 lifted: instances name their mesh via
   mesh_index (0xFFFFFFFF = identity, bit-for-bit backward compatible),
   proven by `shared_mesh_renders_identically` (shared BLAS bit-identical
   to duplicate BLASes; wrong mapping and out-of-range index both fail).
   Still open: the viewport still traces the hardcoded pair (app-scene
   geometry has no PT path yet).3. The document timeline is decoupled from the UI panel: `tick_session()`
   runs `editor.tick()` on every rendered frame whether or not the node
   editor is visible (previously it lived inside `tick_node_editor()`, so
   clip playback and recording were inert with the editor off). A
   step-released frame ticks as running (`paused=false`): a step is the run
   instruction, matching the headless host, which always ticked steps
   unpaused while the viewport passed its live pause flag -- so even the
   decoupled tick recorded nothing under step control until this landed.
   Verified live on the RTX 2060 with the editor off (`tools/vp_tick_check.py`:
   pause, wide clip, record across a mid-take edit, playback): 17/17,
   4 samples head 1.0 / tail 0.75, replay holds 0.75. Baseline A/B without
   the fix: 12/17, empty tracks, `clip_play` rejected. Plus 662/662 unit
   tests, 6/6 ctest, viewport build warning-free. The clip strip is now in
   the app (G3-B, `WARPLOOM_NODE_EDITOR=1` build): `ClipTimelineView`
   rebuild/sync above the W1 strip, block-grab → pointer-follow →
   release-commits-ClipMove through the session queue; live-proven on the
   RTX 2060 under Xvfb (clip 60→84 by xdotool drag, undo→60, redo→84),
   0 validation diagnostics, 666/666 unit + 39/39 editor tests.
4. Real physics (D1): broadphase, shapes, joints, deterministic by
   construction, stepped inside the shared tick.
5. RT sub-toggles (R2: reflections/AO) and GPU-driven skinned scenes (R3);
   R6b waits on the Vulkan side of the plan (create/alias/destroy), not on the
   planner: Loop 13 derives the declarations from the node sequence now.

- **Fullscreen coverage (fixed):** `fullscreen.vert` generated its third
  vertex as `(-1,1)` instead of `(-1,3)`, so every pass using it covered only
  75% of its target and left the bottom-right quarter at the clear colour --
  the live tonemap included. The PT capture's "composite" was this, not a
  second writer; the capture's Spearman correlation with the accumulation went
  from 0.05 to 0.9994 once fixed. Guarded by
  `VulkanHardware.CaptureArrangementScenePassBeforeCompose`, which asserts
  zero clear-colour pixels in the composed image.
- **PT resize correctness (fixed):** both compose chains now re-point their
  external source after the path tracer's accumulation image is rebuilt; the
  capture chain had been registering the pre-rebuild view, which the rebuild
  destroys.
- **Viewport clip dispatch (fixed):** the viewport now delegates all eight
  timeline-clip control kinds. The wire table parsed them; the dispatch switch
  did not, so the viewport answered "unhandled command". The session tick is
  still coupled to the node editor's visibility -- open.
