# Vulkan Rendering Stack

Modern Vulkan 1.3-first rendering layer, Qt-free, verified on hardware (RTX 2060) under the
Khronos validation layer with zero diagnostics, and exercised in CI on Mesa lavapipe
(software GPU).

## Layers

| Layer | Header | Purpose |
|-------|--------|---------|
| Context & feature negotiation | `engine/render/vulkan_context.hpp` | Instance, device, queues, Vulkan 1.2/1.3 feature chain |
| Swapchain | `engine/render/vulkan_swapchain.hpp` | Format/present-mode selection, recreation |
| Render pass | `engine/render/vulkan_render_pass.hpp` | Color+depth pass, framebuffers, depth resources |
| Pipeline | `engine/render/vulkan_pipeline.hpp` | SPIR-V loading, graphics pipeline, explicit layouts |
| Renderer | `engine/render/vulkan_renderer.hpp` | Frame loop, sync2 submission, timeline pacing, GPU telemetry |
| Memory allocator | `engine/render/vulkan_memory_allocator.hpp` | Block sub-allocation + staging upload ring |
| Descriptors | `engine/render/vulkan_descriptors.hpp` | SPIR-V reflection, layouts, pools, set writes |
| Render graph | `engine/render/vulkan_render_graph.hpp` | Automatic image barriers between passes |
| Parallel recorder | `engine/render/vulkan_parallel_recorder.hpp` | Multithreaded secondary command buffers |
| Offscreen target | `engine/render/vulkan_offscreen.hpp` | Headless color+depth targets with GPU readback |
| H-Z pyramid | `engine/render/vulkan_hiz_pyramid.hpp` | Runtime-sized sampled hierarchical depth resource |
| H-Z frame state | `engine/render/vulkan_hiz_frame_state.hpp` | A/B ownership, publication tokens, and invalidation |
| Scene snapshot | `engine/render/vulkan_scene.hpp` | Immutable camera/object snapshots for indexed scene submission |
| Depth convention | `engine/render/depth_convention.hpp` | Canonical projection and conservative sphere-depth equations |

## Feature Negotiation

`VulkanContext` requests the highest instance API the loader supports (up to 1.3), then
computes the **effective API** as `min(instance version, device version)`. Feature structs
(`VkPhysicalDeviceVulkan13Features`, `VkPhysicalDeviceVulkan12Features`) are chained into
`vkCreateDevice` only when the *effective* API covers them — chaining a 1.3 struct under an
effective 1.2 API is a spec violation (`VUID-VkDeviceCreateInfo-pNext-pNext`) that some
drivers tolerate and others (correctly) reject.

The context queries both feature structs in one chained `vkGetPhysicalDeviceFeatures2` call
and enables, only when the device advertises them:

- **Synchronization 2** (1.3) — unlocks `vkQueueSubmit2` and stage/access-scoped submit infos
- **Timeline semaphores** (1.2) — unlocks fence-free CPU/GPU frame pacing
- **Descriptor indexing** (1.2) — the full bindless combination: runtime-sized arrays,
  partially-bound sets, update-after-bind for sampled images and storage buffers, and
  non-uniform array indexing

Introspection: `has_synchronization2()`, `has_timeline_semaphores()`,
`has_descriptor_indexing()`, and `timestamp_period_ns` (device limits, for GPU timestamp
conversion).

Extensions such as `VK_KHR_synchronization2` are added to the device only when the
corresponding feature is actually enabled.

## Frame Submission

`VulkanRenderer::submit_frame()` has two paths selected by negotiated features:

- **Sync2 path**: `vkQueueSubmit2` with `VkCommandBufferSubmitInfo` /
  `VkSemaphoreSubmitInfo`, resolving the function pointer through the device.
- **Legacy path**: `vkQueueSubmit` with a `VkTimelineSemaphoreSubmitInfo` pNext chain when
  timeline values are signaled.

Both paths preserve per-image render-finished semaphore ownership.

### CPU Frame-Time Percentiles

The renderer records one CPU frame-time sample per successfully presented frame
(`begin_frame()` → present completion) into a fixed-capacity ring buffer
(`engine/core/latency_telemetry.hpp`, 4096-sample window, no allocation on the
record path). `frame_latency_stats()` returns nearest-rank **p50 / p90 / p99 /
p99.9 / max** over the most recent window — tail latency, not averages.
Recording can be disabled with `record_frame_latency(false)`.

### Timeline Frame Pacing

When `set_timeline_pacing(true)` is requested and timeline semaphores are negotiated:

- One global timeline semaphore signals a monotonic frame counter on every submit
  (`frame_counter()`).
- `begin_frame()` throttles CPU/GPU in-flight frames by waiting on timeline value
  `frame_counter − slots + 1` (no binary fences involved).
- Per-image reuse waits on the exact timeline value that last rendered that image.
- The per-frame fence is omitted entirely in this mode; the legacy fence path remains the
  fallback for devices without timeline semaphores.

### GPU Timestamp Telemetry

Timestamp query pools bracket the offscreen render pass; durations convert to nanoseconds
via `timestampPeriod`. On the RTX 2060 the 640×480 triangle pass measures ~6 µs.

## Memory: Allocator + Upload Ring

