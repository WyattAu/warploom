- **Engine**; is it implemented in `modules/render`?
- **App**; can the shipped `warploom_viewport` actually reach it?
- **Proof**; what test exercises it, and where does that test run?
| `record_pbr_frame` (the module's own PBR frame) | yes | no; the app records its lit pass via `record_pbr_scene` inside the renderer's own render pass | `test_shadow_mapping`, `test_rt_shadows`, `test_rt_reflections` |
| `record_pbr_frame_gpu_driven` (one-submission GPU-driven frame) | yes | partial; the app drives the two halves separately (`record_gpu_driven_cull`, `record_gpu_driven_draw`) because it renders a shadow pass between them | `test_gpu_driven_frame`, `test_gpu_driven_occlusion_frame`; app A/B is 100% byte-identical |
| Post: fullscreen pass, ACES tonemap, FXAA | yes | yes; `RendererConfig::enable_hdr_compose`; the app renders into a float intermediate and tonemaps into the swapchain | `test_postprocessing`; app verified under validation in 8 configurations |
| Bloom (Karis downsample + tent upsample) | yes | yes; opt-in via `RendererConfig::enable_bloom` (`WARPLOOM_BLOOM=1`) | `test_postprocessing` |
| Shadow map: depth pre-pass, PCF, bias | yes | yes; via `record_shadow_pre_pass`, the hand-rolled copy deleted | `test_shadow_mapping`, `test_rt_shadows`; app A/B: identical texel occupancy, uniform bias shift |
| H-Z depth pyramid (built every frame) | yes | yes; `RendererConfig::enable_hiz`; contract checked on real frames by `HiZGraph.ContractHoldsOnARealFrame` (headless) and the swapchain golden test | tested |
| H-Z occlusion culling (consumer) | yes | yes; gd mode (`WARPLOOM_GPU_DRIVEN=1`, cubes scene via `WARPLOOM_NO_MANNEQUIN=1`); `WARPLOOM_OCCLUSION=0` disables at the push | `HiZGraph.PackedPyramidMatchesAbi` (packed ABI), `test_gpu_driven_occlusion_frame` (occluder-present/occludee-culled shader proof); live wiring proven by a forced-depth_min probe (74.7% of pixels change). The shipped demo scene is occlusion-sparse; identical frames on/off are correct there |
| SSAO, SSR, TAA, denoiser, volumetrics | no | no |; |
| Animated TLAS for skinned parts | partial | partial | one rigid transform per part from its dominant joint; not vertex-level skinning in the acceleration structure |
| Live socket proof harness  + `e1` (render mode/exposure/bloom wire contract)|; |; | `tools/live_proof.py all` → **64/64 assertions** |
never calls it, and the protocol has no physics commands; so replay
| Broadphase | no; an O(n²) double loop every step |
| ECS integration | no; `physics_world.hpp` documents an ECS bridge that does not exist; `test_physics_ecs_bridge.cpp` tests a helper that exists only in that test file |
| **Interpolation** | no; `TimelineClip::evaluate` takes the last sample at or before the frame, so playback steps between keys |
| Concurrent record and play tracks | no; both arms are single scalars |
| Clip strip widget in the app | no; `ClipTimelineView` exists and is unit-tested, and the app has only the W1 scrub strip |