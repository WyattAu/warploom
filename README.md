# Warploom

A data-oriented C++ engine with deterministic simulation, Vulkan rendering,
an ECS foundation, and a time-warp editor — shipped as six installable
CMake packages (`warploom_asset`, `warploom_core`, `warploom_editor`,
`warploom_engine`, `warploom_render`, `warploom_ui`).

The differentiator is **determinism as product**: a session can be recorded,
replayed, and scrubbed to any frame, and the state comes back exactly. 64
assertions of that run over a real socket in CI (`tools/live_proof.py`).

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
| `headless-debug` / `headless-release` | no Vulkan, no window — the fast inner loop |
| `headless-debug-clang` | the same under Clang |
| `asan-ubsan` / `tsan` | sanitizer legs |
| `vulkan-validation` | Khronos validation layer on Mesa lavapipe |
| `release` | optimised build |

Each has a matching `--build` and `ctest --preset` entry. About 60 more
(compiler, cross and Nix combinations) are `hidden` — reachable with
`--preset=<name>`, absent from `--list-presets`.

## Architecture

Five installable libraries and one headerless aggregate:

| Package | Target | Contents |
|---|---|---|
| `WarploomCore` | `Warploom::core` | deterministic runtime, document/JSON, ECS, node graph, replay/scrubber/recorder, protocol + session, input, physics |
| `WarploomUI` | `Warploom::ui` | widget primitives, glyph atlas |
| `WarploomEditor` | `Warploom::editor` | node editor, inspector, graph→animation bridge, clip timeline view |
| `WarploomRender` | `Warploom::render` | Vulkan context/swapchain/pipelines, render graph, RT, IBL, GPU-driven draw, H-Z occlusion, software rasterizer |
| `WarploomAsset` | `Warploom::asset` | glTF/GLB + skeletal animation import, PNG/JPEG/KTX2 decoding |
| `WarploomEngine` | `Warploom::engine` | INTERFACE aggregate over all five |

Each installs its own `Warploom<Name>Config.cmake` and version file, so a
consumer needs only `find_package(WarploomRender)` and a link line. There
is no monolithic engine target.

```
modules/core     11,470 lines   22 headers, 11 TUs
modules/render   13,333 lines   27 headers, 22 TUs
modules/asset     6,410 lines    6 headers,  6 TUs
modules/editor    3,278 lines    4 headers,  3 TUs
modules/ui        1,329 lines    2 headers,  2 TUs
examples/viewport  5,553 lines   the application
tests/           37,246 lines   580 tests across 99 suites
```

Public headers are spelled `warploom/<module>/<header>.hpp` and reached
through each module's own interface include directory. There is no
superproject-wide include path.

## What actually works, and what does not

Read [`docs/rendering-status.md`](docs/rendering-status.md) before trusting
any capability list, including this one. It separates three questions that
are easy to conflate — is it in the engine, can the app reach it, and what
proves it.

The short version:

**Strong.** Ray tracing is real: acceleration structures, an RT pipeline with
a shader-binding table, `vkCmdTraceRaysKHR`, and a loop path tracer checked
against an independent fp64 Monte-Carlo integrator. The render graph, IBL,
PBR, GPU-driven cull→indirect, glTF and skeletal import, and the
determinism/replay/scrub/timeline stack are all implemented and tested.

**Not yet true.** Three things the project is sometimes described as having,
that it does not:

- **Physics is 204 lines.** Semi-implicit Euler, spheres only, an O(n²)
  broadphase, no shapes, no joints, and it is stepped outside the
  deterministic tick — so replay cannot reproduce it.
- **The app does not use the renderer module.** The viewport hand-rolls its
  own Vulkan. `record_pbr_frame`, the render graph, H-Z occlusion, offscreen
  targets and the whole bloom/tonemap chain are real and tested but have no
  production caller. The application is a weaker renderer than its test suite
  proves.
- **The timeline steps rather than interpolates.** Clips, recording,
  playback and undo all work; playback picks the last key at or before the
  frame, so there are no curves, one armed track at a time, and the clip
  strip widget is not wired into the app.

## Validation

| Check | Status |
|---|---|
| `ctest` | 6/6 suites |
| Full suite on hardware (RTX 2060) under `VK_LAYER_KHRONOS_validation` | 0 diagnostics, 0 leaks |
| `default` preset, under `VK_LAYER_KHRONOS_validation` | 580 tests, 0 diagnostics, 0 leaks |
| `headless-debug`, `asan-ubsan`, `tsan` | **do not build** — see below |
| `live_proof.py all` over real sockets | 64/64 |
| `cpack` | 353-file package: modules, headers, package configs, shaders, viewport |

The Vulkan CI leg is Mesa lavapipe, which has no ray-tracing extensions, so
every RT test skips there. Hardware RT is verified locally, not in CI.

### Sanitizer and headless presets

All four presets build warning-free on both clang and GCC, and the sanitizer
presets are now actually run rather than merely configured.

`asan-ubsan` (Debug, `WARPLOOM_USE_VULKAN=OFF`) reports 0 AddressSanitizer
diagnostics, 0 UndefinedBehaviorSanitizer diagnostics and 0 leaks across 540
unit tests plus the 39 editor, 10 core, 19 ui and 10 runtime suites. `tsan`
reports 0 data races over the same 540 tests, which is the first real
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
modules/            the five libraries + the aggregate, each a CMake package
examples/viewport/  the windowed application (XCB + Vulkan)
tools/              headless control host used by the live-proof harness
assets/shaders/     73 GLSL sources, compiled by cmake/Shaders.cmake
tests/              suites, module fixtures, the live-proof harness
cmake/              build system: presets live in CMakePresets.json
docs/               architecture, roadmap, capability status
```

## Licence

MIT — see [`LICENSE`](LICENSE).