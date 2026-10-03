# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

The project was renamed from OmniCPP to **Warploom**. Modules, namespaces,
build options, binaries and the script-module C ABI now carry the new name;
the compatibility surface decays on the schedule in
`docs/warploom-identity-plan.md`.

### Added

- `warploom-render`: Vulkan context, swapchain, pipelines, descriptors,
  render graph with computed barriers, IBL baker, ray-tracing pipeline with a
  shader-binding table, acceleration-structure builder, H-Z occlusion, GPU-driven
  draw path, offscreen target, software rasterizer.
- `warploom-asset`: glTF 2.0 / GLB import with skeletal animation, plus
  self-contained PNG, JPEG and KTX2 decoders.
- `warploom-core`: deterministic runtime with replay and state hashing,
  archetype ECS, versioned document, node graph, replay scrubber, command
  recorder, protocol and editor session, input abstraction, script-module
  loader.
- `warploom-editor`: node editor, inspector, graph→animation bridge, clip
  timeline view.
- `warploom-ui`: widget primitives and glyph atlas.
- `warploom-engine`: INTERFACE aggregate over the five modules. The
  `omnicpp_runtime` monolith is deleted.
- Timeline clips as document entities (schema v3) with undoable edits,
  record/play session arms, and axis-suffix property tracks.
- Protocol record/replay (`warploom-replay-v1`) with hash-verified hydration
  and opening-checkpoint restore.
- `tools/live_proof.py`: checked-in socket harness, 64 assertions over the
  scrubber, save/load round-trip, two-instance record/replay and timeline.
- CI: `examples` job that builds the viewport and asserts all shaders compile,
  and a `presets` job that checks the documented entry points configure.
- CMake `buildPresets` and `testPresets` for every preset.

### Changed

- **Shaders are a build product, not a test fixture.** `cmake/Shaders.cmake`
  defines one root-scope target that the tests and the viewport both depend
  on; the output directory reaches the app as a compile definition, so a
  fresh clone no longer needs `WARPLOOM_SHADER_DIR` set by hand. 690 lines of
  hand-written `add_custom_command` rules became one glob.
- `cmake --preset default` works on any machine. It previously aborted on a
  hardcoded `/nix/store` `Qt6_DIR`.
- Every preset owns its build directory; 61 are `hidden` so
  `--list-presets` is readable.
- Dependencies consolidated to one: GoogleTest, via CPM, with a shared source
  cache. The phantom `quill`, `glm`, `stb`, `glfw` and `nlohmann_json`
  packages are gone, along with Conan.
- Every strict warning flag is probed against the actual compiler before use.
- `cpack` produces a working package; the version is declared once as
  `WARPLOOM_VERSION`.
- `docs/rendering-status.md` now separates *in the engine*, *reachable from
  the app* and *what proves it*.

### Fixed

- **Shadow depth bias was inert.** `vkCmdSetDepthBias` was recorded against
  pipelines that never declared `VK_DYNAMIC_STATE_DEPTH_BIAS`, so it was both
  a validation error and a no-op. The static slope factor and the dynamic
  bias are now separate parameters, and the viewport's shadow pass — which had
  no bias at all — records one.
- `shadow.frag` wrote a colour output to a render pass with no colour
  attachment.
- `VulkanOffscreenTarget`'s destructor called `cleanup(VK_NULL_HANDLE)`, and
  `cleanup` ignores a null device, so every offscreen target leaked its image,
  views, render pass and framebuffer.
- The ray-tracing shaders could not compile with `glslangValidator`, because
  the flag spelling was glslc's. CI installs only `glslang-tools`, so the
  shader build failed on that leg.
- `clang-tidy` was passed literal `**` globs (CMake does not glob) over
  directories that no longer contain code, so lint checked nothing.
- `.clang-format` could not be parsed — `[[nodiscard]]` was unquoted, which
  YAML reads as a nested sequence — so `format-check` had never run.
- `.clangd` pointed at a build directory that does not exist and declared
  `-std=c++17` against a C++26 build, leaving new contributors with no
  IntelliSense.

### Removed

- 58 `engine/*` compat forwarder headers and the superproject-wide include
  path that fed them.
- `legacy/`, `.archive/`, the pre-module template scaffolding (`include/game`,
  `math.hpp`, `string_utils.hpp`, `OmniCppLib`, the orphaned `src/main.cpp`),
  the pong and simple_game examples, and the unreferenced CMake modules.
- `WARPLOEM_LEGACY_ENGINE` and `WARPLOOM_BUILD_GAME`, whose targets no longer
  exist.
- Duplicate `WAYLAND_COMPOSITOR_QA_TEST_REPORT` files, with contradictory
  conclusions.

### Known issues

Deliberately recorded rather than hidden; see `docs/roadmap.md` Phase E.

- The viewport does not use the renderer module's own frame path; the render
  graph, H-Z occlusion, offscreen targets and post-processing are tested but
  have no production caller.
- Physics is 204 lines: semi-implicit Euler, spheres only, an O(n²)
  broadphase, no joints, and stepped outside the deterministic tick so replay
  cannot reproduce it.
- Timeline playback is step-hold; there is no interpolation, and only one
  track can record or play at a time.
- The ECS has no production consumer.
- `format-check` fails: 194 of 195 files do not match `.clang-format`.
- `WARPLOOM_WARNINGS_AS_ERRORS` defaults off against roughly 270 warnings.
- Every ray-tracing test skips in CI, which runs Mesa lavapipe; there is no
  hardware runner.