`VulkanMemoryAllocator` replaces one-`vkAllocateMemory`-per-resource with:

- **Block sub-allocation**: 64 MiB device-local / 16 MiB host-visible blocks carved by
  first-fit with alignment-aware free ranges, leading-pad reuse, and neighbor coalescing.
- **Memory-type selection**: best-match against `VkPhysicalDeviceMemoryProperties`, with
  block selection filtered by each resource's required `memoryTypeBits` (multi-heap GPUs).
- **Persistent mapping**: host-visible blocks return a stable `mapped` pointer — no
  per-object `vkMapMemory`.
- **API**: `create_buffer()`, `bind_image()`, `destroy_allocation()`, `stats()`.

`VulkanUploadRing` is a single persistently-mapped `TRANSFER_SRC` staging buffer used as a
ring: `acquire()` → CPU write → `record_copy()` → `submit()`. Each submit's fence guards
exactly the ranges handed out since the previous submit; wrap-around retires in-flight
fences for the tail region before reusing bytes.

## Descriptors + SPIR-V Reflection

`reflect_spirv_resources()` is a dependency-free SPIR-V parser that extracts set/binding/
type/count for UBOs, SSBOs, samplers, combined image samplers, storage images, and input
attachments. Stage flags derive from `OpEntryPoint` execution models and merge across
modules. Arrayed resources resolve their dimension: runtime arrays report a descriptor
count of 0 (expanded to a bounded capacity in bindless layouts), sized arrays report the
constant length. glslang's `readonly buffer` SSBO pattern (Uniform storage class with a
runtime-array member) is classified correctly as a storage buffer.

`VulkanDescriptorManager` builds layouts from reflected bindings, sizes its pool
automatically (one pool per layout, so bindless and regular layouts coexist), allocates
sets, and applies buffer/image writes.

### Bindless (descriptor indexing)

When the device advertises the required Vulkan 1.2 features (checked as a full set:
`descriptorIndexing`, `runtimeDescriptorArray`, `descriptorBindingPartiallyBound`,
update-after-bind for sampled images and storage buffers, and non-uniform indexing for
sampled-image and storage-buffer arrays), `has_descriptor_indexing()` is true and
`create_layout(bindings, sets, /*bindless=*/true)` creates:

- update-after-bind + partially-bound bindings (null descriptors legal at bind time,
  writes any time up to draw/dispatch submission)
- runtime-sized arrays in shaders (`float values[]`), bounded by the layout's capacity
- an `UPDATE_AFTER_BIND` pool backing persistent sets

The hardware test renders 8 palette bands through a runtime-sized SSBO array indexed by
push constants, with zero validation diagnostics.

## Render Graph

`compile_render_graph()` / `execute_render_graph()`:

- Passes declare color/depth attachments with explicit usage timelines.
- The compiler tracks per-image layout/access/stage state and emits exactly the required
  `VkImageMemoryBarrier`s between consecutive passes.
- The executor inserts barriers and records each pass via a callback in one command buffer.

## Multithreaded Command Recording

`VulkanParallelRecorder` splits the frame into horizontal bands and records one secondary
command buffer per band on worker threads, using per-thread command pools (Vulkan
external-sync rules) and correct `RENDER_PASS_CONTINUE` inheritance info. Secondaries
execute via `vkCmdExecuteCommands` inside one primary render pass.

Two execution backends, selected with `set_job_system`:

- **Job system (default for the engine frame path)** — band jobs fork onto the
  prioritized `JobSystem`'s persistent workers as allocation-free function pointers
  (`submit_raw`), joined with a `JobCounter`. No thread creation and no heap allocation
  per frame. Band 0 records on the calling thread while the rest run on workers.
- **Ad-hoc threads (fallback)** — original behavior; a thread per band per frame.
  Kept for embedders that do not run a job system.

The contention stress test exercises both backends in alternating waves.

## H-Z Renderer Integration Contract

`VulkanRenderer` can own a persistent H-Z pair when `RendererConfig::enable_hiz` is true.
The pair is sized from the swapchain extent, `hiz_tile_size`, and `hiz_levels` (zero means
all legal mips), and is rebuilt only while the device is idle. `hiz_frame_state()` exposes
A/B selection and invalidation state; `begin_hiz_frame()` returns a generation-checked
`HiZFrameToken`, and `complete_hiz_frame()` is the only operation that publishes a newly
reduced pyramid as eligible previous-frame data. Camera cuts and projection changes are
reported through `invalidate_hiz()`; swapchain resync invalidates and rebuilds the pair.

`set_hiz_record_callback()` remains available for application-specific culling and scene-buffer work. During
`record_commands()`, after the graphics depth pass, the callback receives `HiZFrameRecord`
with the actual depth source, the destination/previous pyramid pair, runtime dimensions,
and mip count. Returning false discards the token and fails the frame. The renderer does not
claim H-Z completion until the corresponding frame submission succeeds.

