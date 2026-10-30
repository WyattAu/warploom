# Performance budgets: per-config frame budgets + scale baselines (T0)

Status: baselines measured on the RTX 2060, 1280x720, 120 frames each
(`WARPLOOM_MAX_FRAMES=120`, `WARPLOOM_FIXED_DT=0.0166667`), Xvfb
(`DISPLAY=:99`), 2026-10-30. Machine load during measurement was
~11–12 (shared, loady) — budgets are set at **~2x measured p95** so a
clean machine has headroom and a regression still trips the gate. A
budget trip on a loaded machine is a canary, not a conviction: re-run
at low load before blaming the change (same rule as
`JobSystem.DispatchOverheadIsSubMicrosecond`).

## Baselines (measured)

| Config | Wall total med / p95 | GPU device med / p95 | CPU record med / p95 | gpu coverage |
|---|---|---|---|---|
| default (mannequin, HDR compose) | 20.48 / 24.67 ms | 641 / 684 us | 65 / 235 us | 118/120 |
| bloom (`WARPLOOM_BLOOM=1`) | 19.48 / 22.41 ms | 644 / 673 us | 65 / 230 us | 118/120 |
| no-HDR (`WARPLOOM_NO_HDR=1`) | 18.27 / 19.34 ms | 562 / 582 us | 62 / 71 us | 118/120 |
| gd cubes (`WARPLOOM_GPU_DRIVEN=1 WARPLOOM_NO_MANNEQUIN=1`) | 18.34 / 20.53 ms | 208 / 241 us | 23 / 28 us | 118/120 |
| scale 10k (`+ WARPLOOM_INSTANCE_COUNT=10000`, 120 frames) | 19.03 / 22.53 ms | 209 / 221 us | 25 / 33 us | 118/120 |

Notes worth keeping:

- Wall total is present/vsync-dominated (~18–20 ms under Xvfb here vs
  9.97 ms in `docs/scale-benchmark.md` on 2026-09-25) — the engine
  budgets below gate on **GPU device time and CPU record time**, not on
  wall total. Wall total is reported for context only.
- Bloom barely exceeds default here (644 vs 641 us med). The O2
  cross-check (bloom compose 0.22 vs 0.09 ms) remains the evidence that
  bloom costs a real second fullscreen pass set; on this scene the
  absolute delta hides inside vsync noise.
- gd record cost (~25 us at 10k instances) confirms the scale claim:
  per-object CPU cost does not grow — 10k instances record in ~the same
  time as the 3-object gd scene.
- `record_us` p95 spikes in default/bloom (230+ us vs 65 med) are
  load noise on a machine at load ~12, which is exactly why budgets sit
  at 2x p95 rather than at p95.

## Budgets (gates)

Machine-readable form: `docs/perf-budgets.json` (same numbers). Gate:

```bash
python3 scripts/analyze_telemetry.py <telemetry_dir> --check-budgets docs/perf-budgets.json
```

| Config key | GPU p95 budget | Record p95 budget | Min gpu coverage |
|---|---|---|---|
| default | 1400 us | 500 us | 90% |
| bloom | 1400 us | 500 us | 90% |
| nohdr | 1200 us | 200 us | 90% |
| gd | 500 us | 100 us | 90% |
| scale10k | 500 us | 100 us | 90% |

## Reproduce

```bash
rm -rf /tmp/bud_default && mkdir -p /tmp/bud_default
DISPLAY=:99 WARPLOOM_TELEMETRY_DIR=/tmp/bud_default WARPLOOM_MAX_FRAMES=120 \
  WARPLOOM_FIXED_DT=0.0166667 ./build/default/bin/warploom_viewport
python3 scripts/analyze_telemetry.py /tmp/bud_default
python3 scripts/analyze_telemetry.py /tmp/bud_default --check-budgets docs/perf-budgets.json
```

Variants: add `WARPLOOM_BLOOM=1` (bloom), `WARPLOOM_NO_HDR=1` (nohdr),
`WARPLOOM_GPU_DRIVEN=1 WARPLOOM_NO_MANNEQUIN=1` (gd),
`+ WARPLOOM_INSTANCE_COUNT=10000` (scale10k, also needs
`WARPLOOM_NO_MANNEQUIN=1` — with the mannequin the 10k run never
reaches its frame cap in 280 s).
