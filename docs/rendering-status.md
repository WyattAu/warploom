# Rendering status: claims vs. proofs

Every rendering feature below is verified by GPU tests with pixel-readback
assertions running under the Khronos validation layer (preset
`vulkan-validation`, zero diagnostics) plus a headless build where all GPU
tests skip cleanly. This page distinguishes **what is proven** from **what is
app-facing**.

## Proven rendering features (offscreen GPU tests)

| Feature | Proof | Test |
|---|---|---|
| Render graph (auto barriers, layout transitions) | shadow→main, HDR→tonemap chains run with computed (not hand-authored) sync; zero validation diagnostics | `test_render_graph*`, `test_shadow_mapping`, `test_postprocessing` |
| PBR (Cook-Torrance, metallic-roughness) | metallic vs. roughness cubes shade differently under identical camera; golden fingerprints | `test_pbr_scene` |
| IBL: BRDF LUT, prefiltered env, irradiance bake | GPU-baked maps change cube shading as expected; bake readback hashes | `test_pbr_ibl` |
| Shadows (shadow-map pre-pass through the graph) | occluded ground pixels darken; same pixels through graph path | `test_shadow_mapping` |
| Analytic sky (Rayleigh+Mie, sun disc) | background switches to blue-dominant sky pixels; horizon vs. zenith sweeps | `test_sky_integration` |
| HDR post: ACES tonemap + FXAA | pixel-identical readback through two graph nodes | `test_postprocessing` |
| glTF 2.0 import (meshes, materials, embedded PNG/JPEG, skins, nodes) | whole-scene import renders; per-primitive multi-material sRGB proofs; malformed-input rejection suite | `test_gltf_scene`, `test_gltf_importer` |
| GPU vertex skinning (bone SSBO, blended deform) | bent pose changes rendered pixels exactly as the bone math predicts | `test_gpu_skinning` |
| Skeletal glTF import (skins, node forest, animations) | rest pose reproduces the bind pose exactly; malformed-document rejections; byte determinism | `test_gltf_animation` |
| GLB 2.0 container import (all glTF entry points) | mannequin packed as .glb imports byte-identically to .gltf+.bin (FNV fingerprint + sampled pose); 6 malformed-container rejections | `test_gltf_animation` |
| CUBICSPLINE animation samplers | exact Hermite basis values at quarter/midpoint, tangents steer the curve, unit-norm rotations, 3x-count contract enforced | `test_gltf_animation` |
| Matrix node decomposition (real DCC exports) | 90-degree matrix root decomposes to TRS and recomposes to 1e-5; shear/singular rejected | `test_gltf_animation` |
| Real rigged asset (Khronos CesiumMan) | full import: 22 nodes, 19 joints, 2 s 57-channel walk, decoded texture; sampling moves >= 3 joints | `test_gltf_animation` |
| Pose blending / clip cross-fade | exact endpoints, 45-degree midpoint quaternion, clamped alpha, full-alpha == direct sampling; live viewport walk<->idle cycle validated | `test_gltf_animation`, telemetry run |
| KTX2 containers (uncompressed RGBA8) | mip-chained container decodes level 0 byte-exactly; 12 malformed variants rejected; supercompressed payloads fail loudly | `test_ktx2_decoder` |
| Viewport observability (telemetry + GPU capture) | JSONL per-frame log, deterministic env run control, offscreen color+depth captures; 31-check analyzer passes on both scene variants with 0 validation diagnostics | `scripts/analyze_telemetry.py` + live runs |
| Scene state exposure (static + dynamic) | scene_objects/skeleton/clips manifests at startup; per-frame pose records (swing joint, allocator stats); analyzer asserts structure | live runs + `analyze_telemetry.py` scene-structure section |
| Input abstraction + virtual driver | named actions/axes, double-buffered snapshots with edge detection; JSONL virtual driver drives camera zoom (analytic radius response) and walk<->idle cross-fade; determinism proof: 669 telemetry lines byte-identical across runs | `test_input_state` + scripted live runs |
| Closed-loop scenario runner | scenario JSON (model + input script + expected responses) -> viewport runs under validation, analyzer gates, response assertions (input consumed, radius delta, blend targets), optional double-run determinism; 13/13 on mannequin scenario, 5/5 on the real CesiumMan asset | `scripts/run_scenario.py` + `scenarios/*.json` |
| Real-device input (keyboard/mouse/gamepad) | pure translators (18 headless tests: WASD/arrows/wheel/pointer-delta mapping, xpad axis layout, deadzone rescale, trigger idle remap, dpad, edge semantics); live XTEST proof: a real keystroke moves the camera on the analytic 1.5 u/s curve; gamepad driver no-ops cleanly when absent | `test_input_translators` + `scripts/xinput_proof.sh` |
| Animated mannequin through the skinned pipeline | imported on-disk asset walks: rest vs mid-stride render different images, >800 px each | `test_gpu_mannequin` |
| Mesh LOD (GPU selection via projected size) | near/mid/far bars select LOD 0/1/2 on the GPU; readback proof | `test_lod_integration`, `test_gpu_driven_cull` |
| H-Z depth pyramid + occlusion culling | mip-chained real-depth pyramid; footprint-adaptive selection culls fully-covered objects | `test_gpu_lod_occlusion`, `test_depth_pyramid_mips` |
| Mesh table + dedup (GPU-driven step 1) | byte-exact dedup, global index rewrite (CPU tests, headless) | `test_mesh_table` |
| Vertex-pull A/B parity (GPU-driven step 2) | ONE `vkCmdDrawIndexedIndirect` produces **byte-identical pixels** to the per-draw path | `test_gpu_driven_ab` |
| GPU-driven cull→indirect draw (step 3) | compute writes draw commands; near bar LOD 0 (~224 px), far bar LOD 1 (~20 px), behind-camera culled | `test_gpu_driven_cull` |
| One-submission GPU-driven frame (renderer-owned) | `record_pbr_frame_gpu_driven`: command readback exact + pixel structure identical; CPU never reads cull results | `test_gpu_driven_frame` |
| Sustained frame benchmark | 300 frames, 64 instances: frame CPU p50 ≈ 105 µs | `SustainedGpuDrivenFrameBenchmark` |
| Occlusion in the one-submission frame | A/B in one test: occlusion off → far bar renders; on → GPU writes degenerate command, visible 2→1, pixels disappear | `test_gpu_driven_occlusion_frame` |
| Windowed presentation path | swapchain + XCB surface + acquire/submit/present verified against the real X server on hardware | `HeadlessSwapchainAndRenderSubmission`, `SwapchainRecreationStress` |

