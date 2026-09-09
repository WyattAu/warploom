#!/usr/bin/env python3
"""Closed-loop scenario runner for the viewport observer.

A scenario JSON fully describes an automated viewport test: scene model,
run configuration, a scripted input sequence, and the closed-loop responses
the engine MUST produce. The runner executes the viewport under validation
layers, runs the telemetry analyzer, then asserts each scripted response
from the run's own telemetry -- proving that input actually reached and
moved the scene.

Scenario schema (all keys optional except name):
{
  "name": "zoom_and_fade",
  "model": "mannequin",           // mannequin | cesiumman | cubes
  "frames": 420,                  // OMNICPP_MAX_FRAMES
  "fixed_dt": 0.01666667,
  "capture_every": 105,           // 0 disables captures
  "capture_limit": 8,
  "input_script": [ {"tick": 30, "action": "zoom_in", "value": 1.0}, ... ],
  "expect": {
    "skinned": true,              // --expect-skinned to the analyzer
    "walk_duration": 1.0,
    "responses": [
      {"kind": "input_consumed", "tick": 30, "action": "zoom_in", "value": 1.0},
      {"kind": "radius_decreases_during", "action": "zoom_in",
       "from_tick": 30, "to_tick": 90, "min_delta": 0.5},
      {"kind": "blend_reaches", "after_tick": 120, "within_ticks": 40,
       "target": 1.0, "tolerance": 0.01}
    ]
  },
  "determinism": true             // run twice; telemetry must be identical
}

Usage: python3 scripts/run_scenario.py <scenario.json> [workdir]
Exit 0 = every check passed.
"""

import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VIEWPORT = REPO / "build" / "vulkan-validation" / "bin" / "omnicpp_viewport"
ANALYZER = REPO / "scripts" / "analyze_telemetry.py"
MODELS = {
    # OMNICPP_MODEL is a path WITHOUT the .gltf extension (the viewport
    # appends it); buffer URIs resolve relative to the document's directory.
    "mannequin": (REPO / "assets" / "models" / "mannequin", 1.0),
    "cesiumman": (REPO / "assets" / "models" / "cesiumman" / "CesiumMan",
                  2.0),
}

PASSED = 0
FAILED = 0


def check(condition, label, detail=""):
    global PASSED, FAILED
    if condition:
        PASSED += 1
        print(f"  PASS  {label}")
    else:
        FAILED += 1
        print(f"  FAIL  {label}" + (f"  [{detail}]" if detail else ""))


def load_telemetry(path):
    frames, inputs = [], []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        record = json.loads(line)
        if record.get("type") == "frame":
            frames.append(record)
        elif record.get("type") == "input":
            inputs.append(record)
    return frames, inputs


def strip_volatile(path):
    out = []
    run_root = str(Path(path).parent)
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        record = json.loads(line)
        if record.get("type") == "frame":
            for key in ("record_us", "total_us", "fps"):
                record.pop(key, None)
        # Normalize run-specific paths (e.g. the per-run input script).
        text = json.dumps(record, sort_keys=True).replace(run_root, "<RUN>")
        out.append(text)
    return out


def find_shader_dir():
    """Locate the directory holding the compiled pbr_scene.*.spv shaders."""
    hits = sorted((REPO / "build").rglob("pbr_scene.vert.spv"))
    return str(hits[0].parent) if hits else None


def run_viewport(telemetry_dir, scenario):
    env = dict(os.environ)
    env["OMNICPP_MAX_FRAMES"] = str(scenario.get("frames", 300))
    env["OMNICPP_FIXED_DT"] = str(scenario.get("fixed_dt", 0.01666667))
    env["OMNICPP_TELEMETRY_DIR"] = str(telemetry_dir)
    env["OMNICPP_CAPTURE_EVERY"] = str(scenario.get("capture_every", 0))
    env["OMNICPP_CAPTURE_LIMIT"] = str(scenario.get("capture_limit", 8))
    if "OMNICPP_SHADER_DIR" not in env:
        shader_dir = find_shader_dir()
        if shader_dir:
            env["OMNICPP_SHADER_DIR"] = shader_dir
    model = scenario.get("model", "mannequin")
    if model in MODELS:
        env["OMNICPP_MODEL"] = str(MODELS[model][0])
    elif model != "cubes":
        raise SystemExit(f"unknown model {model}")
    if scenario.get("input_script"):
        script_path = telemetry_dir / "input_script.jsonl"
        with open(script_path, "w", encoding="utf-8") as handle:
            for event in scenario["input_script"]:
                handle.write(json.dumps(event) + "\n")
        env["OMNICPP_INPUT_SCRIPT"] = str(script_path)
    log_path = telemetry_dir / "run.log"
    with open(log_path, "w", encoding="utf-8") as log:
        proc = subprocess.run([str(VIEWPORT)], env=env, stdout=log,
                              stderr=subprocess.STDOUT, timeout=300)
    return proc.returncode, log_path


