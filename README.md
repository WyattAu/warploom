# Warploom

A data-oriented C++ engine with deterministic simulation, Vulkan rendering,
an ECS foundation, and a time-warp editor; shipped as seven installable
CMake packages (`warploom_asset`, `warploom_core`, `warploom_editor`,
`warploom_engine`, `warploom_physics`, `warploom_render`, `warploom_ui`).

The differentiator is **determinism as product**: a session can be recorded,
replayed, and scrubbed to any frame, and the state comes back exactly —
physics included (T4a checkpoints restore the full solver state
bit-identically). 224 assertions of that run over real sockets, viewport and
headless hosts (`tools/live_proof.py all`; measured 2026-11-06, extended per
feature; the headless subset runs in CI).

## Build

```bash
cmake --preset default
cmake --build build/default -j$(nproc)
ctest --test-dir build/default --output-on-failure
./build/default/bin/warploom_viewport
```

That is the whole path. Shaders are compiled by the build; the viewport
finds them without any environment variable.

**Requirements** (Linux): CMake ≥ 3.28, Ninja, a C++26 compiler, Vulkan
headers, `libxcb`, and `glslc` or `glslangValidator` for the shaders.
`apt install cmake ninja-build g++ libvulkan-dev libxcb1-dev pkg-config
glslang-tools`.

### Presets

| Preset | Purpose |
|---|---|
| `default` | modules + tests + viewport, with whatever toolchain CMake finds |
| `headless-debug` / `headless-release` | no Vulkan, no window; the fast inner loop |
| `headless-debug-clang` | the same under Clang |
| `asan-ubsan` / `tsan` | sanitizer legs |
| `vulkan-validation` | Khronos validation layer on Mesa lavapipe |
| `release` | optimised build |

Each has a matching `--build` and `ctest --preset` entry. About 60 more
(compiler, cross and Nix combinations) are `hidden`; reachable with
`--preset=<name>`, absent from `--list-presets`.

## Architecture

Six installable libraries and one headerless aggregate:

| Package | Target | Contents |
|---|---|---|
| `WarploomCore` | `Warploom::core` | deterministic runtime, document/JSON, ECS, node graph, replay/scrubber/recorder, protocol + session, input, physics snapshot codec |
| `WarploomUI` | `Warploom::ui` | widget primitives, glyph atlas |
| `WarploomEditor` | `Warploom::editor` | node editor, inspector, graph→animation bridge, clip timeline view |
| `WarploomRender` | `Warploom::render` | Vulkan context/swapchain/pipelines, render graph, RT, IBL, GPU-driven draw, H-Z occlusion, software rasterizer |
| `WarploomAsset` | `Warploom::asset` | glTF/GLB + skeletal animation import, PNG/JPEG/KTX2 decoding |
| `WarploomPhysics` | `Warploom::physics` | header-only deterministic solver: spheres/capsules, XPBD, distance joints, speculative TOI contacts, uniform-grid broadphase |
| `WarploomEngine` | `Warploom::engine` | INTERFACE aggregate over all five |

Each installs its own `Warploom<Name>Config.cmake` and version file, so a
consumer needs only `find_package(WarploomRender)` and a link line. There
is no monolithic engine target.

```
modules/core     16,500 lines   30 headers, 18 TUs
modules/render   21,549 lines   35 headers, 30 TUs
modules/asset     6,707 lines    8 headers,  7 TUs
modules/physics   2,083 lines    1 header,  solver only
modules/editor    3,671 lines    5 headers,  9 TUs
modules/ui        1,342 lines    2 headers,  4 TUs
examples/viewport  9,475 lines   the application
tests/           56,186 lines   879 tests in five test binaries
```

Public headers are spelled `warploom/<module>/<header>.hpp` and reached
through each module's own interface include directory. There is no
superproject-wide include path.

## What actually works, and what does not

Read [`docs/rendering-status.md`](docs/rendering-status.md) before trusting
any capability list, including this one. It separates three questions that
are easy to conflate; is it in the engine, can the app reach it, and what
proves it.

The short version; every claim carries its proof in
[`docs/rendering-status.md`](docs/rendering-status.md):

**Strong.** Ray tracing is real: acceleration structures, an RT pipeline with
a shader-binding table, `vkCmdTraceRaysKHR`, and a loop path tracer checked
against an independent fp64 Monte-Carlo integrator — with adaptive sampling
and an edge-stopped à-trous denoiser live in the render path. The viewport
records through the renderer module (Phase B closed the hand-rolled gap).
Physics is a real deterministic solver (spheres/capsules, distance joints,
XPBD compliance, speculative TOI contacts, a uniform-grid broadphase proven
equivalent to all-pairs at 200–8000 bodies), the session sim is wire-driven
and scrub-restorable bit-identically (T4a), the timeline interpolates
sub-frame, and the document projects into one ECS driven by one tick.