Set `RendererConfig::hiz_reduction_shader_path` to a compiled
`depth_reduce_image.comp` SPIR-V module to enable the renderer-owned direct reduction path.
The renderer then owns the compute pipeline, descriptor layout, and two per-pyramid/per-mip
descriptor-set families; it records depth sampling, one dispatch per mip, and explicit
write-to-read barriers directly in the frame command buffer. The callback, when installed,
runs after this reduction and is intentionally still responsible for application-specific
culling. This separation prevents a generic renderer from guessing the application's scene
buffer ABI while making the expensive depth reduction a first-class renderer operation.

`make_hiz_graph_plan()` converts that record into explicit per-mip `GraphComputePass`
metadata, including depth-source sampling, destination storage writes, previous-frame
sampling, runtime dispatch group counts, and external producer layout/access metadata.
Pass metadata can be compiled by `compile_graph()`; the direct reduction recorder and
custom culling callback share the same frame token and publication rules.

## Production Indexed Scene Submission

The first reusable scene-rendering seam is `VulkanScene` in
`engine/render/vulkan_scene.hpp`. An extraction system creates an immutable snapshot
containing a camera matrix and renderable objects. Each `SceneObject` refers to a stable
`SceneMesh` resource, which supplies a vertex-storage buffer, an index buffer, and a
material/mesh descriptor set. `VulkanRenderer::record_scene()` consumes that snapshot
inside an active render pass, binds each mesh, pushes its model matrix with the camera
view-projection matrix, and issues indexed `uint32` draws.

The scene snapshot is populated from gameplay state through `extract_vulkan_scene()`.
The ECS-facing components are `SceneCameraComponent`, `SceneRenderableComponent`, and
`SceneTransformComponent`. Extraction is const-only and copies matrices into the snapshot;
no mutable ECS references escape. Active cameras are selected by lowest priority and then
entity ID. Visible renderables are filtered and sorted by entity ID, giving deterministic
draw order. Missing transforms use identity. The resulting snapshot may be recorded on a
render thread while gameplay continues mutating the next ECS state, provided mesh resource
lifetime is protected by the renderer's frame/timeline retirement policy.

Renderables may carry optional `SceneBounds` (model space). When the active camera is
present, extraction builds a CPU frustum (`engine/render/frustum.hpp`, Gribb-Hartmann
planes from the view-projection matrix) and drops objects whose transformed box is fully
outside it; geometry without valid bounds is never culled. `SceneExtractionStats` reports
the pre-cull and culled counts for deterministic verification, and glTF-imported meshes
expose their POSITION min/max union as ready-made bounds (`GltfBounds`, valid only when
every merged primitive declared min/max).

This deliberately separates gameplay/ECS mutation from Vulkan recording. The renderer
does not retain snapshot pointers or destroy mesh resources. Resource registries must defer
GPU resource destruction until the relevant frame/timeline value has completed.

All scene meshes share one canonical vertex layout: **eleven floats per vertex**
(`kSceneVertexFloats`) — position.xyz, linear color.rgb, normal.xyz, uv.xy — fetched by
`gl_VertexIndex` from a mesh storage buffer, with one descriptor set per mesh, indexed
`uint32` draws, opaque depth-tested geometry, and a single graphics pipeline per snapshot.

The scene has two snapshot modes over that layout. The existing pointer path binds a
128-byte push-constant range (`view_projection`, `model`) and passes vertex color straight
to the frame. The handle-backed path is the lit material layer:
`VulkanSceneResourceRegistry` supplies generational mesh, material, and texture handles,
`extract_vulkan_scene(...)` copies the resolved records into the snapshot, and
`record_scene()` uses a 160-byte push-constant range (`view_projection`, `model`,
`base_color`, bindless `albedo_index`) with
`assets/shaders/indexed_scene_material.{vert,frag}.spv`. That fragment stage multiplies
the tinted vertex color by a directional lambert term computed from the interpolated world
normal and by an albedo texture sampled from the bindless set-1 array
(`albedos[albedo_index]`); element 0 is the opaque-white fallback, so untextured
materials sample it unchanged. Normals and UVs transform with the rigid/uniform-scale
model matrices documented by the scene path. Slot reuse is deferred by the registry's
`collect()` policy and must be gated by the frame/timeline retirement point.

Stable mesh/material/texture registries and per-frame upload ownership are in place:
`VulkanSceneResourceRegistry` provides generational handles with deferred slot reuse,
and `VulkanFrameUploadArena` (in `engine/render/vulkan_frame_upload.hpp`) keeps one
staging ring per frame slot so callers can wait on a slot, write staging memory, and
submit slot-local copies without exposing a raw Vulkan command buffer. Frustum culling
is integrated into extraction (see above).

Cameras are first-class deterministic systems: `engine/render/scene_camera.hpp`
provides `scene_camera_look_at()` / `scene_camera_projection()` /
`scene_camera_view_projection()` (right-handed look-at view space looking down -Z at
rest, fov_y-over-vertical projection, column-major, matching every hardware scene
test), plus `update_orbit_cameras()` — a pure-arithmetic ECS system that ticks every
entity carrying an active `SceneCameraComponent` and an `OrbitCameraController`
component from a deterministic `OrbitCameraInput` (yaw/pitch look, exponential zoom,
and frame-aligned target panning whose right/up axes match the look-at basis exactly)
and writes the recomputed matrix into `SceneCameraComponent::view_projection`. No
Vulkan, no clock, no randomness, so the same inputs always produce the same matrices
and the module runs in headless builds; feeding it real mouse/keyboard state is an
application-layer concern. What remains for a complete asset-driven renderer is
window/input plumbing to those controls and the standard GPU-driven culling path
(multi-material glTF assets are now captured per-primitive at import — see below —
and the scene path already renders one material per object, so splitting a mesh into
per-material draws is render-layer plumbing).