## App-facing surface

`omnicpp_viewport` (examples/viewport, built with `-DOMNICPP_BUILD_EXAMPLES=ON`):
a real window (XCB) showing a lit PBR scene — spinning metal cube, rough cube,
ground slab — with an orbiting camera, presented via vsync. It renders through
the same `record_pbr_scene` and swapchain paths the tests prove, using the
renderer's `set_scene_record_callback` frame hook. Run:

```sh
OMNICPP_SHADER_DIR=<build>/tests/shaders ./build/<preset>/bin/omnicpp_viewport
```

Verified live on hardware (RTX 2060, X11): window maps, frames present, the
image changes every frame (animation), zero validation diagnostics under the
Khronos layer.

## What is NOT yet app-facing

- **RT (ray query) in the viewport**: ray queries are proven on hardware
  (below) but not yet wired into the window path (shadows/AO, reflections).

## RT acceleration structures + ray queries (E1) — GPU proof complete

`VulkanAccelerationStructureBuilder` builds triangle-geometry BLASes and
per-frame TLAS rebuilds on caller command buffers (device-local storage,
shared scratch pool, host-coherent instance buffer, `scratchData` addressing
per the current spec — the old scratch usage bit no longer exists). RT
entry points are extension functions and are fetched via
`vkGetDeviceProcAddr` (the loader does not export them). Supporting fixes
that the proof forced into the open:

- **Device feature chain**: `bufferDeviceAddress` is now enabled whenever
  supported (not only behind RT), and the pNext chain joins the 1.2/1.3
  structs unconditionally at their API level — previously a device without
  sync2 or timeline semaphores would have silently dropped descriptor
  indexing or RT bits. Also fixed a self-referential RT pNext chain bug.
- **Allocator**: every allocation now carries
  `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` when the feature is supported
  (VUID 03339) — SHADER_DEVICE_ADDRESS buffers were previously unaddressable.

