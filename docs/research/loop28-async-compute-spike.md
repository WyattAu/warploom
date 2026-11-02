# Loop 28: async-compute overlap for the H-Z reduce (spike + adopt decision)

Status: measured, verdict ADOPT. The in-renderer migration is staged, not
landed (see §5); the spike (`AsyncComputeSpike.ReduceOverlapsGraphics`)
stays green in-suite as the measurement record.

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

1. CONCURRENT pyramid + H-Z depth-source images (mechanical; verifiable by
   golden parity + 0 diagnostics, no behaviour change).
2. Persisted per-frame depth for the compute leg (frame-parity discipline;
   the lifetime fix §5 requires).
3. Compute-queue reduce + pack behind the existing timeline handoff,
   golden re-proof + sabotage, full suite under validation.
