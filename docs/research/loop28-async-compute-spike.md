# Loop 28: async-compute overlap for the H-Z reduce (spike + adopt decision)

Status: measured, verdict ADOPT. Migration staged (see §5): steps 1-3
landed 2026-11-02/03; step 3 is proved synchronously (bit-identical
compute result), pipelined overlap is the remaining stage. The spike
(`AsyncComputeSpike.ReduceOverlapsGraphics`) stays green in-suite as the
measurement record.

## Source

Vendor documentation (labelled as such, not as evidence):

- AMD GPUOpen, "RDNA Performance Guide" (gpuopen.com/learn/rdna-performance-guide,
  2023): async compute queues issue compute commands parallel to the
  graphics queue, using otherwise-idle shader resources.
- AMD GPUOpen, "Leveraging Asynchronous Queues for Concurrent Execution"
  (gpuopen.com/learn/concurrent-execution-asynchronous-queues, 2016):
  queue traces showing the graphics queue not stalled while the async
  compute queue runs.
- NVIDIA Turing-era guidance (widely reported, vendor talks): concurrent
  graphics + compute execution on independent queues when no dependency
  exists between the two legs.

No paper is borrowed here; this loop is plumbing + measurement. The
technique (overlap independent graphics/compute on separate queues) is
settled vendor-documented hardware behaviour, not a claim under test. What
was unknown -- and is now measured -- is whether THIS engine's H-Z-style
reduce overlaps THIS frame's graphics on the available hardware
(RTX 2060: COMPUTE-only family 2, 8 queues).

## The claim being checked

Submitting the production `depth_reduce` buffer kernel (per-tile maxima
over a depth-word buffer -- the real H-Z workload, not a synthetic kernel)
on the dedicated compute queue while the graphics queue renders
concurrently beats doing both sequentially on the graphics queue, with
determinism intact (bit-identical pyramid whichever queue reduces).

## Novelty assessment

