# GPU-driven rendering: design and migration plan

## Goal

One submission per frame; the CPU records a fixed command sequence and
never touches per-object state at record time. Culling, LOD selection, and
draw-command generation run on the GPU; the graphics queue consumes them
via indirect draws.

## Current blockers (measured in the code today)

1. **Per-mesh descriptor set.** `SceneMesh::descriptor_set` is bound per
   draw inside `record_pbr_scene` (set 0). An unattended GPU draw list
   cannot perform `vkCmdBindDescriptorSets`.
2. **CPU LOD readback.** `VulkanPbrScene::lod_results` is a mapped pointer
   read at record time. This forces selection -> fence-wait -> record: the
   selection can never be a same-frame graph node.
3. **Per-object push constants.** The 160-byte PBR push carries `model`
   and `material_index`, written per draw at record time.

## Target frame ABI

### Buffers

| Buffer | Writer | Contents |
|---|---|---|
| Object payload SSBO | CPU per scene change (or a transform compute pass) | per object: model matrix (64 B), material_index (4), mesh slot (4), bounding sphere (16), pad to 96 B |
| Mesh table SSBO | CPU at asset load | per mesh slot: index_count, index_offset (shared index buffer), vertex buffer reference, vertex count |
| Shared index buffer | CPU at load | every mesh's indices concatenated (offsets live in the mesh table) |
| Draw command buffer | **GPU** (cull/LOD pass) | per object: `VkDrawIndexedIndirectCommand` (20 B), `indexCount = 0` when culled |
| Visibility/count buffer | **GPU** | visible count, per-object LOD word (kept for debugging/readback tests) |

### Shaders

- **Vertex-pull PBR variant** (`pbr_gpu_driven.vert`): no vertex
  attributes. `gl_InstanceIndex` selects the object payload; the payload's
  mesh slot selects the mesh table entry; positions/indices are pulled
  from SSBOs (buffer device address, or a fixed-capacity SSBO array when
  BDA is unavailable). Model matrix comes from the payload, not push
  constants. Camera/view-projection moves to a frame UBO.
- **Cull + LOD compute** (`cull_and_draw_lod.comp`): frustum test +
  projected-size LOD selection (reuses `lod_select.comp` logic), then
  writes the selected mesh variant's `index_count`/`index_offset` into the
  object's draw command, or `indexCount = 0` when culled. One atomic max
  on the visible counter for telemetry.
- **Draw**: `vkCmdDrawIndexedIndirect(draw_cmd_buffer, 0, object_count,
  20)`. Degenerate commands skip culled objects. `vkCmdDrawIndexedIndirectCount`
  is an optional optimization (skips degenerates entirely) behind a
  feature check with the degenerate form as fallback.

### What already works unchanged

- Materials: set 2 is already an indexed SSBO; the payload's
  `material_index` feeds it directly.
- Textures: set 1 is already a bindless array.
- Barriers: the cull -> draw dependency is a `GraphBufferEdge`
  (compute write -> vertex/indirect read) on the existing render graph;
  the executor already emits it, including across queue families.

## Migration steps (each independently shippable + GPU-tested)

1. **Mesh table + shared buffers.** Registry gains mesh dedup into a
   shared index buffer; CPU-drawn path unchanged. Proof: existing suite
   green, mesh table readback matches per-mesh metadata.
2. **Vertex-pull A/B.** New pipeline renders the standard test scene
   alongside `record_pbr_scene`; readback hashes must match exactly.
3. **GPU cull/LOD -> indirect.** Compute pass writes draw commands;
   graphics uses `vkCmdDrawIndexedIndirect`. Proof: the LOD integration
   test's assertions move from buffer words to pixels (near cube full
   size, far cube low-LOD, culled cube absent) with no CPU selection.
4. **Async compute + drawIndirectCount.** Cull moves to the compute queue
   (family-transfer edge already supported); count-based draws behind
   `VK_KHR_draw_indirect_count` availability.

## Non-goals / notes

- Multi-draw-per-object (multi-submesh) comes later via a command
  amortization pass; single command per object is the correct v1.
- ~~Skinned objects: the skin palette SSBO join keys off the payload; the
  vertex-pull shader branches on a payload flag. Deferred to step 5.~~
  DONE as R3 (loop 26): `pbr_gpu_driven_skinned.vert` — the palette join
  key rides payload word 23 (the pad word; cull/payload/buffer sizes
  untouched), the skin stream runs parallel to the shared vertex stream
  (set 0 binding 4; mesh table format untouched), and the scene is
  partitioned static-first/skinned-second with one indirect draw per
  partition (firstInstance stays global) instead of the sketched
  per-vertex branch — so `pbr_gpu_driven.vert` is byte-frozen and its A/B
  proof stands. Proven by `GpuDrivenSkinnedMatchesCpuSkinnedPixels`
  (byte-identical hash + two sabotage directions + restore check).
  Live in the viewport (loop 26): the driven cubes scene carries an
  animated two-segment beam prop — own table slot + skin stream, own
  2-joint palette (identity + T(pivot)*Rz(a)*T(-pivot), a pure function of
  sim time), always the last driven object so the static partition is
  [0, beam) and the skinned partition is [beam, 1). Mannequin-scale driven
  scenes (one indirect draw per part) remain a follow-up; the engine
  supports N skinned objects and the viewport demonstrates one. Known
  limit: the beam receives shadows but does not cast them (the shadow
  pre-pass draws the CPU-built scene, which has no beam mesh); the parity
  proof is shadowless on both paths, so the claim stands as stated.
  Live proof (RTX 2060, 200-frame driven run, 8 captures, 0 diagnostics):
  baseline-vs-beam captures are pixel-identical outside a 61x253 beam
  bbox; in all 8 captures 100% of beam pixels are blue (material 1) and
  80-100% lie within 25 px of the analytically projected bent segment
  (exact camera math from the logged sim time, so the projection is a
  prediction, not a fit); the top-half screen displacement tracks the
  palette bend 6.6 -> 30.8 px as a(t) goes 0.12 -> 0.35. A pre-existing
  trait the A/B confirmed: the driven path renders no sky (black
  background in both baseline and beam runs) — the gd branch replaces
  record_pbr_scene, which is where the sky is drawn.
- The CPU LOD readback path (`lod_results`) stays supported for
  determinism tests and CPU-cull scenarios; the GPU path is additive.
