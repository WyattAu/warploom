#!/usr/bin/env python3
"""Check the caller/callee contract of the shared C++ gate matrix.

`.github/workflows/test.yml` is a thin caller; the gates live in
`.github/workflows/cpp-gates.yml` (`on: workflow_call`) so the estate can
adopt the file verbatim. This script fails CI when the two drift:

1. the callee declares `on: workflow_call`;
2. the caller has a job whose `uses:` points at the callee;
3. every input the caller passes is declared by the callee;
4. every gate in the matrix below exists as a callee job;
5. every checkout in both files sets `persist-credentials: false`
   (estate convention, cf. go-kit.yml).

Unlike check_docs_links.py this is a gate, not an audit: PyYAML missing is
a hard failure, and the `contract` job pip-installs it first.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("check_ci_contract: PyYAML is required (pip install pyyaml)")
    sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent
CALLER = ROOT / ".github" / "workflows" / "test.yml"
CALLEE = ROOT / ".github" / "workflows" / "cpp-gates.yml"

# The warploom gate matrix: every id here must exist as a callee job.
MATRIX = (
    "contract",
    "format-check",
    "headless-debug",
    "headless-debug-clang",
    "headless-release",
    "tsan",
    "asan-ubsan",
    "vulkan-validation",
    "live-proof",
    "docs",
    "examples",
    "presets",
)


def fail(message: str) -> int:
    print(f"check_ci_contract FAILED: {message}")
    return 1


def main() -> int:
    caller = yaml.safe_load(CALLER.read_text(encoding="utf-8"))
    callee = yaml.safe_load(CALLEE.read_text(encoding="utf-8"))

    # NOTE: YAML 1.1 parses the `on:` key as boolean True. Both spellings
    # are accepted so the check does not depend on the loader version.
    on = callee.get("on", callee.get(True, {}))
    if "workflow_call" not in on:
        return fail("cpp-gates.yml does not declare on: workflow_call")
    declared_inputs = set((on.get("workflow_call") or {}).get("inputs", {}))

    caller_jobs = caller.get("jobs", {})
    uses_jobs = {
        name: job.get("uses", "")
        for name, job in caller_jobs.items()
        if isinstance(job, dict) and "uses" in job
    }
    if not any("cpp-gates.yml" in uses for uses in uses_jobs.values()):
        return fail("test.yml has no job using cpp-gates.yml")
    for name, uses in uses_jobs.items():
        if "cpp-gates.yml" not in uses:
            continue
        for passed in (caller_jobs[name].get("with") or {}):
            if passed not in declared_inputs:
                return fail(
                    f"caller passes undeclared input {passed!r} "
                    f"(declared: {sorted(declared_inputs)})"
                )

    callee_jobs = callee.get("jobs", {})
    for gate in MATRIX:
        if gate not in callee_jobs:
            return fail(f"gate {gate!r} missing from cpp-gates.yml")

    for path, doc in ((CALLER, caller), (CALLEE, callee)):
        for job_name, job in doc.get("jobs", {}).items():
            if not isinstance(job, dict):
                continue
            for step in job.get("steps", []) or []:
                if not isinstance(step, dict):
                    continue
                if str(step.get("uses", "")).startswith("actions/checkout@"):
                    with_ = step.get("with") or {}
                    if with_.get("persist-credentials") is not False:
                        return fail(
                            f"{path.name} job {job_name!r}: checkout "
                            "without persist-credentials: false"
                        )

    print(f"check_ci_contract: caller/callee agree ({len(MATRIX)} gates)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