None. Overlapping independent queue work is standard practice on every
modern GPU; the in-tree `AsyncComputeQueue` (timeline-semaphore handoff,
`VulkanHardware.AsyncComputeTimelineHandoff`) already proves the mechanism.
What is specific to this codebase is the measurement (below) and the
consequence for the H-Z path: its 1-frame-latency design (previous pyramid
consumed by the next frame's cull) is exactly the shape that pipelines
across queues without mid-frame waits.

## Why it fits here, and what it costs

- Fit: the renderer records the image-based H-Z reduce inline in the
  graphics command buffer today. The steady-state overlap is free in
  principle: frame N+1's graphics render needs only frame N-1's cull data
  (already the latency contract), so no submit ever waits on the other
  queue's current frame.
- Cost (why the migration is staged, §5): the live path samples the depth
  IMAGE and writes pyramid IMAGES, all `VK_SHARING_MODE_EXCLUSIVE`. Cross-
  queue use needs (a) CONCURRENT pyramid + depth-source images (buffers
  already are -- the allocator marks them concurrent when a dedicated
  family exists), (b) per-frame persistence of the depth the compute leg
  reads (today's depth attachment is overwritten by the next frame's
  render; a same-frame wait would serialize the two queues and erase the
  win -- the analysis, not just the intuition, is in §5), and (c) a CPU-
  side frame-parity discipline for which depth/pyramid generation the
  compute leg consumes. None of that is in doubt technically; all of it
  touches the frame loop, so it lands as its own stage with golden
  re-proof, not as a drive-by.

## What was measured (this codebase, RTX 2060, under validation)

`AsyncComputeSpike.ReduceOverlapsGraphics`, 1280x768 frame, tile 32
(40x24 tiles), 200 fullscreen-triangle draws vs the production reduce
chain, 5 warmup + 30 timed iters, wall clock submit->fences:

| run | A sequential (gfx) | B overlapped (gfx+cmp) | delta |
|-----|-------------------|------------------------|-------|
| 1   | 8.087 ms          | 4.029 ms               | -50.2% |
| 2   | 12.933 ms         | 8.273 ms               | -36.0% |
| 3   | 8.353 ms          | 5.016 ms               | -39.9% |
| 4   | 8.766 ms          | 4.073 ms               | -53.5% |

Absolute times move with machine load (run 2 was taken at high load);
the delta is stable at roughly -3.3 to -4.7 ms: the ~4 ms reduce hides
almost entirely behind the render. B ≈ max(render, reduce), A ≈ sum --
the signature of genuine concurrent execution, not a scheduling artifact.

Correctness, same binary: pyramid bit-identical across queues AND every
tile word equals floatBitsToUint(0.5) (constant-depth input makes the
kernel output fully determined -- a mis-wired dispatch cannot hide behind
A==B). 0 validation errors/warnings on all runs: no queue-ownership
violation on the shared buffers.

## Adopt staging (the part that is NOT this loop)

1. CONCURRENT pyramid + H-Z depth-source images -- DONE 2026-11-02
   (RTX 2060, commit with this doc). `ImageSharing` (one validity rule:
   concurrent with two distinct families) flows from
   `VulkanContext::cross_queue_sharing()` into the pyramid, the
   transient-store HDR depth (surgical: HDR color and bloom stay
   EXCLUSIVE), and the swapchain depth (renderer + viewport both build
   from the same helper). Verifiable by golden parity + 0 diagnostics,
   no behaviour change: `ConcurrentSharingMatchesExclusive` reduces a
   0.5-cleared depth into both pyramids on the graphics queue --
   identical words, absolute check both legs (sabotaged live: a wrong
   constant fails both legs), 0 diagnostics -- and the compose+H-Z
   (`ComposeDepthReductionIsValidationClean`) and golden-hash tests are
   green with sharing auto-enabled under the same binary.
2. Persisted per-frame depth for the compute leg (frame-parity discipline;
   the lifetime fix §5 requires) -- DONE 2026-11-03 (this session).
   Each graphics-queue H-Z frame copies its live depth into a same-extent/
   same-format snapshot indexed by the ping-pong token, parked in
   SHADER_READ_ONLY_OPTIMAL next to the destination pyramid; the reduction
   still samples live (no behaviour change). Proved by the new
   `HiZGraph.DepthSnapshotsMatchLiveDepth`: after two ping-pong frames both
   snapshots exist, the valid flag + last-slot introspection agree, and the
   snapshot words are bit-identical to the live depth (image->buffer
   readback of both, DEPTH aspect). 0 diagnostics/warnings; the sabotage
   direction is structural (skip the copy -> null/invalid -> this fails
   while ComposeDepthReductionIsValidationClean still passes).
3. Compute-queue reduce + pack -- DONE 2026-11-03 (this session,
   synchronous proof shape). `RendererConfig::hiz_compute_reduce` arms a
   renderer-owned `AsyncComputeQueue` (dedicated COMPUTE-only family;
   silent graphics fallback otherwise). A snapshot-reading twin of the
   reduction sets (binding 0 + binding 2 at level 0 re-pointed at the
   snapshot, zero steady-state updates) feeds the same graph plan with one
   patch: the level-0 depth edge's initial layout is the snapshot's
   resting SHADER_READ_ONLY_OPTIMAL, not the live attachment's
   DEPTH_STENCIL_ATTACHMENT_OPTIMAL (unpatched, the executor emits a
   VUID-01209 barrier against a SAMPLED/TRANSFER image -- caught live, not
   reasoned about). `compute_reduce_hiz_snapshot(slot)` records reduce +
   pack on the compute queue, CPU-waits, and marks the pyramid/pack fresh.
   Proved by `HiZGraph.ComputeReduceMatchesGraphicsReduce`: after two
   graphics frames the packed words are read back, the last slot is
   re-reduced on compute, and the words match bit-exactly, 0 diagnostics/
   warnings (CONCURRENT images need no ownership transfer). Remaining for
   the full migration: pipeline the compute leg a frame behind graphics
   with the timeline handoff instead of CPU-waiting (overlap, not just
   correctness) -- the spike already measured the headroom at -36..-54%.
