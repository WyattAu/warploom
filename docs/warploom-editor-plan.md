# warploom-editor — S4 dependency analysis and migration plan

Status: **plan** (docs/roadmap.md track S). Gate document for S4, same
discipline as `docs/warploom-core-plan.md`: what `warploom-editor` owns,
what it must not take, what breaks, and the order of operations. Counts
below are measured on this tree, not estimated.

## Measured starting point

- `include/engine/editor/`: **3 public headers** (`node_editor.hpp` 280
  lines, `inspector.hpp` 94, `graph_anim_bridge.hpp` 87);
  `src/engine/editor/`: **2 translation units** (`node_editor.cpp` 871,
  `inspector.cpp` 216). Total extraction surface: **~1.5k LOC** — an order
  of magnitude smaller than S2's 84-include-site core move.
- **6 consumers** include an `engine/editor/*` header: the viewport
  (`examples/viewport/main.cpp`, 3 headers), the sources themselves
  (2), and 3 unit tests (`test_node_editor.cpp`, `test_inspector.cpp`,
  `test_graph_anim_bridge.cpp`). `tools/headless_host.cpp` touches the
  layer only through `omnicpp::editor::EditorSession` (core — no widget
  header).
- Dependency direction is already correct: both sources include only
  `engine/core/*` (control_server contract, node_graph, document,
  editor_session, property_registry, prop_value) + `warploom/ui/widget.hpp`
  + std. **Zero render/asset includes.** `graph_anim_bridge.hpp` is
  header-only and core-only (no ui include at all).
- Namespace today: all 3 widget headers declare `omnicpp::editor` (with
  the S1 `namespace ui = ::warploom::ui;` alias inside node_editor.hpp and
  inspector.hpp).

## The truthful `omnicpp::editor` split (S4 owns this)

S2's audit was conservative; this re-audit corrects one finding and pins
the full map. **`include/engine/render/vulkan_parallel_recorder.hpp` does
NOT declare `omnicpp::editor`** (it opens `omnicpp::core` then
`omnicpp::render`) — no render header needs to move for this split. The
complete measured map of `omnicpp::editor`-declaring files:

- **7 core headers** (stay in `warploom-core`, own the document/session
  identity): `document.hpp`, `editor_session.hpp`, `node_graph.hpp`,
  `property_registry.hpp`, `prop_value.hpp`, `command_recorder.hpp`,
  `replay_scrubber.hpp`.
- **3 widget headers** (move to `warploom-editor`): `node_editor.hpp`,
  `inspector.hpp`, `graph_anim_bridge.hpp`.
- **Non-header residents**: `tools/headless_host.cpp`,
  `examples/viewport/main.cpp`, and 8 unit tests merely *use* the
  namespace; `tests/unit/test_command_recorder.cpp` and friends use the
  core half.

Decision: the namespace name itself does NOT change in S4 (that is the
S2 phase-B / S-decision identifier pass). S4 makes the split *truthful*
by ownership: after this move, every `omnicpp::editor`-declaring header in
`warploom-editor` is a widget/view header and every one in `warploom-core`
is a document/session header. A file-scoped rename in phase B then has a
mechanical rule: core headers → `warploom::editor`, widget headers →
`warploom::editor` (module decides the shared name; the *packages* already
distinguish them). Nothing needs to be re-badged post-hoc anymore.

## Boundary decision

**warploom-editor owns** (view/projection layer over core + ui):

- `node_editor` — `NodeEditorView` (pure projection from `NodeGraph` to
  the widget tree), wire routing, pin/link/param-row hit-testing, link
  drag + param edit state machines, `value_to_text`, `pin_canvas`,
  toolbar build/hit-test.
- `inspector` — `InspectorPanel` (outliner/properties/bindings
  projection), `InspectorHit`, binding anchors.
- `graph_anim_bridge` — `GraphSignalAdapter` (graph outputs →
  `InputSnapshot` actions for the animation state machine).

**warploom-editor does NOT take** (measured, each with a reason):

- `TimelinePanel` (main.cpp:186–325, ~140 lines) — tempting because it is
  widget code, but it is the viewport's W1 chrome (absolute-positioned
  bottom strip over the *window*, not a panel inside the editor UI tree)
  and it consumes `ReplayScrubber` directly. Stays in the viewport this
  session; a candidate for a later `warploom-editor` widget once G3
  (timeline panel) defines what the product surface actually is. Moving it
  now would freeze the wrong interface.
- `ViewportControlHost` (main.cpp:726–914) — the app's ControlHost
  binding; host-specific click routing (timeline + node editor +
  inspector dispatch) stays with the app until G3/S5 say otherwise.
- `setup_node_editor` / `tick_node_editor` (main.cpp:4450–4679) — the
  demo scene seeding and per-frame glue; they render the *scene* (GraphContext
  drives scene properties), which is app composition, not module surface.
- Anything under `render/` or `asset/` (S3), the headless host, the
  examples, the tests (superproject-side, same as S2 phase A).

## Viewport `main.cpp` decomposition (the budgeted part)

5,552 lines today; the roadmap budgeted a session for this alone. S4's
scope for main.cpp is deliberately narrow — delete what the module makes
redundant, don't attempt a grand refactor:

| Anchor | Line | Disposition |
|---|---|---|
| `TimelinePanel` | 186 | **Keep** (see boundary decision above) |
| `ViewportControlHost` | 726 | Keep; editor/inspector command delegation unchanged |
| Click routing (toolbar/inspector/node) | ~1150–1280 | Keep — thin, already module-shaped |
| `setup_node_editor` | 4450 | Keep — demo seeding is app content |
| `tick_node_editor` | 4568 | Keep — glue, shrinks only if module API absorbs it |

