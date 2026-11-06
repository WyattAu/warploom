# Quick Start Guide

Warploom is a deterministic C++ engine (rendering + simulation). The supported baseline is Linux with CMake and a recent compiler (targets require C++26 language features); Vulkan is optional — headless builds run without it.

## Prerequisites

- C++26-capable compiler
- CMake 3.28 or later
- Python 3.11 or later
- Ninja (recommended)

## Configure and build the portable baseline

The following builds the portable baseline without Vulkan or the examples, and is useful for validating the core build. The headless runtime supports fixed-step execution and configurable catch-up behavior; use the default policy for deterministic replay, or configure a tick cap when bounded recovery from stalls is required:

```bash
cmake -S . -B build -G Ninja \
  -DWARPLOOM_USE_VULKAN=OFF \
  -DWARPLOOM_BUILD_EXAMPLES=OFF
cmake --build build
```

Enable only dependencies installed on your system. Do not rely on hard-coded SDK paths.

## Run tests

```bash
cmake -S . -B build-tests -G Ninja \
  -DWARPLOOM_BUILD_TESTS=ON \
  -DWARPLOOM_USE_VULKAN=OFF
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Python tests can be run after package installation or with the repository root on `PYTHONPATH`:

```bash
PYTHONPATH=. python -m pytest -q
```

## Deterministic runtime policy

The canonical headless runtime defaults to `run_all`, which preserves all elapsed simulation time. Applications with real-time deadlines should configure either `cap_and_drop_time` or `cap_and_report_overrun` and monitor the overrun counters rather than allowing an unbounded catch-up loop.

## Status

Warploom is pre-0.1: the publish gate (clone-and-build, installed packages, demo story, path-tracing feature) is tracked in docs/roadmap.md.

## Next steps

- Read the [API Documentation](../api/overview.md)
- Review the [Developer Guide](../developer-guide.md)
- Inspect the [build documentation](../user-guide-build-system.md)
