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
| Skeletal glTF import (skins, node forest, animations) | rest pose reproduces the bind pose exactly; 7 malformed-document rejections; byte determinism | `test_gltf_animation` |
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

- **Skeletal animation in the viewport**: the whole path is now proven end
  to end offscreen — `assets/models/mannequin.gltf` ships on disk, imports
  with skins/animations, and its walk cycle deforms through the GPU skinned
  pipeline (`test_gpu_mannequin`). Wiring the animated figure into the
  windowed scene (bone upload inside the frame callback) is mechanical but
  not yet done.
- **Shadow/IBL/sky in the default viewport scene**: proven features whose
  viewport composition (multi-pipeline scene assembly) is pending.
- **GPU-driven frame in the viewport**: the renderer API exists and is
  proven; the app still uses the per-draw path.
