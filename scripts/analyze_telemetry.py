#!/usr/bin/env python3
"""Validate a viewport telemetry run: determinism, scene correctness, capture sanity.

Reads <dir>/telemetry.jsonl (manifest + frame lines written by the viewport)
and asserts the properties a correct run must have. Exit 0 = all checks pass;
nonzero = the run is broken, with every violation printed.

Usage: python3 scripts/analyze_telemetry.py <telemetry_dir> [--expect-skinned]
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

CHECKS_PASSED = 0
CHECKS_FAILED = 0


def check(condition, label, detail=""):
    global CHECKS_PASSED, CHECKS_FAILED
    if condition:
        CHECKS_PASSED += 1
        print(f"  PASS  {label}")
    else:
        CHECKS_FAILED += 1
        print(f"  FAIL  {label}" + (f"  [{detail}]" if detail else ""))


def load_run(telemetry_dir):
    telemetry_dir = Path(telemetry_dir)
    log_path = telemetry_dir / "telemetry.jsonl"
    manifest, frames, events = None, [], []
    scene_objects, skeleton, clips = None, None, None
    poses, inputs = [], []
    with open(log_path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            kind = record["type"]
            if kind == "manifest":
                manifest = record
            elif kind == "frame":
                frames.append(record)
            elif kind == "event":
                events.append(record)
            elif kind == "scene_objects":
                scene_objects = record
            elif kind == "scene_skeleton":
                skeleton = record
            elif kind == "scene_clips":
                clips = record
            elif kind == "pose":
                poses.append(record)
            elif kind == "input":
                inputs.append(record)
    return (manifest, frames, events, telemetry_dir, scene_objects,
            skeleton, clips, poses, inputs)


def read_ppm(path):
    data = path.read_bytes()
    # P6 header: "P6\n<w> <h>\n<max>\n"
    parts = data.split(b"\n", 3)
    width, height = (int(x) for x in parts[1].split())
    pixels = parts[3]
    return width, height, pixels


def read_depth32(path, expected_count):
    raw = path.read_bytes()
    return list(struct.unpack(f"<{expected_count}f", raw[: expected_count * 4]))


def analyze(manifest, frames, events, telemetry_dir, expect_skinned,
            walk_duration=1.0, allow_zoom=False):
    print("== Manifest ==")
    check(manifest is not None, "manifest line present")
    if manifest is None:
        return
    check(manifest["width"] == 1280 and manifest["height"] == 720,
          "viewport resolution 1280x720",
          f"{manifest['width']}x{manifest['height']}")
    check(manifest["fixed_dt"] > 0.0, "fixed dt positive")

    print("== Frame progression ==")
    indices = [f["frame"] for f in frames]
    check(indices == sorted(indices) and len(set(indices)) == len(indices),
          "frame indices strictly increasing and unique")
    check(indices == list(range(1, len(frames) + 1)),
          "frame indices are 1..N without gaps")
    sim_times = [f["t"] for f in frames]
    dt = manifest["fixed_dt"]
    drift = max(abs(sim_times[i] - (i * dt)) for i in range(len(sim_times)))
    check(drift < 1e-3, "sim time tracks fixed dt exactly",
          f"max drift {drift:.2e}s")

    print("== Scene composition ==")
    if expect_skinned:
        check(all(f["skinned"] for f in frames),
              "every frame uses the skinned pipeline (skinned scene)")
        check(all(f["objects"] >= 2 for f in frames),
              "skinned scene draws >= 2 objects (figure + ground)",
              f"min objects {min(f['objects'] for f in frames)}")
    else:
        check(not any(f["skinned"] for f in frames),
              "cubes-only scene never touches the skinned pipeline")
        check(all(f["objects"] == 3 for f in frames),
              "cubes scene has exactly 3 objects (ground + 2 cubes)")

    print("== Camera orbit ==")
    eyes = [f["eye"] for f in frames]
    radii = [math.hypot(e[0], e[2]) for e in eyes]
    radius_span = max(radii) - min(radii)
    if allow_zoom:
        # Scripted zoom responses intentionally move the camera radius; only
        # require that it stays a plausible orbit distance.
        check(min(radii) > 0.5, "orbit radius stays plausible under zoom",
              f"min {min(radii):.3f}")
    else:
        check(radius_span < 1e-2, "orbit radius constant over the run",
              f"span {radius_span:.4f}")
    angles = [math.atan2(e[2], e[0]) for e in eyes]
    unwrapped = []
    for a in angles:
        if unwrapped:
            while a - unwrapped[-1] > math.pi:
                a -= 2 * math.pi
            while a - unwrapped[-1] < -math.pi:
                a += 2 * math.pi
        unwrapped.append(a)
    total_sweep = unwrapped[-1] - unwrapped[0]
    check(total_sweep > 0.0, "camera orbit advances monotonically",
          f"sweep {total_sweep:.3f} rad")
    # omega = 0.25 rad/s in the viewport scene.
    expected_sweep = 0.25 * sim_times[-1]
    check(abs(total_sweep - expected_sweep) < 0.05 * max(expected_sweep, 1e-6),
          "orbit angular rate matches 0.25 rad/s",
          f"sweep {total_sweep:.3f} vs expected {expected_sweep:.3f}")

    print("== Animation ==")
    walk_times = [f["walk_t"] for f in frames]
    blends = [f.get("blend", 0.0) for f in frames]
    if manifest["has_mannequin"]:
        if any(abs(b) > 1e-6 for b in blends):
            # Cross-fade run: weight must reach both extremes (full walk and
            # full idle) and stay within [0, 1].
            check(all(-1e-4 <= b <= 1.0 + 1e-4 for b in blends),
                  "cross-fade weight stays in [0, 1]",
                  f"min {min(blends):.4f} max {max(blends):.4f}")
            check(any(abs(b - 1.0) < 0.01 for b in blends),
                  "cross-fade reaches full idle")
            check(any(abs(b) < 0.01 for b in blends),
                  "cross-fade returns to full walk")
        duration = walk_duration  # walk-cycle length of the loaded model
        wraps = 0
        for prev, curr in zip(walk_times, walk_times[1:]):
            if curr < prev:
                wraps += 1
            step = (curr - prev) % duration if curr < prev else curr - prev
            check_abs = abs(step - dt) < 1e-3
            if not check_abs:
                check(False, "walk phase advances by dt each frame",
                      f"step {step:.4f} at wrap")
                break
        else:
            check(True, "walk phase advances by dt each frame (wraps included)")
        check(wraps >= 1, "walk phase wrapped at least once (cycle repeats)",
              f"{wraps} wraps in {len(frames)} frames")
    else:
        check(all(w == 0.0 for w in walk_times), "walk phase unused")

    print("== Timing sanity ==")
    totals = [f["total_us"] for f in frames]
    check(all(t > 0.0 for t in totals), "frame times positive")
    med = sorted(totals)[len(totals) // 2]
    check(med < 100_000.0, "median frame under 100 ms (vsync or faster)",
          f"median {med / 1000:.1f} ms")

    print("== GPU captures ==")
    captures = [f for f in frames if f["capture"]]
    if captures:
        max_frames_captured = min(8, len(frames))
        check(len(captures) <= max_frames_captured,
              "capture count within limit")
        for frame_record in captures:
            ppm = telemetry_dir / frame_record["capture"]
            check(ppm.exists(), f"{frame_record['capture']} exists on disk")
            if not ppm.exists():
                continue
            width, height, pixels = read_ppm(ppm)
            check((width, height) == (manifest["width"], manifest["height"]),
                  f"{frame_record['capture']} resolution matches manifest",
                  f"{width}x{height}")
            n = width * height
            # Color content: a rendered scene is never a flat clear color.
            unique = len(set(pixels[i:i + 3] for i in range(0, len(pixels), 3)
                             )) if n <= 4096 else None
            if unique is not None:
                check(unique > 16, f"{frame_record['capture']} has varied pixels",
                      f"{unique} unique colors")
            hist_r = sum(1 for i in range(0, len(pixels), 3)
                         if pixels[i] > 60)
            check(hist_r > n // 100,
                  f"{frame_record['capture']} has >1% lit (non-clear) pixels",
                  f"{hist_r}/{n}")
            depth_path = ppm.with_suffix(".depth32")
            if depth_path.exists():
                depth = read_depth32(depth_path, n)
                # Clear depth = 1.0 (background). A scene render must have
                # geometry, i.e. some pixels strictly closer than 1.0.
                closer = sum(1 for d in depth if d < 0.999)
                check(closer > n // 100,
                      f"{frame_record['capture']} depth has >1% geometry",
                      f"{closer}/{n}")
                check(any(d > 0.0 for d in depth), "depth values positive")
            else:
                check(False, f"{depth_path.name} missing")
    else:
        print("  (no captures configured)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("telemetry_dir")
    parser.add_argument("--expect-skinned", action="store_true",
                        help="assert the mannequin scene ran")
    parser.add_argument("--walk-duration", type=float, default=1.0,
                        help="walk-cycle duration of the loaded model")
    parser.add_argument("--allow-zoom", action="store_true",
                        help="scripted zoom changes camera radius by design")
    args = parser.parse_args()

    (manifest, frames, events, telemetry_dir, scene_objects, skeleton,
     clips, poses, inputs) = load_run(args.telemetry_dir)
    if not frames:
        print("no frame records found")
        return 2
    analyze(manifest, frames, events, telemetry_dir, args.expect_skinned,
            args.walk_duration, args.allow_zoom)

    # Scene-structure assertions (Phase B1 data).
    print("== Scene structure ==")
    check(scene_objects is not None, "scene_objects manifest present")
    check(bool(scene_objects and scene_objects["objects"]),
          "scene lists at least one object")
    if manifest and manifest["has_mannequin"]:
        check(skeleton is not None, "skeleton manifest present")
        check(bool(skeleton and skeleton["joints"]), "skeleton names joints")
        check(bool(clips and clips["clips"]), "animation clips listed")
        check(poses is not None and len(poses) == len(frames),
              "one pose line per frame",
              f"{len(poses)} poses vs {len(frames)} frames")
        if poses:
            # Walk-phase consistency between frame and pose records.
            mismatched = sum(
                1 for f, p in zip(frames, poses)
                if abs(f["walk_t"] - p["walk_phase"]) > 1e-4)
            check(mismatched == 0, "pose walk_phase matches frame walk_t",
                  f"{mismatched} mismatches")
            # The largest swing joint is named once the pose moves (the
            # bind pose has zero deviation, so early frames may be empty).
            check(any(p["swing_joint"] for p in poses),
                  "pose lines name a swing joint after warm-up")
            check(sum(1 for p in poses if p["swing_joint"]) >=
                  len(poses) // 2,
                  "a majority of pose lines name a swing joint")
            degs = [p["swing_deg"] for p in poses]
            check(max(degs) > 1.0, "swing angle moves during the run",
                  f"max {max(degs):.1f} deg")
            # Allocator stats are stable and nonzero.
            check(all(p["allocator_reserved"] > 0 for p in poses),
                  "allocator telemetry populated")

    print(f"\n{CHECKS_PASSED} passed, {CHECKS_FAILED} failed")
    return 1 if CHECKS_FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