## glTF 2.0 Asset Ingestion

`import_gltf_mesh()` in `engine/asset/gltf_importer.hpp` converts a glTF 2.0 document
(JSON plus its single external `.bin`, or fully embedded `data:` URIs) into the
canonical engine mesh format consumed by the scene path: eleven floats per vertex
(position.xyz, linear color.rgb, normal.xyz, uv.xy) plus one triangle-list
`uint32` index stream. Material bindings are captured **per primitive**: the merged
index/vertex streams keep a `GltfPrimitiveMaterial` record for every glTF primitive
(contiguous index/vertex slice, material name, `baseColorFactor`, and optional albedo
texture reference into the import's shared `images`), so multi-material meshes import
completely and a render layer can split them into per-material draws without re-reading
the document. The mesh-level `base_color`/`albedo` fields alias the first primitive
that binds a material (the historical single-material convenience). Optional
attributes default deterministically — color (1,1,1), normal (0,0,1), uv (0,0) — so
positions-only geometry imports cleanly.

The importer is dependency-free, deterministic, and strict: a self-contained RFC 8259
JSON parser feeds a spec-conformant decoder that validates every reference against its
declared buffer before reading. Supported input is `POSITION` (VEC3 float32), `COLOR_0`
(VEC3/VEC4 float32), `NORMAL` (VEC3 float32), `TEXCOORD_0` (VEC2 float32) — optional
attributes must match the POSITION vertex count — `SCALAR` indices as uint16 or uint32
(non-indexed primitives become sequential indices), tightly packed or
`byteStride`-interleaved buffer views, and multi-primitive meshes merged into one draw
with re-based indices. Malformed documents are rejected with
`RuntimeError::malformed_asset` and a diagnostic instead of being mis-decoded: sparse
accessors, non-TRIANGLES modes, unsupported component types or attribute shapes,
out-of-range indices, regions beyond their buffer view, strided index views, any
external buffer beyond the first, and out-of-range material references on *any*
primitive all fail loudly.

Texture wiring is part of the same import: optional glTF `samplers`/`textures`/`images`
arrays are validated eagerly (types, ranges, sampler filter/wrap enums, image source
rules), and a material's `pbrMetallicRoughness.baseColorTexture` on `TEXCOORD_0` is
decoded **lazily** into RGBA8 `GltfImage`s carried on the import, with the
declared sampler state in glTF enum numbers. Bytes stay in the payload's *encoded*
form — decoding never colour-transforms on the CPU — and each binding's
`encoded_srgb` flag records that glTF `baseColorTexture` is an sRGB colour texture
to be sampled through an sRGB image format (hardware linearisation; see the colour
section below). Every primitive's material is processed: a texture shared by several
primitives of one mesh decodes exactly once, and each primitive record's
`albedo.image_index` names the shared image. Supported image payloads are PNG
(`data:image/png;base64,...`) and baseline-JPEG (`data:image/jpeg;base64,...`) URIs,
tightly packed (stride-free) bufferViews over the existing buffers, and **external
image files** whose non-`data:` URI is resolved lazily through an optional
`ExternalFileLoader` callback (the importer stays filesystem-free; without a loader,
referencing an external file is a diagnostic error). Payloads are dispatched on magic
bytes by `decode_image()`; non-zero texCoord sets, ambiguous images (uri + bufferView
together), and out-of-range texture/image/sampler/material references are rejected
with diagnostics. Payloads referenced by *unused* meshes never decode (and external
files they reference never reach the loader), so a document mixing supported and
unsupported image formats still imports for meshes that do not bind them.

Whole scenes import through `import_gltf_scene()` (same inputs as
`import_gltf_mesh`, plus the `scenes`/`nodes` arrays): the hierarchy is parsed
(`matrix` XOR TRS transforms, shared-subtree DAGs, mesh references), validated
(node/child/mesh/scene ranges, cycles, skinned nodes, matrix+TRS conflicts), and
depth-first flattened into `GltfSceneImport` — one `Node` per mesh-bearing
instance carrying its absolute column-major `model` (pure group nodes contribute
transforms only, shared meshes import once and instantiate per path). Those
world models drop straight into `SceneTransformComponent`, so a whole `.gltf`
scene becomes a set of drawables with no manual transform math.

Ingestion is covered two ways. `GltfImporter.*` unit tests exercise valid fixtures
(all four attributes, interleaving, merging, base64, mesh selection, data-URI and
bufferView texture decode, sampler round-trip/defaults), encoding variants,
and malformed-input rejections — pure CPU, so they run in every build flavor. The
end-to-end `VulkanHardware.GltfImportedSceneWithRegistryMaterials` test builds two cubes
as glTF in memory with real per-vertex normals, imports them, uploads the streams,
registers mesh + material handles, and renders through the 160-byte lit material ABI
(with the opaque-white bindless fallback bound at albedo element 0): a red
vertex-colored cube occludes a white-vertex cube tinted green purely by its material
`baseColorFactor` under directional lambert shading; depth ordering and animation are
verified by pixel readback, and the run is clean under Khronos validation. The companion
`VulkanHardware.AlbedoTextureTintsLitMaterialScene` test proves the albedo texture path
itself: two cubes share one white-vertex mesh, and switching one cube's material to a
registry texture record pointing at a solid-red 1x1 (bindless element 1) turns that cube
red under lambert on the GPU while its untextured neighbour stays neutral — verified by
readback with zero validation diagnostics. `VulkanHardware.GltfSceneGraphRendersInstances`
proves the scene import on the GPU: one cube mesh shared by a two-node hierarchy renders
as two world-space instances (left green root, right red child whose model inherits the
root translation), verified by readback with zero validation diagnostics.

`VulkanHardware.GltfBaseColorTextureEndToEnd` and
`VulkanHardware.GltfJpegBaseColorTextureEndToEnd` close the loop for real glTF files:
the in-memory glTF embeds a solid red PNG / JPEG behind `baseColorTexture`, the
importer decodes it through the magic-dispatched codecs (CPU assertions on
dimensions/pixels/sampler — the JPEG's constant colour decodes bit-exactly), those
exact bytes upload through the arena into a registry texture record, and switching the
cube's material to it turns the lit scene red on the GPU — while the plain material
stays neutral — under Khronos validation.

Multi-material meshes are proven on the GPU two ways.
`VulkanHardware.MultiMaterialGltfMeshRendersBothPrimitiveTextures` imports one glTF
mesh whose two primitives each bind their own solid-colour baseColorTexture (red,
green data URIs), asserts every primitive record's material/texture wiring at the
asset layer, uploads each primitive's vertex/index slice as its own mesh, and renders
both through the lit scene path — readback shows red *and* green regions in one frame
under zero validation diagnostics.
`VulkanHardware.MultiMaterialMeshSplitsIntoSharedBufferDraws` proves the shared-buffer
form of the same split: the external-file loader feeds a red PNG and a green PNG to the
import, both primitives' slices are drawn from one merged vertex/index buffer via byte
`index_offset`s, and the per-material draws land in the same frame.

## Image Decoding + GPU Texture Upload

PNG images decode on the CPU in `engine/asset/png_decoder.hpp` — a deterministic,
dependency-free decoder (self-contained RFC 1951 DEFLATE plus the PNG chunk layer)
that turns file bytes into tightly packed 8-bit RGBA (`DecodedImage`, straight alpha).
Supported input is the spec-relevant surface: colour types 0/2/3/4/6 at 8-bit, all five
scanline filters, PLTE + tRNS (palette alpha and grey/truecolour keys), one or many
consecutive IDAT chunks, and stored / fixed-Huffman / dynamic-Huffman DEFLATE blocks.
Malformed data is rejected with `RuntimeError::malformed_asset` and a diagnostic — bad
signatures, per-chunk CRC failures, unknown *critical* chunks, truncated framing,
interlaced (Adam7) or non-8-bit images, palette-index overruns, invalid zlib headers
(preset dictionaries included), over-subscribed Huffman tables, and matches beyond the
emitted output never mis-decode. A 256 MiB decoded-size cap bounds hostile dimensions.
Coverage runs everywhere (`PngDecoder.*`, 33 CPU tests): golden streams produced by an
independent encoder (Python zlib: stored, fixed-Huffman, dynamic-Huffman) plus
real-world Pillow files, with exact pixel comparisons.

JPEG images decode on the CPU in `engine/asset/jpeg_decoder.hpp` — a deterministic,
dependency-free **baseline sequential** decoder (self-contained Huffman decode,
0xFF00 byte-stuffing, zigzag + dequantisation, float IDCT, BT.601 YCbCr→RGB, and
triangle "fancy" chroma upsampling for 2x ratios with edge replication, matching
libjpeg's default upsampler). Supported input: SOF0 8-bit frames with one component
(grayscale) or three in the JFIF YCbCr convention, per-component sampling factors up
to 4 with whole-number ratios (4:4:4, 4:2:2, 4:2:0, 4:1:1), 8-bit quantisation tables,
restart intervals (DRI/RSTn), and APPn/COM segments. Progressive (SOF2), extended
sequential / 12-bit (SOF1), arithmetic-coded frames, non-8-bit samples, fractional
upsampling ratios, out-of-range table/component references, truncated entropy data,
and a missing EOI are all rejected with `RuntimeError::malformed_asset` and a
diagnostic — nothing unsupported is ever mis-decoded. A 256 MiB decoded-size cap
bounds hostile dimensions. Coverage runs everywhere (`JpegDecoder.*`, 12 CPU tests):
golden blobs from an independent encoder (Pillow over libjpeg — constant-grey exact,
a 4:4:4 colour ramp and a 4:2:0 two-colour image bounded against the analytic source)
plus strict malformed-input rejection; importer-level tests prove
`data:image/jpeg;base64` payloads decode through the glTF pipeline.

**Colour space:** glTF `baseColorTexture` bytes are sRGB-encoded and lighting must
happen in linear space, so colour textures are uploaded as
`VK_FORMAT_R8G8B8A8_SRGB` images — the hardware decodes to linear at sample time and
no CPU colour transform is ever applied (an 8-bit CPU linearisation would quantise).
Non-colour data textures (normal/ORM) will use plain UNORM. The importer marks each
binding's colour role (`GltfMaterialTexture::encoded_srgb`) so uploaders choose the
format; sampling an encoded value as raw-linear is the bug this convention prevents.
`VulkanHardware.SrgbBaseColorTextureLinearisesAtSample` proves it on the GPU: an
encoded grey 186 decodes to linear 0.491021, and the lit scene's frame-centre pixel
reads ~104 whether that linear value arrives as a material factor or as an
sRGB-format texture — while sampling the raw 186 as linear would read ~155.

The decoded RGBA then travels the GPU path through the frame upload arena:
`VulkanFrameUploadArena::record_copy_image_rgba8()` stages the span into a fresh
`TRANSFER_DST | SAMPLED` image, records `UNDEFINED -> TRANSFER_DST ->
SHADER_READ_ONLY_OPTIMAL` transitions with matching access/stage masks on the
ring-owned command buffer, and leaves the image sample-ready after submit.
`VulkanHardware.PngDecodedTextureUploadAndSample` proves the whole chain under Khronos
validation: it builds an 8x8 PNG in memory, decodes it, uploads it through the arena,
binds view + sampler in a descriptor set, and renders it with a UV-mapped textured
quad; every one of the 64 texels is verified 1:1 (RGBA, alpha included) at its output
location by readback.

Packaging note: the repository's dependency manifests advertise an `stb` image feature
(`vcpkg.json` "image", `OMNICPP_USE_STB`), but no build target links or uses stb — the
image pipeline above is the engine's own decoder and is what the tests exercise. Registry
textures are already sampled by the lit scene material path (bindless set 1, verified on
the GPU), glTF `images`/`textures`/samplers (`baseColorTexture`) are decoded and wired
into registry texture records, and whole `scenes`/`nodes` hierarchies flatten into
world-space instances (see the glTF section). Real-world glTF assets now load
texture payloads in all three supported forms — embedded PNG/JPEG data URIs,
bufferView images, and external files through the loader callback. Colour textures
are sampled through sRGB image formats so lighting happens in linear space (below).
The per-primitive *splitting* of a multi-material mesh into separate draws is
already GPU-proven (see the two `MultiMaterial*` tests in the glTF section: slice
draws and shared-buffer submesh records over one vertex/index buffer); what remains
for real-world assets is engine plumbing that performs that split automatically
from a single import instead of test-side records, the output side of the pipeline
(an sRGB swapchain / final write encode — today rendering targets are UNORM and
shaders write linear values straight), and the GPU-driven culling path.

## Swapchain Recreation

`VulkanSwapchain::recreate()` rebuilds image views safely;
`VulkanRenderer::resync_for_swapchain()` rebinds the swapchain and render pass, rebuilds
per-image semaphores, and resets timeline history while the device is idle. Mid-frame
resync is rejected. When renderer-owned H-Z is enabled, resync also retires both pyramids,
recreates them at the new extent, and invalidates previous-frame visibility data.

## Validation Gate

Hardware tests run under the Khronos validation layer, forced via
`VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation` when
`OMNICPP_VULKAN_VALIDATION_TESTS=ON`. Any diagnostic fails the suite.

Key hardware tests (`tests/unit/test_rendering.cpp`):

- `VulkanHardware.AllocatorAlignmentPadSubAllocation`
- `VulkanHardware.BindlessDescriptorIndexingRender`
- `VulkanHardware.SwapchainRecreationStress`
- `VulkanHardware.OffscreenTriangleReadback`
- `VulkanHardware.HeadlessSwapchainAndRenderSubmission`
- `VulkanHardware.RendererIndexedSceneSubmission`
- `VulkanHardware.DescriptorReflectionAndUboRender`
- `VulkanHardware.RenderGraphTwoPassBarriersAndRender`
- `VulkanHardware.ParallelRecorderMultithreadedBands`
- `VulkanHardware.ParallelRecorderContentionStress` — repeated multithreaded
  recording waves with band count above core count (TSan pressure test)
- Allocator/upload-ring sub-allocation and byte-verification tests

Robustness suites (`tests/unit/test_reflector_and_allocator_robustness.cpp`):

- `SpirvReflector.*` — the reflector survives truncation, lying instruction
  word counts, and 2000 iterations of random byte corruption of a real shader
  without out-of-bounds reads
- `VulkanAllocator.RandomizedAllocFreePreservesDisjointness` — randomized
  alloc/free sequences (fixed seed) verified for non-overlap and full
  reclamation

Tests skip gracefully when no Vulkan loader, display, or compiled shaders are present, so
the same binary runs on GPU-less CI machines and full-GPU workstations.

## Building

```sh
# Configure (requires Vulkan SDK or distro packages: libvulkan-dev, glslc or glslangValidator)
cmake --preset vulkan-validation
cmake --build build/vulkan-validation
ctest --test-dir build/vulkan-validation --output-on-failure
```

Shader compilation prefers `glslc` (shaderc) and falls back to
`glslangValidator -V` where glslc is not packaged.

CI runs this exact preset on Mesa lavapipe (software GPU) with the validation layer
enabled — see `.github/workflows/test.yml`.

## Compute

`VulkanCompute` (`engine/render/vulkan_compute.hpp`) dispatches compute pipelines on the
compute-capable queue with event-based synchronization: record a dispatch, signal an
`VkEvent`, then have graphics waits consume it (`wait_events`) for a compute-to-graphics
handoff inside one submission — no queue round-trip. Pipelines load through
`VulkanPipeline::load_shader_stage_file(..., "compute")` and
`create_compute_pipeline`. Covered by `VulkanHardware.ComputeToGraphicsEventHandoff`
(compute fills a storage buffer; a graphics pipeline renders it; the result is verified
by pixel readback) with zero validation diagnostics.

### Async Compute Overlap

`AsyncComputeQueue` (same header) drives a **dedicated compute queue** when the device
exposes one (NVIDIA dGPUs do): a fence ring allows compute frame N+1 to be recorded while
frame N is still in flight, a `VkSemaphore` of type TIMELINE hands results to graphics, and
buffer ownership between compute/graphics families is managed explicitly
(`release_buffer_to`/`acquire_buffer_from`) for exclusive-mode resources. The context
enables `VK_FEATURE_TIMELINE_SEMAPHORE` and selects a compute-only family when available.
Hardware-verified by `VulkanHardware.AsyncComputeTimelineHandoff`: compute generates and
animates triangle vertices for frame N+1 while graphics renders frame N, with the vertex
data validated against the analytic rotation.

## GPU-Driven Rendering

The `cull_and_draw.comp` + `gpu_objects` pipeline implements a CPU-independent draw path:
compute culls each instance's bounding sphere against view-frustum planes, compacts
accepted indices into a list, and **atomically maintains the indirect draw command** —
graphics executes `vkCmdDrawIndirect` and the vertex shader resolves
`compacted[gl_InstanceIndex]`. The CPU never touches per-instance draw data; it only
publishes instance state. `VulkanHardware.GpuDrivenCullIndirectDraw` verifies both
directions by readback (an all-visible frame is populated; moving every sphere behind the
camera empties the frame to 0 pixels), and `VulkanHardware.SustainedGpuDrivenFrameBenchmark`
runs a 300-frame animated loop, reporting frame-CPU percentiles (p50/p90/p99/p99.9/max)
via `LatencyTracker`.

## 3D Objects: Depth, Transforms, Animation

`VulkanOffscreenTarget::create_depth()` adds an optional depth attachment (D32 or D24S8,
format-capability checked); the render pass gains a depth attachment with clear-to-1.0.
Cube meshes are drawn with model + view-projection matrices through push constants,
depth-tested, backface-culled, and animated. `VulkanHardware.CubeMeshDepthOcclusionAnimation`
verifies by pixel classification: the near cube occludes the far one, swapping depths
swaps the visible color both ways, and a 45-degree rotation changes the rendered image.

## Mixed Render/Compute Graph

`compile_graph`/`execute_graph` (same header as the render graph) extend the pass
sequence with `GraphComputePass` nodes and `GraphBufferEdge` dependencies declared on
the **consumer** node (the barrier runs before the consumer's work). Edges whose
producer and consumer families differ emit release/acquire ownership halves based on
the `current_family` argument; same-queue edges are plain buffer barriers. Compute
passes record through a callback outside any render pass.
`VulkanHardware.RenderGraphComputeThenDraw` drives compute-generated animated geometry
through the graph and verifies both the buffer content analytically and the render by
pixel readback.

## GPU-Driven Scale-Up: LOD + Occlusion + Counters

`cull_lod_occlude.comp` combines per-instance frustum culling, tile-based occlusion
against the previous frame's reduced depth pyramid (1-frame latency, conservative),
distance-band LOD selection with per-instance bias, per-band compaction lists, and
per-band indirect draw commands. Atomic counters report accepted/frustum-culled/
occlusion-culled per frame for lock-free readback. `depth_reduce.comp` reduces a depth
source to per-tile maxima. `VulkanHardware.GpuLodOcclusionCounters` verifies occlusion
rejection (stats + empty draw commands), band assignment, bias forcing, and
boundary-crossing animation on hardware.

The cull shader computes each sphere's nearest-point depth with the **exact**
GL-projection mapping (`c1 - c2/view_d`, constants from the `near_z`/`far_z` push
constants), so occlusion comparisons against real rendered depth are exact rather
than a linear proxy. The pyramid lives at a caller-chosen word offset
(`pyramid_off` push constant) subject to storage-buffer offset alignment. The
`occl_mode` push constant selects between the legacy 2x2-tile center test and a
**footprint-adaptive** test: the sphere's projected bounding square picks the
mip level whose texel size best matches it (footprint spans <= 2x2 texels
there), so small occluders can hide behind small geometry and large occluders
are tested against coarser, cheaper texels.

## Real-Depth Occlusion: End-to-End Pyramid

`VulkanHardware.RealDepthPyramidOcclusion` closes the loop with no CPU depth
knowledge: render a wall+cube scene (D32 depth stored — `VulkanOffscreenTarget`
depth attachments use `STORE` so the pyramid path can copy them), copy the actual
depth attachment to a buffer (`vkCmdCopyImageToBuffer`), reduce it on the GPU, then
cull against that pyramid. The cube behind the wall is occlusion-culled by real
pixel data; the front cube survives. Tile maxima are verified against the analytic
projection mapping.

## Mip-Chained Pyramid + Footprint-Adaptive Occlusion

`depth_reduce_mips.comp` builds a 4-level MAX pyramid from the copied depth
buffer: 8x8 tiles of 32px texels, then 4x4 @ 64px, 2x2 @ 128px, and a 1x1 whole-
frame max (85 words total). One level per dispatch with a buffer barrier between
levels — a single dispatch cannot synchronize across workgroups, so inter-level
visibility must come from explicit barriers. Level 0 verifies analytically
against the projection mapping; level 3 equals the nearest rendered depth in the
frame.

`VulkanHardware.MipPyramidOcclusionAdaptive` renders the wall scene, builds the
chain on the GPU, and culls with both modes (adaptive occludes the hidden cube,
legacy mode still passes) with zero validation diagnostics.

## Dual-Queue Pipelined Rendering

`VulkanHardware.DualQueuePipelinedGraph` drives the steady-state two-queue
pipeline the async-compute primitives were built for: the compute queue produces
NEXT frame's GPU-driven draw state (per-frame ring generation, then GPU-side
frustum culling that maintains the indirect draw's instanceCount), while the
graphics queue renders frame N's already-culled state. Ordering is one frame of
latency — compute for frame N+1 is submitted before graphics for frame N, and
graphics waits on compute N's timeline value (`VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT`
included in the wait mask) — no CPU round-trip, no CPU touch of per-frame draw
data after setup. Per-frame command buffers + fences are required: re-recording
or fence-resetting while frame N is still pending violates
VUID-vkQueueSubmit-pCommandBuffers-00071 / VUID-vkResetFences-pFences-01123.
Verified by per-frame analytic readback (positions exact per frame),
cull-produced instanceCounts (2 / 1 / 2 including a partial-cull frame), and a
skipped path on devices without a COMPUTE-only family (CI's lavapipe).

## Scene Rendering: Objects, Lighting, Animation

`scene.vert`/`scene.frag` render instanced cubes and a ground slab with per-instance
data pulling, lambert + Blinn-Phong directional lighting, and an **analytically
ray-traced sphere** in the fragment stage (depth ordering between the sphere and the
rasterized geometry falls out of the per-pixel ray hit).
`VulkanHardware.Scene3DObjectsLightingAnimation` verifies by pixel classification:
the lit ground dominates the lower frame, the sphere renders red-dominant at the frame
center in front of the cube behind it, orbiting a cube changes the image while static
elements stay identical, and the animation round-trips to a byte-identical frame.

## Sampled H-Z Pyramid + Previous-Frame Ping-Pong

`VulkanHiZPyramid` owns a device-local `R32_SFLOAT` image with a runtime-sized mip
chain (`mip_levels_for_extent(width, height)`) and one nearest sampler. Passing
`levels = 0` to `create()` requests the complete chain; explicit shorter chains
are accepted only when they do not exceed the extent's legal mip count.
`depth_reduce_image.comp` samples the actual D32 depth
attachment for level 0 and max-reduces each subsequent 2x2 mip level on the GPU.
Each destination mip is transitioned explicitly to `GENERAL` for image stores,
then to `SHADER_READ_ONLY_OPTIMAL` before the next level; no workgroup-global
synchronization assumption is used. `GraphImageUse` lets the mixed render/compute
render graph declare these transitions per mip, so unrelated levels do not receive
unnecessary whole-image barriers.

`VulkanHiZFrameState` owns the CPU-side policy for the A/B resources: it computes
runtime pyramid dimensions, selects the current write and previous read indices,
and invalidates previous depth on first use, resize, camera cuts, projection changes,
or explicit reset. A frame token must be completed only after reduction finishes;
stale tokens cannot publish a partially recorded pyramid.

The canonical forward-Z equations live in `depth_convention.hpp`: view-space
near/far distances map to the same depth convention used by the culler, and H-Z
stores maximum depth values so visibility rejection remains conservative.

`cull_hiz_sampled.comp` consumes the completed pyramid as a sampled image,
selects a mip from the projected sphere footprint, samples every covered texel,
and rejects only when all sampled max-depth values are nearer than the sphere's
nearest point. Two pyramids are ping-ponged: frame N writes one while culling
uses the completed opposite image from frame N-1. `VulkanHardware.SampledHiZPreviousFramePingPong`
verifies three cycles under Khronos validation with zero diagnostics.

## Remaining Roadmap

1. **Cross-vendor hardware runs** (AMD/Intel/mobile) — requires physical hardware or a
   GPU CI service; the lavapipe CI job covers driver-independent correctness.
2. Integrate the runtime-sized resource into the production render graph and
   descriptor-backed per-frame resource tables.
3. Add hierarchical early-out traversal and depth-dilation policies for large,
   partially covered footprints while preserving conservative visibility.
