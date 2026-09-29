# warploom-render — S3 gate

Status: GATE (extraction not started; this document is the audit + contract)

## 1. What moves

The render layer of the monolith: 27 public headers under
`include/engine/render/` and 22 translation units under
`src/engine/render/`. After S3 they live in `modules/render/` as
`warploom_render` (exported `Warploom::render`, project `WarploomRender`,
package `WarploomRender`), exactly mirroring the S1/S2/S4 module pattern:

- SHARED library, POSITION_INDEPENDENT_CODE, `cxx_std_23`
- `Warploom::<name>` alias, `EXPORT_NAME render`
- own `Config.cmake.in` + `write_basic_package_version_file`
- GLOB header allowlist (`include/warploom/render/*.hpp`)
- module tests guarded by `if(TARGET gtest)`
- compat forwarders `include/engine/render/X.hpp` → one-line include of
  `warploom/render/X.hpp`

Names stay `omnicpp::render` this phase — S3 is an **include-path and
packaging extraction only**. The `render` → `warploom::render` identifier
migration is S5/S-decision scope (the S2-B scheme needs a real first-class
module first; this is that module). `vulkan_ui_renderer.hpp`'s existing
`namespace ui = ::warploom::ui;` alias inside `omnicpp::render` is the
model: namespace **spelling** changes are out of scope, existing ones are
left untouched.

## 2. Dependency audit (measured)

### 2.1 What render consumes (must become module deps)

| dependency | usage | becomes |
|---|---|---|
| deterministic_runtime.hpp | 18 of 27 headers | `Warploom::core` PUBLIC |
| ecs.hpp | scene_camera, vulkan_scene | (already in core) |
| job_system.hpp | vulkan_parallel_recorder header (S2-B: real include replaced the fwd-decl — a real member declaration hides directive-injected types) | (already in core) |
| latency_telemetry.hpp | vulkan_renderer header | (already in core) |
| clock.hpp, contract.hpp | TUs only | PRIVATE via core |
| warploom/ui/widget.hpp | vulkan_ui_renderer.hpp (S1 alias) | `Warploom::ui` PUBLIC |
| warploom/ui/glyphs.hpp | vulkan_ui_renderer TU | PRIVATE via ui |

All five core headers render needs (`deterministic_runtime`, `ecs`,
`job_system`, `latency_telemetry`, `clock`/`contract`) are already
`modules/core/include/warploom/core/*` post-S2 — **no core change
required**. Render headers must include `warploom/core/*` roots directly
(never `engine/core/*` forwarders — S4 banked lesson).

### 2.2 Vulkan plumbing (the S3-specific risk)

Unlike S1/S2/S4, this module has a hard native dependency:

- `vulkan_types.hpp` includes `<vulkan/vulkan.h>` → **PUBLIC**
  `Vulkan::Vulkan` on the module target, plus `OMNICPP_HAS_VULKAN` as a
  **PUBLIC** compile definition (3 public headers gate declarations on
  it: vulkan_context, vulkan_renderer, vulkan_types).
- xcb / vulkan_xcb / win32 appear **only in TUs** → PRIVATE, with the
  `VK_USE_PLATFORM_XCB_KHR` definition PRIVATE too.
- `find_package(Vulkan QUIET)` moves into the module (with a graceful
  degrade path preserved: the monolith builds with Vulkan absent today,
  and must keep doing so). In-tree, the superproject's
  `Vulkan_FOUND` (cmake/FindDependencies.cmake) is respected.

Exported target carries the include dirs and definitions via
`target_include_directories`/`target_compile_definitions` PUBLIC so a
standalone `find_package(WarploomRender)` consumer compiles headers
without extra plumbing.

### 2.3 What consumes render (include sites)

| consumer | headers used | mechanism after S3 |
|---|---|---|
| `examples/viewport/main.cpp`, `telemetry.hpp` | renderer/graph/etc | forwarders (unchanged spellings) |
| 42 `tests/unit/test_*.cpp` TUs | full surface | forwarders |
| `modules/editor/tests/test_node_editor.cpp` | software_rasterizer.hpp | forwarder + superproject include seam (in-tree only, already guarded) |
| engine glue TUs (main.cpp, asset) | — | none (link-time only) |

Grep across `src include examples tests modules tools` finds **no other
engine-layer includes** of `engine/render/*` (e.g. nothing in
`src/engine/asset/`). 44 total include sites, all kept compiling via
forwarders with zero source edits.

### 2.4 What stays in the monolith

`src/engine/asset/*` (6 TUs) stays — asset is render-adjacent but
consumed only by glue today; its extraction is not S3 scope.
`omnicpp_runtime` keeps: asset TUs + glue, and links `Warploom::render`
PUBLIC (public headers of the glue include render forwarders → module
include dirs must flow transitively; PUBLIC not INTERFACE, the DSO now
carries render code, so link order/symbols stay correct for the
`dlopen`-style single-ABI contract).

## 3. Mechanical plan

1. `git mv` 27 headers → `modules/render/include/warploom/render/`;
   rewrite `engine/render/X.hpp` self-includes → `warploom/render/X.hpp`
   (from `git show HEAD:` content, **never** find-surgery on files with
   `// ====` doc dividers — S2-B lesson; line count must never shrink).
