`vkCreateDevice` only when the *effective* API covers them; chaining a 1.3 struct under an
- **Synchronization 2** (1.3); unlocks `vkQueueSubmit2` and stage/access-scoped submit infos
- **Timeline semaphores** (1.2); unlocks fence-free CPU/GPU frame pacing
- **Descriptor indexing** (1.2); the full bindless combination: runtime-sized arrays,
p99.9 / max** over the most recent window; tail latency, not averages.
- **Persistent mapping**: host-visible blocks return a stable `mapped` pointer; no
- **Job system (default for the engine frame path)**; band jobs fork onto the
- **Ad-hoc threads (fallback)**; original behavior; a thread per band per frame.
(`kSceneVertexFloats`); position.xyz, linear color.rgb, normal.xyz, uv.xy; fetched by
sets 0/1/2 and pushes a **160-byte PBR ABI**; `view_projection` (64) + `model` (64) +
`camera_position` (16) + `material_index` (4) + padding; with
indices travel inside the material record rather than the push block; objects sharing
with one fixed directional key light plus a small ambient; no IBL yet (an environment
- `VulkanHardware.PbrMetallicRoughnessDiscrimination`; a smooth metallic cube
- `VulkanHardware.PbrEmissiveAddsUnlitColor`; a black base factor with a red
- `VulkanHardware.PbrAlbedoTextureTintsLitCube`; a 1×1 red base-colour texture at
test), plus `update_orbit_cameras()`; a pure-arithmetic ECS system that ticks every
(multi-material glTF assets are now captured per-primitive at import; see below -
attributes default deterministically; color (1,1,1), normal (0,0,1), uv (0,0); so
(VEC3/VEC4 float32), `NORMAL` (VEC3 float32), `TEXCOORD_0` (VEC2 float32); optional
attributes must match the POSITION vertex count; `SCALAR` indices as uint16 or uint32
form; decoding never colour-transforms on the CPU; and each binding's
depth-first flattened into `GltfSceneImport`; one `Node` per mesh-bearing
and malformed-input rejections; pure CPU, so they run in every build flavor. The
red under lambert on the GPU while its untextured neighbour stays neutral; verified by
dimensions/pixels/sampler; the JPEG's constant colour decodes bit-exactly), those
cube's material to it turns the lit scene red on the GPU; while the plain material
stays neutral; under Khronos validation.
both through the lit scene path; readback shows red *and* green regions in one frame
PNG images decode on the CPU in `engine/asset/png_decoder.hpp`; a deterministic,
Malformed data is rejected with `RuntimeError::malformed_asset` and a diagnostic; bad
JPEG images decode on the CPU in `engine/asset/jpeg_decoder.hpp`; a deterministic,
diagnostic; nothing unsupported is ever mis-decoded. A 256 MiB decoded-size cap
golden blobs from an independent encoder (Pillow over libjpeg; constant-grey exact,
`VK_FORMAT_R8G8B8A8_SRGB` images; the hardware decodes to linear at sample time and
sRGB-format texture; while sampling the raw 186 as linear would read ~155.
(`vcpkg.json` "image", `OMNICPP_USE_STB`), but no build target links or uses stb; the
texture payloads in all three supported forms; embedded PNG/JPEG data URIs,
(an sRGB swapchain / final write encode; today rendering targets are UNORM and
- `VulkanHardware.ParallelRecorderContentionStress`; repeated multithreaded
- `SpirvReflector.*`; the reflector survives truncation, lying instruction
- `VulkanAllocator.RandomizedAllocFreePreservesDisjointness`; randomized
enabled; see `.github/workflows/test.yml`.
handoff inside one submission; no queue round-trip. Pipelines load through
accepted indices into a list, and **atomically maintains the indirect draw command** -
knowledge: render a wall+cube scene (D32 depth stored; `VulkanOffscreenTarget`
levels; a single dispatch cannot synchronize across workgroups, so inter-level
latency; compute for frame N+1 is submitted before graphics for frame N, and
included in the wait mask); no CPU round-trip, no CPU touch of per-frame draw
1. **Cross-vendor hardware runs** (AMD/Intel/mobile); requires physical hardware or a