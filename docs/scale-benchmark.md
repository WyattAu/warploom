# Scale benchmark: GPU-driven path (P-track gate)

## Setup

- Hardware: NVIDIA GeForce RTX 2060 (6 GB), driver 615.71.09
- Run: `OMNICPP_GPU_DRIVEN=1 OMNICPP_INSTANCE_COUNT=10000 OMNICPP_MAX_FRAMES=600`
  (compute cull/LOD -> ONE indirect draw; 1280x720)
- Source: viewport telemetry (per-frame `total_us`, `record_us`, `gpu_ns`)

## Results (600 frames, 2026-09-25)

| Metric | Value |
|---|---|
| Frame total (median / p95) | **9.97 ms / 11.7 ms** (~100 FPS) |
| GPU device time (median / p95) | **70 us / 407 us** |
| CPU scene record (median / p95) | **64 us / 73 us** |

## Analysis

- The GPU-driven claim holds at 10k instances: the CPU cost of "managing"
  10,000 objects is ~64 us of scene recording and a single indirect draw —
  the CPU never enumerates, culls, or builds per-object commands.
- GPU device time (70 us median) is dominated by the vertex pull of ~1.2M
  triangles; the cull/LOD compute pass is noise at this scale.
- The ~9.9 ms frame total vs ~0.5 ms GPU+CPU work gap is present-path /
  vsync composition on the RTX 2060 at 1280x720 — the simulation + render
  pipeline itself has ~20x headroom before it would appear on the frame
  budget. Scaling past 10k instances is a payload-size question, not a
  per-object CPU cost question.

## Reproduce

```bash
mkdir -p /tmp/bench_tel
OMNICPP_GPU_DRIVEN=1 OMNICPP_INSTANCE_COUNT=10000 OMNICPP_MAX_FRAMES=600 \
OMNICPP_SHADER_DIR=build/vulkan-validation/tests/shaders \
OMNICPP_TELEMETRY_DIR=/tmp/bench_tel \
build/vulkan-validation/bin/omnicpp_viewport
```

Analyze: filter `type == "frame"` rows in `<telemetry_dir>/telemetry.jsonl`
for `total_us` (wall), `record_us` (CPU record), `gpu_ns` (device).