Net main.cpp delta after extraction ≈ −40 lines (includes + no code
moves). That is honest: **the real main.cpp decompression is G3 + G5**
(timeline panel, asset browser) landing as *new* module features instead
of main.cpp additions — the roadmap's stated motivation for extracting
before the next editor-facing feature.

## Strategy: phase A of S4 — the package, zero identifier churn

1. **New module tree** `modules/editor/` mirroring S1/S2:
   - `CMakeLists.txt`: `warploom_editor` SHARED, PIE on, `Warploom::editor`
     alias, `EXPORT_NAME editor`, own `WarploomEditorConfig.cmake` +
     version file, `find_dependency(WarploomUI)` + `find_dependency(Threads)`
     in the template (core is pulled transitively via ui? NO — ui does not
     expose core; the editor headers include `engine/core/*` forwarders,
     so the config template lists **`find_dependency(WarploomCore)` too**).
   - Sources git-mv from `src/engine/editor/*.cpp`; headers git-mv from
     `include/engine/editor/*` to `modules/editor/include/warploom/editor/*`.
     Internal include paths sed'd `engine/editor/` → `warploom/editor/`.
   - Public header allowlist = all 3 headers.
2. **Module tests**: git-mv the 3 unit tests into
   `modules/editor/tests/` as a new `warploom_editor_tests` binary
   (pattern: `warploom_ui_tests` / `WarploomCoreTests`). `test_node_editor.cpp`
   also includes `engine/render/software_rasterizer.hpp` for two golden-pixel
   tests — rasterization is a view concern and the test exercises the
   paint list end-to-end; keep the test whole by linking the
   `omnicpp_render` INTERFACE target (header-only, include-path only, no
   link-time dep). This is the only reason the module test binary needs
   the superproject include root; documented, not hidden.
3. **Compat layer**: `include/engine/editor/` keeps one-line forwarders
   (`#include <warploom/editor/X.hpp>`), exactly like the 22 core
   forwarders. All 6 consumer sites compile unchanged; installed package
   exposes only `warploom/editor/*`.
4. **Link updates**:
   - `src/engine/CMakeLists.txt`: drop `editor/*.cpp` from
     `omnicpp_runtime`; add `target_link_libraries(omnicpp_runtime PUBLIC
     Warploom::editor)` (PUBLIC: main.cpp includes the widget headers
     through the runtime's transitive usage). Note: `omnicpp_runtime` is
     now purely render+asset+glue; it keeps its name (binary rename is the
     S-decision pass).
   - `add_library(omnicpp_editor ALIAS warploom_editor)` next to the
     existing alias block.
   - `warploom_editor` links `PUBLIC Warploom::ui Warploom::core Threads::Threads`.
   - Viewport/test CMake unchanged (they link `omnicpp_runtime`, which now
     PUBLIC-links the editor).
5. **Root CMake**: `add_subdirectory(modules/editor)` after
   `modules/core` (order: ui → core → editor) under `OMNICPP_BUILD_ENGINE`.
6. **Verification gate (S4 phase-A exit)**:
   - 6/6 suites on all four CI legs (adds `WarploomEditorTests`; ui, core,
     unit, runtime-only, ui-gpu stay);
   - standalone scratch install + `find_package(WarploomEditor CONFIG
     REQUIRED)` consumer binary runs (`EDITOR_CONSUMER_OK`) — build a
     tiny NodeEditorView + InspectorPanel against a real NodeGraph/SceneDocument;
   - S1 ui-consumer and S2 core-consumer proofs still pass;
   - 37/37 `tools/live_proof.py` assertions on hardware (protocol path
     unchanged, but the runtime link graph changed — prove it).

## Risks / open questions

1. **SHARED + visibility**: editor TUs export no symbols consumed
   cross-DSO beyond what tests already use; S1/S2 pattern (SHARED + PIE)
   applies unchanged.
2. **`namespace ui = ::warploom::ui;` inside `omnicpp::editor`** moves with
   the headers verbatim — no churn until phase B.
3. **Test binary needs gtest + the superproject include root** (rasterizer
   header). The `if(TARGET gtest)` guard from modules/ui covers
   standalone-configure; the module's own smoke tests must not depend on
   the rasterizer (they don't — see allowlist note in CMake comments).
4. **Load order in `live_proof` / headless host**: the headless host links
   `omnicpp_runtime` only (session, no widgets) — it must keep working with
   the editor moved out of the runtime object list; this is exactly what
   the runtime-only suite + live proofs verify.
5. **Config template dependency graph**: `WarploomEditorConfig.cmake.in`
   must `find_dependency(WarploomCore)` AND `find_dependency(WarploomUI)`
   with explicit PATHS hints, because the exported target links both by
   name. S1's template needed no deps; S2's needed Threads; this is the
   first two-deep module — the consumer proof is the check.

## Sequencing within this session

1. Commit this plan (gate-first discipline).
2. git-mv 3 headers + 2 sources + 3 tests; write module CMake + config
   template + module tests wiring; sed internal includes; forwarders;
   link/alias fixups; root CMake entry.
3. Main leg green → remaining three legs.
4. Standalone editor consumer proof + ui/core consumer re-proofs.
5. Live proofs (37) on hardware.
6. Roadmap checkbox + docs update in the same commit as (2)–(5) results.
