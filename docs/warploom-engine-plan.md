# warploom-engine — S5-A gate

Status: GATE (aggregate not started; this document is the audit + contract)

## 1. What S5-A is

The last S-track extraction: the engine **aggregate**. `Warploom::engine`
is an INTERFACE target whose package config `find_dependency`s the five
real modules (core, ui, editor, render, asset). With it in place:

- the viewport links `Warploom::engine` instead of `omnicpp_runtime`;
- the root test targets and the headless host do the same;
- the zero-source `omnicpp_runtime` shell (S3.5) is **deleted** — the
  moment the monolith stops being load-bearing (0.1 gate criterion #2);
- standalone consumers do `find_package(WarploomEngine CONFIG REQUIRED)`
  and get every module's usage requirements transitively.

Nothing else changes: no namespace moves (S5-B), no spelling changes
(the `engine/*` forwarders stay and are still how in-tree code spells
includes), no test placement changes. S5-A is pure CMake surgery plus
target renames at consumer sites.

## 2. Audit (measured)

### 2.1 The aggregate module

`modules/engine/` follows the module pattern minus everything a library
needs that an INTERFACE target doesn't:

- `add_library(warploom_engine INTERFACE)` +
  `target_link_libraries(warploom_engine INTERFACE Warploom::core
  Warploom::ui Warploom::editor Warploom::render Warploom::asset)`
- `Warploom::engine` alias is the target itself (EXPORT_NAME engine);
- install: EXPORT `WarploomEngineTargets` + Config + version file; **no
  headers, no DSO** (`rename-and-modules.md` §5 already scopes
  warploom-engine as the aggregate with deps on all of the above);
- `cmake/WarploomEngineConfig.cmake.in`: `find_dependency` ×5
  (WarploomCore, WarploomUI, WarploomEditor, WarploomRender,
  WarploomAsset) — Threads flows transitively from core/asset/ui.
- Editor module standalone note: WarploomEditorConfig already
  find_dependencies WarploomCore+UI, and the editor tests' rasterizer
  seam is in-tree-only, so the aggregate adds nothing the modules don't
  already export.

### 2.2 Who consumes `omnicpp_runtime` today (all switch to Warploom::engine)

| consumer | file | notes |
|---|---|---|
| viewport | `examples/viewport/CMakeLists.txt:11` | PRIVATE; add `${CMAKE_SOURCE_DIR}/include` include dir for the `engine/*` forwarder spellings (runtime used to supply it) |
| unit tests | `tests/CMakeLists.txt:784` | keep the explicit `${CMAKE_SOURCE_DIR}/include` (already present, line 785) |
| contract violator | `tests/CMakeLists.txt:824` | include dir already explicit (line 825) |
| runtime-only tests | `tests/CMakeLists.txt:836` | include dir already explicit (line 837) |
| runtime benchmark | `tests/CMakeLists.txt:842` | add include dir (check current sourcing) |
| headless host | `tools/CMakeLists.txt:12` | include dir already explicit (lines 9–10) |
| legacy/integration | `tests/CMakeLists.txt:853,859` | guarded by `OMNICPP_LEGACY_ENGINE` (OFF) — reference `omnicpp_engine`, not the runtime; untouched |

The 42 render/asset/etc test TUs keep compiling through the same
forwarder spellings; only the link target name changes.

### 2.3 What gets deleted / kept

- **Deleted:** the `omnicpp_runtime` target (S3.5 left it a zero-source
  INTERFACE shell — deletion is mechanical; nothing references the
  artifact name, no dlopen surface).
- **Kept:** the `omnicpp_*` ALIAS block as compatibility aliases of the
  real module targets (`omnicpp_core/ui/editor/render_lib/asset` —
  already real-target aliases) plus a new
  `add_library(omnicpp_runtime ALIAS warploom_engine)` so any straggler
  spelling keeps configuring; `omnicpp_render` INTERFACE rasterizer shim
  (links Warploom::render) stays for the editor seam.
- **Kept:** `include/engine/*` forwarders verbatim — they are the S5-B
  migration surface, not S5-A's.

## 3. Mechanical plan

1. `modules/engine/{CMakeLists.txt,cmake/WarploomEngineConfig.cmake.in}`
   per §2.1; root CMakeLists `add_subdirectory(modules/engine)` after
   the five modules (order irrelevant for INTERFACE, keep tidy).
2. `src/engine/CMakeLists.txt`: delete the `omnicpp_runtime` block;
   keep the compat aliases + rasterizer shim. (The directory keeps
   existing for the shim; it shrinks to ~20 lines.)
3. Repoint the six consumer sites (§2.2) to `Warploom::engine`; add the
   superproject include dir where the runtime used to supply it.
4. Install/export: `cmake --install` now lays six package configs
   (WarploomEngine added); the runtime DSO disappears from the install
   tree (it was already absent — the shell installed no artifact since
   S3.5's INTERFACE conversion; verify install log shrinks accordingly).

## 4. Correctness risks and mitigations

1. **Include-dir regression** — the runtime shell carried
   `${CMAKE_SOURCE_DIR}/include` PUBLIC; consumers relying on it
   transitively lose it. Mitigation: every consumer site audited
   (§2.2); explicit include dirs added where missing; the 4-leg matrix
   is the compile-time proof (any miss fails the build, not silently).
2. **Link-order/topology** — INTERFACE aggregate changes nothing about
   the five DSOs or their inter-dependencies (core ← ui/editor/render/
   asset — unchanged since S3.5). The suite's link lines are built by
   CMake from the same module targets as before.
3. **Standalone consumer parity** — the four existing consumers +
   asset/render proofs must still pass against a fresh install that now
   also ships WarploomEngine; a new `/tmp/engine_consumer` proves
   `find_package(WarploomEngine CONFIG REQUIRED)` + one
   `Warploom::engine` link reaching a core type AND an asset function
   (proves transitive find_dependency wiring end to end).
4. **0.1 gate wording** — criterion #2 says "viewport and tests consume
   `find_package(Warploom …)`". In-tree, CMake subdirectories consume
   the target directly (aliases), which is the same dependency shape;
   the standalone `find_package` path is proven by the consumers (§4.3)
   and becomes the viewport's own mode when the repo publishes (S5-B /
   P3). Roadmap criterion text stays as written.

## 5. Verification plan (machine-checked)

1. Four-leg matrix × 6 suites (headless leg re-proves the no-Vulkan
   path with the aggregate in the chain).
2. Fresh install (`rm -rf /tmp/editor_install`); expect SIX package
   config dirs and NO runtime artifact.
3. Five existing consumers re-proofed + new ENGINE_CONSUMER_OK
   (`/tmp/engine_consumer`) through `find_package(WarploomEngine)`.
4. 64/64 live proofs (systemd hosts, positional socket path).
5. `python3 scripts/check_docs_links.py`; roadmap S5-A `[x]` +
   sequencing note update.

## 6. Non-goals (S5-B scope, explicitly out)

- Namespace footer/re-badge for `omnicpp::editor`/`omnicpp::render`/
  `omnicpp::asset`.
- `OMNICPP_*` → `WARPLOOM_*` flags/defines, binary renames, C ABI
  dual-export shim, contract string, repo rename.
- Any change to the `engine/*` forwarder spellings.