2. `git mv` 22 TUs → `modules/render/src/warploom/render/`; rewrite
   self-includes same way. TU includes of forwarders
   (`engine/core/*`) → `warploom/core/*` roots.
3. `modules/render/CMakeLists.txt` per the module pattern + §2.2 Vulkan
   handling. `WarploomRenderConfig.cmake.in` does
   `find_dependency(WarploomCore)` + `find_dependency(WarploomUI)` +
   `find_dependency(Vulkan)` (QUIET-tolerant like the in-tree branch) +
   `find_dependency(Threads)`.
4. Forwarders: 27 one-liners under `include/engine/render/` (same text
   pattern as S2's `engine/core/*` forwarders, with an S3 comment).
5. `src/engine/CMakeLists.txt`: drop the 22 render TUs, keep asset +
   glue, `target_link_libraries(omnicpp_runtime PUBLIC Warploom::ui
   Warploom::core Warploom::editor Warploom::render)`. The
   `omnicpp_render` INTERFACE target and the `omnicpp_render_lib` alias
   are re-pointed/replaced so existing target names keep working
   (`omnicpp_render` → INTERFACE alias of Warploom::render; alias-of-
   alias is forbidden, so it becomes a plain INTERFACE library that
   links `Warploom::render`).
   Root `add_subdirectory(modules/render)` **before** `src/engine`
   (CMake resolves plain names at call time; ORDER MATTERS — editor/core
   precede src/engine today for the same reason).
6. Vulkan plumbing moves with the module; superproject keeps its
   `OMNICPP_USE_VULKAN` option forwarded as
   `-DWARPLOOM_RENDER_USE_VULKAN` default from parent value.
7. Module tests: **no standalone gtest suite in the module this phase**
   (render tests are the 42-TU GPU/runtime suite, which already covers
   the full surface; per S1/S2/S4 the module keeps in-tree tests
   guarded, but a separate Vulkan-heavy gtest target would double-build
   22 TUs for zero assertion gain — decision recorded, revisit if the
   module gains headless-only logic). Superproject suites unchanged:
   all 42 test TUs keep compiling via forwarders and link the runtime
   which now pulls the render DSO transitively.

## 4. Correctness risks and mitigations

1. **PUBLIC compile definition loss** — `OMNICPP_HAS_VULKAN` gates
   declarations in 3 public headers. If it becomes PRIVATE, every
   consumer TU compiling those headers sees a different class layout →
   ODR. Mitigation: it is PUBLIC on the module target and flows to
   every consumer via the exported target; verified by the full suite
   compiling + `OMNICPP_HAS_VULKAN` grep from a consumer TU perspective
   (any miss fails at compile/link time, not silently).
2. **Vulkan optional** — headless-debug-clang leg is the canary: if
   Vulkan plumbing is mis-scoped, that leg fails first. Graceful
   degrade preserved (`WARPLOOM_RENDER_NO_VULKAN` state must still
   compile — the graceful path lives in TUs that already guard on
   `OMNICPP_HAS_VULKAN`).
3. **Link topology change** — render symbols move from
   `libomnicpp_runtime.so` to `libwarploom_render.so`. The runtime DSO
   links render PUBLIC so transitive link works for all 42 test TUs.
   `dlopen` surface unchanged (single-ABI contract, one DSO per module,
   no cross-DSO RTTI/dynamic_cast anywhere in the surface).
4. **Namespace stays** — no `warploom::render` identifiers yet; zero
   chance of the S2-B doubled-name trap (`warploom::editor::warploom::…`)
   this phase. ui alias inside `omnicpp::render` untouched.
5. **Forwarder-only consumer churn** — 44 include sites keep their
   spellings; the only text edits are *inside* the moved module.

## 5. Verification plan (machine-checked)

1. Four-leg matrix: `vulkan-validation`, `headless-debug-clang`, `tsan`,
   `asan-ubsan` — full 6-suite ctest (`timeout 850 ctest … -j4`).
2. Fresh install tree: `cmake --install build/vulkan-validation --prefix
   /tmp/editor_install` (rm -rf the old one first — headers change).
3. Consumers re-proofed fresh against the new install:
   `/tmp/core_consumer` (CORE_CONSUMER_OK), `/tmp/ui_consumer`
   (UI_CONSUMER_OK), `/tmp/editor_consumer` (EDITOR_CONSUMER_OK +
   G3B_CONSUMER_OK) — rebuild clean (`rm -rf` their build dirs first)
   since headers move. A new `/tmp/render_consumer` (RENDER_CONSUMER_OK)
   proves `find_package(WarploomRender CONFIG REQUIRED)` standalone with
   an `omnicpp::render` spelling (exercise the compat path).
4. Live proofs: 64/64 via systemd hosts (`all` mode on sock-b per g3).
5. `python3 scripts/check_docs_links.py` before doc commits.
6. Roadmap S3 `[x]` + sequencing note only after all of the above.

## 6. Non-goals

- No `warploom::render` identifier migration (S5/S-decision).
- No asset extraction.
- No viewport `main.cpp` decomposition beyond what forwarders give for
  free (G3/G5 scope).
- No new render features; the extraction is behavior-preserving by
  construction (pure CMake/file moves + forwarders).