**Not yet true** (the current honest gaps, each a named roadmap item):

- **The gd demo scene still steps its own physics world.** The session sim is
  the deterministic, replayable one; the viewport's `WARPLOOM_PHYSICS=1`
  scene runs an app-local `PhysicsWorld` stepped per window frame. Migrating
  it onto the session sim is the remaining D1 follow-up.
- **Some renderer surfaces are still test-only.** The render-graph
  single-entry path, `record_pbr_frame`, `VulkanOffscreenTarget` and the
  parallel recorder are tested but have no production caller — the app drives
  the two GPU-driven halves separately because it renders a shadow pass
  between them.
- **Timeline selection, not capability, is missing.** Sub-frame interpolation
  works and replays stay deterministic, but no document field or wire command
  stores a per-clip easing (the app reaches Linear live, Step at whole
  frames), and lights have no track path.
- **Every ray-tracing test skips in CI** (Mesa lavapipe has no RT
  extensions), and on the shared RTX 2060 the GPU suite is VRAM-gated
  (loop81: 774/779 under trainer memory pressure, with a varying failing
  set). E3 — a hardware runner — is open.
- **PT usability at budgets** is proven only on the test scene: the tracer
  snapshots a static view and holds the camera while active. The app-scene
  budget proof is post-0.1 feature work.

## Validation

| Check | Status |
|---|---|
| `ctest` | 6/6 suites |
| `default` preset test count | 879 tests in five binaries (779 unit + 22 core + 49 editor + 19 ui + 10 runtime; counted 2026-11-08 via `--gtest_list_tests`) |
| Full suite on hardware (RTX 2060) under `VK_LAYER_KHRONOS_validation` | 0 diagnostics, 0 leaks — VRAM-gated when a trainer holds memory (loop81: 774/779, varying failing set = contention, not a fault) |
| `headless-debug`, `asan-ubsan`, `tsan` | all four presets build warning-free on GCC and clang; sanitizers 0 diagnostics, 0 leaks, 0 races (2026-11-06 re-measure) |
| `live_proof.py all` over real sockets | 224/224 (2026-11-06; the headless subset runs in CI) |
| `cpack` | 353-file package: modules, headers, package configs, shaders, viewport |

The Vulkan CI leg is Mesa lavapipe, which has no ray-tracing extensions, so
every RT test skips there. Hardware RT is verified locally, not in CI.

### Sanitizer and headless presets

All four presets build warning-free on both clang and GCC, and the sanitizer
presets are now actually run rather than merely configured.

`asan-ubsan` (Debug, `WARPLOOM_USE_VULKAN=OFF`) reports 0 AddressSanitizer
diagnostics, 0 UndefinedBehaviorSanitizer diagnostics and 0 leaks across 673
unit tests plus the 48 editor, 22 core, 19 ui and 10 runtime suites. `tsan`
reports 0 data races over the same suites, which is the first real
verification of the job system's concurrency claims. `headless-debug` builds
clean and is what CI's live-proof job uses.

Reaching that took paying every warning in a configuration the default preset
never compiles, which is worth knowing: a clean build in one configuration is
not a clean build. It also turned up two memory-safety defects that no clang
warning had reported -- a dangling pointer in the property registry and a null
dereference in the latency telemetry -- and a logic bug in `default_registry()`
where each `register_type` call overwrote the previous success flag, directly
under a comment claiming every registration was checked.

`asan-ubsan` and `tsan` had a second problem: their binary directories carried
a `CPM_DIRECTORY` cache entry pointing at the location CPM used before it was
vendored in-tree, so the vendored script treated it as a foreign newer version
and returned without ever defining `CPMAddPackage`. That made configure fail
outright for anyone whose build directory predated vendoring. `cmake/CPM.cmake`
now detects and clears the stale pointer, so old build directories heal
themselves.


## Layout

```
modules/            the six libraries + the aggregate, each a CMake package
examples/viewport/  the windowed application (XCB + Vulkan, native Wayland surface)
tools/              headless control host used by the live-proof harness
assets/shaders/     82 shader sources, compiled by cmake/Shaders.cmake
tests/              suites, module fixtures, the live-proof harness
cmake/              build system: presets live in CMakePresets.json
docs/               architecture, roadmap, capability status
```

## Licence

MIT; see [`LICENSE`](LICENSE).