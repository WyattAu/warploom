# warploom-core — S2 dependency analysis and migration plan

Status: **plan** (docs/roadmap.md track S). This document is the gate the
roadmap requires before any target moves: what `warploom-core` owns, what it
must not take, what breaks, and the order of operations. Written after a
repository audit (counts below are measured, not estimated).

## Measured starting point

- `src/engine/core/`: 11 translation units, ~5.1k LOC; `include/engine/core/`:
  22 public headers.
- **84 files** across the repo include an `engine/core/*` header;
  **23 distinct include paths**; **72 files** use `omnicpp::core` or
  `omnicpp::editor`.
- Zero `engine/render` / `engine/asset` references inside core sources or
  headers; zero Vulkan references. Core is already UI-free (S1 consequence:
  nothing under `include/engine/core` includes `warploom/ui`).
- Direction of dependency is correct and one-way: 3 render files include
  `engine/core`; nothing in core reaches back.
- Consumers of the runtime target: `omnicpp_viewport`, `omnicpp_pong_example`,
  `simple_game`, the test suite (8 CMake references), the new
  `omnicpp_headless_host`, and the standalone warploom-ui consumer proof.

## Namespace audit (the hard part)

`omnicpp::core` (engine substrate) and `omnicpp::editor` (document/session/
protocol/scrubber/recorder) are declared in **18 core headers**, and the
`omnicpp::editor` namespace ALSO covers engine-side editor code that is NOT
part of warploom-core:

- `include/engine/editor/node_editor.hpp`, `inspector.hpp` and their sources
  (the widget-layer node editor, S1-aliased to `warploom::ui` inside);
- `include/engine/render/vulkan_parallel_recorder.hpp` (one render header
  declares `namespace omnicpp::editor`).
  *Correction (S4 re-audit, docs/warploom-editor-plan.md):* this header
  declares `omnicpp::core` then `omnicpp::render` — never `omnicpp::editor`.
  The conservative S2 claim was wrong in the safe direction; the truthful
  split is 7 core headers vs 3 widget headers, nothing else.

Therefore: `omnicpp::editor` is NOT a pure core namespace. A repo-wide
namespace sed would silently re-badge editor widgets and a render header as
"core". The migration must be file-scoped, not blind.

## Boundary decision

**warploom-core owns** (determinism substrate, headless, std-only +
warploom-ui-free):

- Deterministic runtime: `deterministic_runtime.hpp`, `engine.hpp/.cpp`,
  `clock.hpp`, `job_system.hpp`, `thread_pool.hpp`, `latency_telemetry.hpp`.
- State: `document.hpp/.cpp` (SceneDocument, byte-deterministic JSON,
  atomic save), `prop_value.hpp`, `property_registry.hpp/.cpp`, `ecs.hpp`,
  `physics_world.hpp`, `animation_state_machine.hpp`, `replay.hpp`.
- Editor document model (headless, no widgets): `node_graph.hpp/.cpp`,
  `replay_scrubber.hpp/.cpp`, `command_recorder.hpp/.cpp`,
  `control_server.hpp/.cpp` (protocol types + transport),
  `editor_session.hpp/.cpp`, `input_state`, `input_translators`,
  `script_module`.
- Namespace ownership moves with the files: `omnicpp::core` and the
  document/session parts of `omnicpp::editor` become `warploom::core` /
  `warploom::editor` as defined by warploom-core's headers. The engine-side
  editor widgets and the render recorder keep their current home — they will
  alias/consume core under S4/S3.

**warploom-core does NOT take**: `node_editor`/`inspector` (widget layer —
S4 `warploom-editor`), anything under `render/` or `asset/` (S3), the
viewport/examples/tests (stay superproject-side), the software rasterizer
interface target.

## Strategy: phase A (this session) — the package, zero churn

S1's lesson: the *package boundary* is the risky part; identifier churn is
mechanical and late-bound. So S2 phase A extracts the boundary WITHOUT
renaming identifiers:

1. **New module tree** `modules/core/` mirroring S1's shape:
   - `CMakeLists.txt`: `warploom_core` SHARED, PIE on, `Warploom::core`
     alias, `EXPORT_NAME core`, own `WarploomCoreConfig.cmake` +
     version file, `find_dependency(Threads)` in the template.
   - Sources move GIT-MV from `src/engine/core/*.cpp`; public headers GIT-MV
     from `include/engine/core/*` to `modules/core/include/warploom/core/*`.
   - Public header allowlist = all 22 headers (they are all consumed today).
2. **Compat layer**: `include/engine/core/` keeps a one-line forwarder per
   header (`#include <warploom/core/xxx.hpp>` + deprecation note). All 84
   existing include sites keep compiling UNCHANGED. `engine/` include root
   stays the superproject's integration root (S1 precedent).
3. **Namespace stays `omnicpp::core` / `omnicpp::editor` in phase A.**
   Rationale: the editor-widget collision means a namespace move is not a
   sed; it is a design act that belongs with S4 (when the widget layer
   moves and the `omnicpp::editor` namespace can finally be split
   truthfully). Renaming now would either break 72 files or mis-badge
   non-core code.