def check_responses(scenario, frames, inputs):
    responses = scenario.get("expect", {}).get("responses", [])
    if not responses:
        return
    print("== Closed-loop responses ==")
    for i, resp in enumerate(responses):
        kind = resp.get("kind")
        if kind == "input_consumed":
            # The consumed-event record logs the event's own value (the
            # fade_toggle press carries 1.0 even though the outcome flips).
            match = any(rec.get("tick") == resp["tick"] and
                        rec.get("action") == resp["action"] and
                        rec.get("value") == resp.get("value", 1.0)
                        for rec in inputs)
            check(match, f"response {i}: {resp['action']}@{resp['tick']} "
                         f"reached the engine",
                  f"{len(inputs)} input records")
        elif kind == "radius_decreases_during":
            lo, hi = resp["from_tick"], resp["to_tick"]

            def radius(tick):
                f = frames[tick]
                return math.hypot(f["eye"][0], f["eye"][2])

            before = radius(max(lo - 5, 0))
            during = min(radius(t) for t in range(lo, min(hi, len(frames))))
            delta = before - during
            check(delta >= resp.get("min_delta", 0.3),
                  f"response {i}: radius shrinks while {resp['action']} held",
                  f"delta {delta:.3f} (before {before:.3f}, min {during:.3f})")
        elif kind == "blend_reaches":
            start = resp["after_tick"]
            window = resp.get("within_ticks", 40)
            target = resp["target"]
            tol = resp.get("tolerance", 0.01)
            reached = any(
                abs(f.get("blend", 0.0) - target) <= tol
                for f in frames[start:start + window])
            check(reached,
                  f"response {i}: blend reaches {target} within {window} "
                  f"ticks of tick {start}",
                  f"window blends "
                  f"{[round(f.get('blend', 0.0), 2) for f in frames[start:start+window:10]]}")
        else:
            check(False, f"response {i}: unknown kind {kind}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    scenario_path = Path(sys.argv[1])
    scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
    name = scenario.get("name", scenario_path.stem)
    print(f"=== scenario: {name} (model={scenario.get('model', 'mannequin')})"
          f" ===")

    if not VIEWPORT.exists():
        print(f"viewport binary missing: {VIEWPORT}")
        return 2

    workdir = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    base = workdir or Path(tempfile.mkdtemp(prefix=f"scenario_{name}_"))

    results = []
    runs = 2 if scenario.get("determinism") else 1
    for run_index in range(runs):
        run_dir = base / f"run{run_index + 1}"
        run_dir.mkdir(parents=True, exist_ok=True)
        print(f"-- run {run_index + 1}/{runs} -> {run_dir}")
        code, log_path = run_viewport(run_dir, scenario)
        check(code == 0, f"run {run_index + 1}: viewport exited 0",
              f"exit {code}, see {log_path}")
        vuids = sum(1 for line in log_path.read_text(
            encoding="utf-8", errors="replace").splitlines()
            if "VUID" in line)
        check(vuids == 0, f"run {run_index + 1}: zero validation diagnostics",
              f"{vuids} VUIDs")
        analyzer_args = [sys.executable, str(ANALYZER), str(run_dir)]
        if scenario.get("expect", {}).get("skinned", True):
            analyzer_args.append("--expect-skinned")
        responses = scenario.get("expect", {}).get("responses", [])
        if any(r.get("kind") == "radius_decreases_during"
               for r in responses):
            analyzer_args.append("--allow-zoom")
        analyzer = subprocess.run(analyzer_args,
                                  capture_output=True, text=True, timeout=120)
        tail = analyzer.stdout.strip().splitlines()
        summary = tail[-1] if tail else "analyzer produced no output"
        check(analyzer.returncode == 0,
              f"run {run_index + 1}: telemetry analyzer passed", summary)
        results.append(run_dir)

    frames, inputs = load_telemetry(results[0] / "telemetry.jsonl")
    check_responses(scenario, frames, inputs)

    if runs == 2:
        print("== Determinism ==")
        a = strip_volatile(results[0] / "telemetry.jsonl")
        b = strip_volatile(results[1] / "telemetry.jsonl")
        check(a == b and len(a) > 0,
              f"determinism: {len(a)} telemetry lines identical across runs",
              f"{len(a)} vs {len(b)} lines")

    print(f"\n{name}: {PASSED} passed, {FAILED} failed"
          + ("" if workdir else f"  (artifacts in {base})"))
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
