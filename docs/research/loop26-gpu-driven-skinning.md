# Loop 26 — GPU-driven skinned draws (R3)

## Source

- Linear-blend skinning from a palette SSBO indexed per-vertex: standard
  real-time practice, canonically documented in **Real-Time Rendering, 4th
  ed. (Akeniune-Möller, Haines, Hoffman; CRC Press, 2018)**, Chapter 17
  (skeletal animation / skinning). Technique status: settled, not novel.
- Vertex-pull (SSBO-addressed) geometry + GPU-generated indirect draws: the
  migration this repo already shipped (docs/gpu-driven-rendering.md, steps
  1–4; B4). Status: in-tree and proven by A/B parity tests.
- RT-skinned outlook (tetrahedral cage, Gruen et al., HPG 2026,
  10.1145/3820014) recorded in loop 11: explicitly NOT adopted — R3 has no
  RT requirement, and the cage technique answers a different question
  (per-frame BLAS cost) than R3 asks (draw-path parity for skinned actors).

## The claim being borrowed

Skinned vertex shading = `world = model * Σ palette[joint_base + j_i] * w_i
* pos`, with the palette join key (joint_base) carried per object. Nothing
about this is new; the claim that matters for R3 is narrower: the join key
can ride the EXISTING 24-word driven payload's pad word (word 23), so the
cull shader, the payload writer, and the buffer sizes are untouched, and
the skin stream can run PARALLEL to the shared vertex stream (8 floats per
vertex in the same vertex order), so the mesh table format and every cull
binding are untouched.

## Novelty assessment

Not novel. This is two published/shipped techniques joined at documented
seams: in-tree vertex-pull driven draws + textbook palette skinning. What
is specific to this engine is the seam choice (payload word 23, parallel
skin stream, partitioned static/skinned indirect draws instead of a
per-vertex branch), which is an engineering decision, not an invention.
Stated plainly so a future reader does not mistake it for one.

## Why it fits here, and what it costs

- Fits: the driven path already resolves material/payload per instance; the
  bone SSBO + slot-3 convention already exists for the CPU skinned path, so
  one bone set serves both. The cull pass never reads the skin stream, so
  its correctness argument is unchanged — the CPU only owes a conservative
  (pose-covering) bounding sphere per skinned object, exactly like the
  static case.
- Costs: one vertex shader variant (`pbr_gpu_driven_skinned.vert`), one
  extra SSBO binding (set 0, binding 4), 32 bytes/vertex of skin stream
  (identity for static meshes — the CPU path's trick, so one shader covers
  both), one extra indirect draw per frame, and a per-frame bone-buffer
  write the viewport already does for the mannequin. No pipeline rebuild on
  toggle: the two pipelines are built once, like the RT stack.
- Determinism consequences: none. Skinning inputs (payload matrices, bone
  palette, joint_base) are snapshot data; the shader is a pure function of
  them. No wall clock, no atomics beyond the existing visible counter.

## Design decision recorded (deviation from the plan)

docs/gpu-driven-rendering.md step 5 sketched "the vertex-pull shader
branches on a payload flag". Shipped instead: TWO pipelines with the scene
partitioned static-first/skinned-second (two `vkCmdDrawIndexedIndirect`,
firstInstance still the global object index). Reasons, all measured or
structural:

1. The static shader (`pbr_gpu_driven.vert`) is byte-frozen — its A/B parity
   proof stays valid without re-verification.
2. A per-vertex branch on uniform-per-draw data does not diverge, but it
   does force every static vertex through a skin fetch it ignores; the
   parallel stream makes the fetch cheap but not free.
3. The engine recorder already groups descriptor runs; the second draw is
   one indirect call, no extra barrier (same command buffer, same
   read-after-write ordering the single-draw path relies on).

## What was measured

- `GpuDrivenSkinnedMatchesCpuSkinnedPixels`: byte-identical hash across the
  CPU-skinned and driven-skinned paths (cube + two-segment beam, top half
  displaced +0.5x by joint 1). Red-signal sanity: 4778 red-dominant pixels
  on both paths.
- Sabotage, both directions: neutered palette (joint 1 → identity) breaks
  parity (full undisplaced beam, hash differs); rebound top-half joints to
  joint 0 breaks parity through the stream instead of the palette; restore
  reproduces the parity hash (proves the sabotages acted through the
  buffers claimed).
- A wrong packing caught live during development: per-vertex-interleaved
  [11+4+4] static+skin (shader reads planar [11N][8N]) rendered the bottom
  quad from v0's correct prefix and garbage afterwards — the parity test
  failed with the beam bottom present and the top clear, which is exactly
  the signature the test was built to catch. The CPU mannequin packer is
  planar (verified in examples/viewport/main.cpp), matching the shader.
- A sabotage-form trap worth keeping: writing integer `1U` into a float
  bone word is a denormal (~0), not 1.0f — a neutered palette built that
  way collapses the top half to a point instead of leaving it undisplaced,
  which "passes" EXPECT_NE for the wrong reason. The test writes `1.0f`
  through a float view, and the comment says why.
- 0 validation diagnostics on the RTX 2060; warning-free build of all
  touched TUs (clang + GCC flags as configured).
