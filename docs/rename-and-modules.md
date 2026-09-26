# Engine rename + module split plan (S-track)

## Status

The repository still carries its scaffold name (`OmniCPP-template`) but is
now a deterministic simulation + node-based editor engine with a verified
vertical slice through every layer (render / simulate / edit / verify).
This document proposes the rename and the module boundaries to publish,
in dependency order, before any API freeze.

## Why split before publishing

Publishing the UI toolkit or editor modules separately freezes their API
surfaces. Splitting along the existing dependency seams first means each
published module has a *smaller* frozen surface, and the engine core can
evolve without dragging every consumer. The seams below already compile
cleanly (the include graph respects them); the work is packaging, not
refactoring.

## Name candidates

Requirements: pronounceable, unique in search, says "deterministic
simulation + editor", room to grow beyond rendering. Working candidates:

| Candidate | Rationale |
|---|---|
| **Verdant** | Growth/simulation connotation; short, clean; likely free in the package space |
| **Axisflow** | Node-graph dataflow + engineering precision; describes the editor substrate |
| **Loomline** | Weaving (graphs) + timeline (animation/sim); distinctive |
| **Kestrel** | Fast, precise, memorable; bird-of-prey metaphor for a low-latency engine |
| **Halide**-style coines (e.g. **Tessera**) | Mosaic tile = node modules composing one engine |

Recommendation: **pick from this list in a separate session with search
availability** (name collisions matter more than taste). The split plan
below is name-agnostic — modules are `engine-*` until the rename lands.

## Module split (dependency order)

Each module becomes a CMake package (add_subdirectory + find_package)
with its own tests and docs. Order = strictly increasing dependencies.

1. **`omnicpp-core`** (no deps)
   - content: `core/` minus editor pieces — Result, contracts, clocks,
     job system, ECS, physics, input, animation state machine, telemetry,
     script-module C ABI, node graph, document/commands
   - the determinism substrate; everything links this
2. **`omnicpp-ui`** (deps: core)
   - content: `ui/` + `editor/glyphs` — widget tree, layout, paint list,
     bitmap font
   - standalone immediate-mode-free retained UI; golden-image tested
3. **`omnicpp-render`** (deps: core)
   - content: `render/` — Vulkan context/swapchain/renderer, scene
     structures, render graph, RT pipeline, IBL, offscreen targets
4. **`omnicpp-editor`** (deps: core, ui)
   - content: `editor/` — session, control server/protocol, node editor
     view, inspector, graph-anim bridge, toolbars
   - the protocol surface for remote/AI driving lives here
5. **`omnicpp-engine`** (deps: all of the above; the aggregate)
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
2. Extract `omnicpp-ui` first (smallest, fewest dependents) as the pilot
3. Then core, render, editor in dependency order
4. Aggregate package last; viewport moves to `find_package`

## Non-goals

- No ABI stability promises until step 4 completes.
- No header shuffling beyond what the seams already dictate — the split
  is packaging, not refactoring.
