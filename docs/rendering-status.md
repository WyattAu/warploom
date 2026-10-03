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
(546 tests, `warploom_unit_tests`) passes on an RTX 2060 with **0
validation diagnostics and 0 leaked objects**; on CI's Mesa lavapipe
software Vulkan, every ray-tracing test skips because the extensions are
absent.

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
| Post: fullscreen pass, ACES tonemap, FXAA | yes | **test-only** | `test_postprocessing` |
| Bloom (Karis downsample + tent upsample) | yes | **test-only** | 2 test files; no engine caller, no app caller |
| PBR (Cook-Torrance, metallic-roughness, bindless, tangent-space normals, emissive) | yes | yes | `test_pbr_scene`; the app's own fragment path |
| IBL: prefiltered env + irradiance + BRDF LUT bake | yes | yes | `test_pbr_ibl`; `VulkanIblBaker` is called by the app |
| Analytic sky (Rayleigh + Mie, sun disc) | yes | partial | `test_sky_integration`; the app only uses it *baked into* IBL |
| Shadow map: depth pre-pass, PCF, bias | yes | yes — via `record_shadow_pre_pass`, the hand-rolled copy deleted | `test_shadow_mapping`, `test_rt_shadows`; app A/B: identical texel occupancy, uniform bias shift |
| Mesh LOD selection (GPU projected-size) | yes | yes | `test_lod_integration`, `test_gpu_driven_cull` |
| Per-object static/rigged pipeline switch in one pass | yes | yes | `test_pbr_frame_variants` |
| Mesh simplification (decimation / LOD mesh generation) | no | no | selection exists; nothing generates lower-LOD meshes |
| H-Z depth pyramid + occlusion culling | yes | **test-only** | `test_gpu_lod_occlusion`, `test_depth_pyramid_mips`; `enable_hiz` is only ever set in tests |
| GPU-driven draw: mesh table, vertex pull, compute cull → indirect | yes | yes | `test_lod_integration`, `test_gpu_driven_cull`; app A/B 921,600/921,600 pixels byte-identical |
| GPU skinning (bone SSBO) | yes | yes | `test_gpu_skinning`, `test_gpu_mannequin` |
| GPU timestamps / frame latency percentiles | yes | yes | `GpuTiming`; telemetry `gpu_ns` |
| Parallel secondary command-buffer recording | yes | **test-only** | `test_parallel_recorder` |
| Async-compute queue submission | no | no | queue family discovered, never used for submission |
| HDR pipeline + exposure control | no | no | the app renders straight to the swapchain; highlights clip |
| Deferred / G-buffer, MSAA | no | no | every image is `VK_SAMPLE_COUNT_1_BIT` |
| CSM / cascaded shadow maps | no | no | single 2048² map |
| SSAO, SSR, TAA, denoiser, volumetrics | no | no | — |
| Compressed textures beyond RGBA8 | no | no | the KTX2 decoder accepts uncompressed RGBA8 only |

## Ray tracing

Genuinely implemented, and the strongest part of the repository.

| Feature | Engine | App | Proof |
|---|---|---|---|
| BLAS build + per-frame TLAS on caller command buffers | yes | yes | `test_ray_query_first_contact` |
| RT pipeline: SBT, `vkCmdTraceRaysKHR`, device-adaptive alignment | yes | no | `test_path_tracing` vs a bit-exact CPU simulator |
| Loop path tracer (N-bounce, per-pixel PCG streams, temporal accumulation) | yes | no | `test_path_tracing_real`, validated against an independent fp64 MC integrator (262k samples) |
| Ray-query shadows vs PCF, A/B | yes | yes | `test_rt_shadows`; app needs `WARPLOOM_RT_MODE=1` |
| Ray-query reflections | yes | no | `test_rt_reflections` |
| Animated TLAS for skinned parts | partial | partial | one rigid transform per part from its dominant joint — not vertex-level skinning in the acceleration structure |

Two honest caveats on the ray tracing:

- **Every ray-tracing test skips in CI.** The CI Vulkan leg is Mesa
  lavapipe, which has no RT extensions. There is no hardware runner, so the
  subsystem with the most capability has the least automated regression
  protection.
- **The RT mode is an environment variable read once at start-up**
  (`WARPLOOM_RT_MODE`). It cannot be toggled after launch, there is no
  keyboard shortcut, and the control protocol has no render-mode command.
  This is why roadmap items R1 (viewport path tracing) and R2 (RT
  reflections/AO toggles) are genuinely unstarted rather than nearly done.

## Determinism

The one subsystem that needs no caveat.

| Feature | Engine | App | Proof |
|---|---|---|---|
| Fixed-step scheduler, state hash, replay | yes | yes | 8 scrubber tests + 23-assertion socket proof |
| Document save/load, atomic write, undo boundaries | yes | yes | protocol-path test + 16-assertion proof |
| Protocol record/replay (`warploom-replay-v1`) | yes | yes | `test_command_recorder`, 19-assertion two-instance proof |
| Timeline clips as document entities | yes | no | 8 tests + 27-assertion proof; the widget has no app caller |
| Live socket proof harness | — | — | `tools/live_proof.py all` → **64/64 assertions** |

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
| Clip strip widget in the app | no — `ClipTimelineView` exists and is unit-tested, and the app has only the W1 scrub strip |

## What closing the gaps means

In priority order, and each is a roadmap item rather than a footnote:

1. Route the viewport through `record_pbr_frame`, the render graph and an
   HDR target, so the module's own frame is the app's frame. This is what
   turns every "test-only" row above into a shipped feature.
2. One data model. The ECS has no production consumer while the document,
   `VulkanScene` and `PhysicsWorld` are three private worlds; unify them so
   physics becomes both ECS-integrated and replayable.
3. Real physics: broadphase, shapes, constraints, deterministic by
   construction, stepped inside the shared tick.
4. Timeline interpolation, and the clip strip actually in the app.
5. Render-mode commands in the protocol so R1/R2 become switches rather
   than start-up environment variables.