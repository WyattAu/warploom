# Scale benchmark: GPU-driven path + per-config budgets (T0 gate)

## Setup

- Hardware: NVIDIA GeForce RTX 2060 (6 GB), driver 615.71.09
- Load at measure time: ~7-15 (shared machine) — every budget below carries
  ~1.5-2x headroom over the measured p95. A canary firing on a loaded
  machine means re-run at low load before blaming the change (proven once:
  a stray X11 `zoom_in` keypress moved the nohdr camera 0.025 units and the
  analyzer correctly failed that run; the quiet re-run is 32/32).
- Run shape: `WARPLOOM_MAX_FRAMES=150 WARPLOOM_TELEMETRY_DIR=/tmp/bud/<cfg>`
  (+ per-config flags below), 1280x720. Gate:
  `python3 scripts/analyze_telemetry.py /tmp/bud/<cfg> [--expect-skinned] [--expect-pt] --check-budgets <cfg>:docs/perf-budgets.json`
- Source: viewport telemetry (per-frame `total_us`, `record_us`, `gpu_ns`)

## Per-config budgets (150 frames, 2026-10-31; pt/scale 60 frames)

| Config | Flags | GPU p50/p95 (us) | Record p50/p95 (us) | Budget (gpu/rec) |
|---|---|---|---|---|
| default | mannequin scene | 655 / 702 | 66 / 158 | 1400 / 500 |
| bloom | `WARPLOOM_BLOOM=1` | 660 / 699 | 68 / 237 | 1400 / 500 |
| nohdr | `WARPLOOM_NO_HDR=1` | 566 / 608 | 71 / 213 | 1200 / 300 |
| gd | `GPU_DRIVEN=1 NO_MANNEQUIN=1` | 221 / 267 | 34 / 43 | 500 / 100 |
| rt | `WARPLOOM_RT_MODE=1` | 764 / 814 | 76 / 172 | 1200 / 500 |
| node | `WARPLOOM_NODE_EDITOR=1` | 675 / 694 | 67 / 117 | 1200 / 500 |
| pt | `WARPLOOM_PT=1` | 4875 / 7697 | 100 / 100 | 12000 / 500 |
| scale10k | gd + `INSTANCE_COUNT=10000` | 290 / 298 | 32 / 45 | 500 / 100 |
| phys4k | gd + `PHYSICS=1 COUNT=4002` | 498 / 649 | 32 / 40 | 1000 / 100 |
| phys8k | gd + `PHYSICS=1 COUNT=8002` | 552 / 656 | 33 / 46 | 1000 / 100 |

All ten analyzer runs green (budgets + correctness checks), 0 validation
diagnostics on every run, exit 0.

## Older 10k result (600 frames, 2026-09-25, pre-cap)

| Metric | Value |
|---|---|
| Frame total (median / p95) | **9.97 ms / 11.7 ms** (~100 FPS) |
| GPU device time (median / p95) | **70 us / 407 us** |
| CPU scene record (median / p95) | **64 us / 73 us** |

## Analysis

- The GPU-driven claim holds at 10k instances: the CPU cost of "managing"
  10,000 objects is ~32 us of scene recording and two indirect draws
  (static + the R3 skinned beam partition) — the CPU never enumerates,
  culls, or builds per-object commands. GPU 290 us median at 10k.
- The 10k run briefly became unmeasurable: the physics-instances commit
  capped the knob at `kGdMaxInstances - 1` = 4095 while the doc still
  claimed 10k. The cap is now 16384 (payload 1.6 MB, draw buffer 320 KB —
  address arithmetic, not memory pressure), and 10k is measured again.
- Physics scenes are wall-time-dominated by the CPU solver step, NOT by
  record or GPU: 4k bodies -> 32 ms/frame wall, 8k -> 70 ms (record stays
  ~33 us, GPU ~550 us). That step cost is D1's (island sleeping is the
  scheduled fix), recorded here so the improvement has a baseline.
- The ~10 ms frame total on non-physics configs vs sub-ms GPU+CPU work is
  present-path composition on the RTX 2060 — the pipeline itself has ~10x
  headroom before it appears on the frame budget.

## Reproduce

```bash
mkdir -p /tmp/bench_tel
WARPLOOM_GPU_DRIVEN=1 WARPLOOM_INSTANCE_COUNT=10000 WARPLOOM_MAX_FRAMES=600 \
WARPLOOM_TELEMETRY_DIR=/tmp/bench_tel \
build/default/bin/warploom_viewport
python3 scripts/analyze_telemetry.py /tmp/bench_tel --check-budgets scale10k:docs/perf-budgets.json
```

Analyze: filter `type == "frame"` rows in `<telemetry_dir>/telemetry.jsonl`
for `total_us` (wall), `record_us` (CPU record), `gpu_ns` (device).
The `--expect-skinned` / `--expect-pt` flags select the mannequin-scene
and path-tracing contracts (orbit advance + walk wraps vs camera held +
animation frozen); without them a correct skinned/PT run fails its own
gate, which is the gate working, not the run failing.
