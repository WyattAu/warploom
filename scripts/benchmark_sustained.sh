#!/usr/bin/env bash
# Sustained-run benchmark + analyzer gate for the composed viewport.
#
# Runs the windowed app offscreen-deterministically for N frames (long enough
# to pass the vsync warm-up and reach steady state), once per draw path:
#   A) per-draw composed path (baseline)
#   B) OMNICPP_GPU_DRIVEN=1 (cull/LOD on the GPU, one indirect draw)
# Then runs scripts/analyze_telemetry.py (including the GPU-timestamp gates)
# on both and prints a CPU/GPU timing comparison table.
#
# Usage: scripts/benchmark_sustained.sh [frames]
set -u
FRAMES="${1:-600}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/vulkan-validation/bin/omnicpp_viewport"
SPVDIR="$(find "$ROOT/build/vulkan-validation" -name pbr_full.frag.spv | head -1 | xargs dirname)"
if [ -z "$SPVDIR" ] || [ ! -x "$BIN" ]; then
  echo "benchmark: build the vulkan-validation preset first" >&2
  exit 1
fi

run_one() {
  local dir="$1" mode="$2"  # mode: "" (per-draw) or "1" (gpu-driven)
  rm -rf "$dir"; mkdir -p "$dir"
  local env_extra=()
  if [ -n "$mode" ]; then env_extra=(OMNICPP_GPU_DRIVEN="$mode"); fi
  env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
      OMNICPP_SHADER_DIR="$SPVDIR" \
      OMNICPP_MAX_FRAMES="$FRAMES" \
      OMNICPP_FIXED_DT=0.01666667 \
      OMNICPP_TELEMETRY_DIR="$dir" \
      OMNICPP_MODEL=/nonexistent/cubes_only \
      "${env_extra[@]}" \
      "$BIN" > "$dir/run.log" 2>&1
  local status=$?
  local vuids
  vuids=$(grep -c VUID "$dir/run.log" || true)
  if [ "$status" -ne 0 ] || [ "$vuids" -ne 0 ]; then
    echo "benchmark: run in $dir FAILED (exit $status, VUIDs $vuids)" >&2
    return 1
  fi
  python3 "$ROOT/scripts/analyze_telemetry.py" "$dir" >/dev/null || {
    echo "benchmark: analyzer gates FAILED for $dir" >&2
    return 1
  }
  return 0
}

run_one /tmp/vp_bench_perdraw "" || exit 1
run_one /tmp/vp_bench_gd "1" || exit 1

python3 - <<'PYEOF'
import json


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(len(s) * p))] if s else 0.0


def stats(path):
    rec, tot, gpu = [], [], []
    with open(path) as f:
        for line in f:
            try:
                j = json.loads(line)
            except json.JSONDecodeError:
                continue
            if j.get("type") == "frame":
                rec.append(j["record_us"])
                tot.append(j["total_us"])
                g = j.get("gpu_ns", 0.0)
                if g > 0.0:
                    gpu.append(g)
    return rec, tot, gpu


def fmt(v):
    return f"{v / 1000:.2f} ms"


rows = []
for name, path in (("per-draw", "/tmp/vp_bench_perdraw/telemetry.jsonl"),
                   ("gpu-driven", "/tmp/vp_bench_gd/telemetry.jsonl")):
    rec, tot, gpu = stats(path)
    rows.append((name, pct(rec, 0.5), pct(rec, 0.95),
                 pct(tot, 0.5), pct(tot, 0.95),
                 pct(gpu, 0.5), pct(gpu, 0.95), len(gpu)))

print(f"{'path':<12} {'rec p50':>10} {'rec p95':>10} {'total p50':>11} "
      f"{'total p95':>11} {'gpu p50':>10} {'gpu p95':>10} {'gpu frames':>11}")
for r in rows:
    print(f"{r[0]:<12} {r[1]:>8.0f}us {r[2]:>8.0f}us {fmt(r[3]):>11} "
          f"{fmt(r[4]):>11} {r[5] / 1000:>7.1f}us {r[6] / 1000:>7.1f}us "
          f"{r[7]:>11}")
PYEOF
echo "benchmark: both paths passed analyzer gates (0 VUIDs, $FRAMES frames each)"