| Claim | Proof | Source |
|---|---|---|
| BLAS/TLAS build + ray query end-to-end | 8x8 parallel-ray grid, exact per-ray expectations: 24 hits on instance A (translated), 12 on instance B (**rotated 90 deg — load-bearing**), 28 misses; committed instanceCustomIndex verified per ray; 0 VUIDs, no leaks | `test_ray_query_first_contact` |
| SBT sizing groundwork (E3) | `VulkanRtQuery::get_properties` returns handle size / aligned raygen stride | `vulkan_rt_query.cpp` |

## Ray-query reflections (E2) — GPU proof complete

`pbr_rt_reflect.frag` + `tests/unit/test_rt_reflections.cpp` (commit 4729b1a):
one ray query along reflect(-V, N) per fragment on top of the Cook-Torrance
path; hits shade from a per-instance color SSBO indexed by
instanceCustomIndex (same indexing as the TLAS build), misses fall to a dim
sky constant. Reflection weight is Schlick-driven (grazing angles reflect
more). Verified on hardware under validation.

| Claim | Proof | Source |
|---|---|---|
| Mirror shows the reflected cube | virtual image B'=(0,-1,-3) of a cube B=(0,0,-3) over a y=-0.5 slab; the pixel at project(B') reads red-dominant (r−max(g,b) > 0.06), CPU-verified segment/AABB ground truth before the GPU render | `test_rt_reflections` |
| Direct material path sanity | direct cube view red-dominant | `test_rt_reflections` |
| Control point | reflected ray at Q misses everything; pixel channels neutral within 0.05 | `test_rt_reflections` |
| Probe y-convention lesson | the projection matrix already carries the Vulkan y-flip (m[5]=-f), so pixel row maps directly from NDC (no second GL-style flip). The env-gated 16px-cell classification map (OMNICPP_RT_REFLECT_DEBUG) exposed the double-flip by matching every silhouette edge to analytic predictions before assertions ran | debug block in `test_rt_reflections.cpp` |

## Ray-query shadows A/B vs PCF (E1 final) — GPU proof complete

One scene rendered twice through `record_pbr_frame`, differing only in the
shadow mechanism: path A = 1024² depth map + PCF (`pbr_shadow.frag`), path B
= ray query against a scene TLAS (`pbr_rt_shadow.frag`). Verified on hardware
under validation, 0 VUIDs.

