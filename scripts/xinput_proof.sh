#!/bin/bash
# Live X-input proof: launch the viewport, wait for telemetry, drive real
# keystrokes through XTEST (xdotool), verify the camera responded from the
# app's own telemetry, then quit by PID. No screen capture involved — the
# app reports its own input-consumption truth.
set -u
DIR=/tmp/vp_xinput3
rm -rf "$DIR"; mkdir -p "$DIR"
SPVDIR=$(find "$(pwd)/build/vulkan-validation" -name '*.spv' | head -1 | xargs dirname)

VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
OMNICPP_SHADER_DIR="$SPVDIR" \
OMNICPP_MAX_FRAMES=4000 \
OMNICPP_FIXED_DT=0.01666667 \
OMNICPP_TELEMETRY_DIR="$DIR" \
  ./build/vulkan-validation/bin/omnicpp_viewport > "$DIR/run.log" 2>&1 &
VP_PID=$!
echo "$VP_PID" > "$DIR/pid"

# Wait for telemetry to appear (up to 10 s).
for i in $(seq 1 50); do
  [ -s "$DIR/telemetry.jsonl" ] && break
  sleep 0.2
done
if [ ! -s "$DIR/telemetry.jsonl" ]; then
  echo "FAIL: telemetry never appeared"
  kill "$VP_PID" 2>/dev/null
  exit 1
fi

# Locate and focus the window (poll, no blocking sync).
WID=""
for i in $(seq 1 25); do
  WID=$(xdotool search --name 'OmniCpp Viewport' 2>/dev/null | head -1)
  [ -n "$WID" ] && break
  sleep 0.2
done
if [ -z "$WID" ]; then
  echo "FAIL: window not found"
  kill "$VP_PID" 2>/dev/null
  exit 1
fi
echo "window: $WID"
xdotool windowfocus "$WID" 2>/dev/null || true

# Hold W for ~0.8 s (zoom in), release, let a few frames pass.
xdotool keydown w
sleep 0.8
xdotool keyup w
sleep 0.4

# Read the camera response from telemetry.
python3 - "$DIR/telemetry.jsonl" <<'PYEOF'
import json, math, sys

frames = []
for line in open(sys.argv[1]):
    line = line.strip()
    if not line:
        continue
    d = json.loads(line)
    if d.get("type") == "frame":
        frames.append(d)

radii = [math.hypot(f["eye"][0], f["eye"][2]) for f in frames]
base = radii[0]
dev = next((i for i, r in enumerate(radii) if abs(r - base) > 0.02), None)
print(f"frames: {len(frames)}; base radius {base:.3f}")
if dev is None:
    print("XINPUT PROOF: FAIL - camera never moved")
    sys.exit(1)
print(f"first deviation: frame {dev + 1}")
lo, hi = max(0, dev - 2), min(len(frames), dev + 70)
print("trajectory:", [(frames[i]["frame"], round(radii[i], 3))
                     for i in range(lo, hi, 10)])
delta = base - min(radii)
print(f"max radius delta: {delta:.3f}")
print("XINPUT PROOF: PASS" if delta > 0.3 else "XINPUT PROOF: FAIL - delta too small")
sys.exit(0 if delta > 0.3 else 1)
PYEOF
STATUS=$?

kill "$VP_PID" 2>/dev/null
wait "$VP_PID" 2>/dev/null
exit $STATUS
