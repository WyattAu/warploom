# Engine rename + module split plan (S-track)

## Status

The repository was scaffolded as `OmniCPP-template` and has been renamed
**Warploom** (CMake project `Warploom`, vcpkg manifest `warploom`); it is
a deterministic simulation + node-based editor engine with a verified
vertical slice through every layer (render / simulate / edit / verify).
This document proposes the rename and the module boundaries to publish,
in dependency order, before any API freeze.

**Decision (2026-09-26): the engine is named Warploom.** The root CMake
project is `Warploom`; the vcpkg manifest is `warploom`. Internal targets
(`omnicpp_runtime`), binary names, `OMNICPP_*` env/option flags, and the
`engine/` include root are unchanged to keep churn low; migrating them is
a follow-up with its own verification pass.

## Why split before publishing

Publishing the UI toolkit or editor modules separately freezes their API
surfaces. Splitting along the existing dependency seams first means each
published module has a *smaller* frozen surface, and the engine core can
evolve without dragging every consumer. The seams below already compile
cleanly (the include graph respects them); the work is packaging, not
refactoring.

## Name decision: Warploom

Requirements: pronounceable, unique in search, says "deterministic
simulation + editor", room to grow beyond rendering. Eleven candidates
were checked via web search, then the finalists against the live
registries (crates.io / npm / PyPI APIs, GitHub org API, DNS). Evidence:

| Candidate | Result |
|---|---|
| **Warploom** | **CHOSEN.** Zero software hits in search (page one is 1940s textile ads); free on crates.io, npm, and PyPI; `warploom.com` unregistered; only blemish is a dormant, empty GitHub `WarpLoom` org (use a suffixed org name if desired). Loom = weaving node graphs into behavior; warp = weaving warp *and* controllable time (fixed-step, replayable determinism). |
| Escapement | Rejected — taken on all three registries; crates.io `escapement` is a deterministic virtual clock crate (direct conceptual collision, Jul 2026). |
| Axisflow | Rejected — GitHub org taken; AxisFlow FreeCAD add-on is active (2026 releases). Best of the original five; second choice. |
| Weftline, Loomwright, Loomforge, Warpforge, Heddle, Tactus, Selvage, Warpwright, Jacquard | Rejected — each has active software users in search. |
| Verdant, Loomline, Tessera, Kestrel (original shortlist) | Rejected — all taken; Tessera and Kestrel heavily (UI framework / ASP.NET Core server). |

## Module split (dependency order)

Each module becomes a CMake package (add_subdirectory + find_package)
with its own tests and docs. Order = strictly increasing dependencies.

1. **`warploom-core`** (no deps)
   - content: `core/` minus editor pieces — Result, contracts, clocks,
     job system, ECS, physics, input, animation state machine, telemetry,
     script-module C ABI, node graph, document/commands
   - the determinism substrate; everything links this
2. **`warploom-ui`** (deps: core)
   - content: `ui/` + `editor/glyphs` — widget tree, layout, paint list,
     bitmap font
   - standalone immediate-mode-free retained UI; golden-image tested
3. **`warploom-render`** (deps: core)
   - content: `render/` — Vulkan context/swapchain/renderer, scene
     structures, render graph, RT pipeline, IBL, offscreen targets
4. **`warploom-editor`** (deps: core, ui)
   - content: `editor/` — session, control server/protocol, node editor
     view, inspector, graph-anim bridge, toolbars
   - the protocol surface for remote/AI driving lives here
5. **`warploom-engine`** (deps: all of the above; the aggregate)
   - what the viewport and tests consume today; keeps one-liner adoption

## Per-module publish criteria

- CMake package config (`*Config.cmake`) + version file
- Own test target; CI leg per module (the existing matrix generalizes)
- README with scope + non-goals
- API stability markers on everything exported (enforce with a header
  allowlist per module)

## Sequencing

1. Land the rename (repo + CMake project name + include roots stay
   `engine/` internally to keep churn low)
2. Extract `warploom-ui` first (smallest, fewest dependents) as the pilot
   — **DONE**: lives in `modules/ui` as package `WarploomUI`, exported
   target `Warploom::ui`, namespace `warploom::ui`; standalone
   find_package consumption proven.
3. Then core — **phase A DONE**: lives in `modules/core` as package
   `WarploomCore`, exported target `Warploom::core`, 22-header allowlist,
   compat forwarders keep `include/engine/core/*` valid (zero churn at
   84 include sites); namespaces stay `omnicpp::*` until phase B (the
   identifier migration, wired to the S-decision pass — the
   `omnicpp::editor` namespace spans non-core widget code, so the
   namespace split waits for S4; see `docs/warploom-core-plan.md`).
   Then render, editor in dependency order
4. Aggregate package last; viewport moves to `find_package`

## Non-goals

- No ABI stability promises until step 4 completes.
- No header shuffling beyond what the seams already dictate — the split
  is packaging, not refactoring.