| Claim | Proof | Source |
|---|---|---|
| RT shadows ≡ PCF shadows | near face lit with byte-identical shading on both paths; far cube hard-shadowed on both (inside the occluder's light column, analytically derived); exact non-clear-pixel parity (1199 == 1199) | `test_rt_shadows` |
| Camera-ray tracer as ground truth | 64×64 parallel-ray compute tracer confirms which TLAS instance each image row hits, plus per-hit world positions | `rt_shadows_a_vs_b.comp` |
| Vulkan depth convention fixed | all four shadow-sampling shaders remapped z with the GL window-depth convention (`*0.5+0.5`); Vulkan stores NDC z directly, so every lookup landed ~0.5 deeper and shadowed everything. Invisible to earlier tests because they only counted non-clear pixels. Fixed in `pbr_shadow/pbr_full/pbr_gpu_driven_full/pbr_ibl_shadow.frag` | A/B test assertions on shadow *values* |
| Slope-scaled shadow bias | pre-pass sets dynamic `vkCmdSetDepthBias` (const 8, slope 128); the constant factor is nearly inert for D32 (scaled by ~2⁻²³), the slope does the work | `vulkan_pipeline.cpp` + `record_shadow_pre_pass` |

## GPU-driven draw path (C2) — DONE

`OMNICPP_GPU_DRIVEN=1` (cubes scene) moves the entire visibility/LOD/draw
pipeline onto the GPU: the frame's object transforms go into a per-image
payload, the `cull_and_draw_lod` compute pass (recorded in the renderer's
pre-pass hook, after the shadow pass) writes every
`VkDrawIndexedIndirectCommand` from the mesh table, and the main pass issues
**one** `vkCmdDrawIndexedIndirect` for the whole scene. The CPU inside the
frame computes no visibility, no LOD, and no per-draw submission.

| Claim | Proof | Source |
|---|---|---|
| GPU-driven path matches per-draw output | A/B live runs, identical cubes-only config: **98.1% of pixels byte-identical**; the 1.9% residual is ±3-level edge pixels where depth-adjacent faces share rasterization edges (verified: ~100% of diff pixels have a tiny depth diff; same-path A/A is bit-exact; fragment shader proven byte-equivalent to `pbr_full.frag`) | capture diff + depth diff |
| Draw commands come from the GPU | indirect buffer is device-local (never host-mapped); the cull pass writes all commands from the mesh table | `setup_gpu_driven` |
| Frustum matches the view camera | cull planes derived from the same pure orbit formula + `tan(fov/2)` as `record_scene_into` | `write_gpu_driven_payload` |
| No CPU/GPU race on payload | one payload copy per swapchain image, indexed by `current_frame()` | descriptor sets |
| Composed lighting preserved | driven fragment shader uses the same shadow (set 4) + IBL (set 5) sets as `pbr_full.frag` | `pbr_gpu_driven_full.frag` |
| Self-describing runs | telemetry logs `draw_path` (`per_draw`/`gpu_driven`) and `scene_variant` (`mannequin`/`cubes`) events — A/B tooling never guesses | telemetry.jsonl |
| Regressions guarded | full ctest suite passes under validation; scenario runner 13/13 + byte-identical determinism | CI matrix |

## GPU timestamps + sustained benchmark (C3) — DONE

The renderer owns a timestamp query pool (2 queries per frame slot: TOP_OF_PIPE
at command-buffer start, BOTTOM_OF_PIPE after the main render pass — covering
the shadow pre-pass and cull dispatch). Results resolve on slot reuse (the
begin_frame wait guarantees the previous submit finished), surface through
`renderer.gpu_timing()`, and land in telemetry as `gpu_ns` on every frame
line. The analyzer gates GPU-timestamp plausibility; the scenario runner
strips `gpu_ns` from determinism comparisons (volatile by nature).

`scripts/benchmark_sustained.sh [frames]` runs both draw paths under
validation on identical config, gates each through the full analyzer, and
prints a CPU/GPU comparison table. Measured on RTX 2060, 600 frames,
cubes-only scene:

| path | record p50 | total p50 | GPU p50 |
|---|---|---|---|
| per-draw | 71 µs | 4.09 ms (vsync) | 151 µs |
| gpu-driven | **43 µs** | 9.99 ms (vsync) | 368 µs |

CPU submission cost drops ~40% on 3 objects; the GPU cull-pass overhead
(368 vs 151 µs) is fixed-cost and amortizes with instance count — the
crossover where GPU-driven wins outright is exactly what the E-phase
benchmark scenarios will measure.

## Composed lighting (C1) — DONE

The windowed viewport now renders the full composed stack by default; every
claim below is backed by a live run under `VK_LAYER_KHRONOS_validation`
(NVIDIA RTX 2060) with **0 VUIDs**.

| Claim | Proof | Source |
|---|---|---|
| IBL baked from our own analytic sky | `VulkanIblBaker` one-shot compute bake (equirect -> prefiltered cube + irradiance + BRDF LUT) feeds set 5 of the composed pipeline | `src/engine/render/vulkan_ibl_baker.cpp` |
| Shadow-mapped figure on ground | Shadow pre-pass renders depth-only into a 2048² map; `OMNICPP_DUMP_SHADOW` readback shows 45% occupied texels | `shadow_pre_pass_cb` + `shadow_skinned.vert` |
| Shadow footprint isolated pixel-exactly | `OMNICPP_NO_SHADOW=1` binds a neutral 1×1 cleared map; diff vs composed run = **10,874 px** darkened ≥2 levels | A/B capture diff |
| Composed vs legacy differ | 13.5% / 7.8% of pixels differ at frames 60/120; composed mean brighter (IBL ambient) | A/B capture diff |
| A/B mode logged, not assumed | telemetry records `lighting_mode`, `sun_direction`, `shadow_mode` events | `telemetry.jsonl` |
| Shadows follow the sun | `OMNICPP_SUN_DIRECTION` sweep: 24,696 px darken under sun B where sun A was lit | two-sun capture diff |
| Regressions guarded | 410/410 unit tests under validation; scenario runner 13/13 + byte-identical determinism double-run | CI matrix |

Diagnostic env vars (all telemetry-logged): `OMNICPP_LEGACY_LIGHTING=1`,
`OMNICPP_NO_SHADOW=1`, `OMNICPP_SUN_DIRECTION=x,y,z`,
`OMNICPP_DUMP_SHADOW=<frame>` (writes `/tmp/shadowmap.f32`).