4. **Consumers**: `omnicpp_runtime` links `PUBLIC Warploom::core` (same
   pattern as `Warploom::ui`). Runtime source list loses the core/*.cpp
   entries; editor/render entries stay. The alias targets
   (`omnicpp_core` → alias of `Warploom::core`) update accordingly.
   Tests keep linking `omnicpp_runtime` (they get core transitively) and
   the runtime-only suite keeps proving headless linkability.
5. **Verification gate (phase A exit)**:
   - 4/4 suites on all four CI legs;
   - standalone scratch install + `find_package(WarploomCore CONFIG
     REQUIRED)` consumer binary runs (`CORE_CONSUMER_OK`);
   - 37/37 `tools/live_proof.py` assertions still pass;
   - the S1 standalone ui-consumer proof still passes (no regression in the
     module family).

## Strategy: phase B (later session) — identifier migration

Mechanical, gated by phase A's green matrix: `warploom::core` /
`warploom::editor` namespace adoption in core headers + compat
`namespace` aliases for the transition, wired to the S-decision pass
(`OMNICPP_*` → `WARPLOOM_*`, repo rename) so external adopters see one
coherent identity change, not two. The `omnicpp::editor` split (document
types vs widget types) is explicitly an S4 prerequisite and is called out
there.

## Risks / open questions

1. **SHARED + PIE + visibility**: mirroring S1's SHARED choice keeps the
   module family uniform; core has no RTTI-exports pitfalls observed (S1
   proved the pattern).
2. **Header-forwarder semantics**: forwarders must work for BOTH include
   spellings during transition (`engine/core/x.hpp` and
   `warploom/core/x.hpp`); installed package exposes ONLY
   `warploom/core/*` (allowlist enforces).
3. **`tests/CMakeLists.txt` include paths**: tests include
   `engine/core/...` via the superproject include root — forwarders keep
   this valid; no test churn in phase A.
4. **Benchmark/determinism sentinel**: the deterministic-runtime benchmark
   is the canary for "extraction changed behavior" — it must stay green and
   byte-stable.
5. **Library ordering**: `warploom_core` must not link `Warploom::ui`
   (verified: no ui includes in core). `omnicpp_runtime` links both.

## Sequencing within this session

1. git-mv files + write module CMake + config templates.
2. Forwarders + runtime link update + alias fixups.
3. Main leg green → remaining three legs.
4. Standalone core consumer proof + ui consumer re-proof.
5. Live proofs (37) on hardware.
6. Roadmap checkbox + docs update in the same commit.

---

# Phase B: identifier migration (S2-B gate)

Status: **gate doc section** — the contract below is the acceptance
authority for S2-B, appended per the design-first discipline. The phase-A
blocker (an `omnicpp::editor` collision across not-yet-split widget code)
dissolved with S4: every `omnicpp::editor` declaration now lives in a
module header, so the namespace move is mechanical.

## Contract

1. **Module core headers adopt the warploom namespace.** All 21 public
   headers and 10 TUs under `modules/core` re-badge their namespace
   blocks: `omnicpp::core` → `warploom::core`, `omnicpp::editor` →
   `warploom::editor`, `omnicpp::anim` → `warploom::anim`,
   `omnicpp::physics` → `warploom::physics`, `omnicpp::contract` →
   `warploom::contract`. File-scoped, mechanical openers + closers —
   exactly the S1 ui move, now for core.
2. **Compat aliases in every public header.** Each header ends with
   `namespace omnicpp { using core = warploom::core; using editor =
   warploom::editor; }` (+ the header's other re-badged namespaces).
   Guards: ODR-fenced (only emitted when the old spelling is not yet the
   alias subject — an alias-then-redeclare would be ill-formed);
   alias-in-namespace is not ambiguous with real child namespaces
   (determining a class takes precedence, then no finding in either
   scope = normal namespace lookup). The one known REAL declaration
   outside modules — `vulkan_parallel_recorder.hpp`'s
   `namespace omnicpp::core` (a struct tag, no named types) — coexists
   with the alias (a struct tag and an alias of a different name do not
   collide); its `omnicpp::render` half stays put (S3 scope).
3. **Consumers compile unchanged.** The ~1,050 qualified spellings
   outside modules/core (`omnicpp::core::X`, `omnicpp::editor::X`,
   `omnicpp::anim::X`) resolve through the aliases: 113 files across
   `modules/editor`, `src/`, `include/`, `tools/`, `examples/`, `tests/`.
   Include roots do NOT change — only namespace spelling moves.
4. **S-decision split.** Repo-wide external identity (`OMNICPP_*` flags,
   repo rename, binary names) stays OUT — it rides with S5 so adopters
   see one coherent identity change. This phase changes namespaces only.
5. **Verification gate (S2-B exit)**:
   - `warploom_core_tests` + `warploom_editor_tests` + full 6/6 suites on
     all four CI legs (aliases exercised by the very consumers that keep
     using the old spelling);
   - standalone consumers re-proofed against a fresh install
     (`CORE_CONSUMER_OK`, `UI_CONSUMER_OK`, `EDITOR_CONSUMER_OK` +
     `G3B_CONSUMER_OK` — the last two now exercise the alias path, since
     their sources still spell `omnicpp::editor`);
   - 64/64 live proofs on hardware (the headless host + proof harness
     are alias consumers);
   - the deterministic-runtime benchmark suite stays green and
     byte-stable (namespaces cannot move bytes, but the sentinel stays
     the sentinel).

## Mechanical plan (this session)

1. Scripted re-badge of the 31 namespace openers/closers across
   modules/core (openers AND comment closers), verified by grep before
   building.
2. Compat-alias footer appended to each public header (5 namespaces
   total, guarded).
3. Full four-leg verification + standalone consumer re-proofs + live
   proofs.
4. Roadmap checkbox + this addendum's status flip in the same commit.
