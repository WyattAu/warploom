# Warploom long-term roadmap: one multipurpose technical engine

Status: living plan. `docs/roadmap.md` remains the sprint-level record;
this file is the multi-year direction. It commits to scope and order, not
to dates. Every item inherits the project's verification discipline
(roadmap "Verification discipline", AGENTS.md "Verification baseline"):
four-leg matrix green, 0 validation diagnostics, 0 leaks, live hardware
proof for anything protocol- or viewport-facing, sabotage-checked guards,
`docs/rendering-status.md` updated in the same commit.

## 1. Goal, stated once

Warploom is a **multipurpose technical engine** for physics simulations,
games, movies, and CAD — no domain is second priority. The shared core
(determinism, data model, frame graph, physics solver, timeline) carries
all four; domain layers stay thin and move out of this repo when they need
a substantial consumer (AGENTS.md dogfooding rule).

Broad v1 means: **live path tracing with an in-tree denoiser, fuller
physics (rotation, joints, more shapes, sleeping), a real content
pipeline (glTF + compressed textures on GPU), and a usable editor
(script nodes, subgraph UI, asset browser, camera/light/render tracks) —
at performance budgets, not just correctness.**

Performance/scale is the binding constraint for the next 6–12 months:
frame cost, scene sizes, and GPU-driven paths gate what "broad" can mean.
Correctness work stays sabotage-checked and docs-honest; it does not pause
for perf, but perf rewrites must still prove equivalence (pixel A/B,
fingerprint stability, byte-identical replays).

## 2. Principles (non-negotiable, from experience)

- **Determinism is a product feature.** Anything reachable from a tick
  reads no wall-clock, iterates no unordered container, depends on no
  scheduling. A replay break is a regression even when every test passes.
  `-ffp-contract=off`; no `-march=native`/platform libm on tick paths.
- **Dependency budget: one package manager (CPM) + one library
  (GoogleTest).** OIDN and friends are excluded by this budget (see
  comparison-matrix §5); the denoiser is in-tree or it does not ship.
- **Delete is a feature.** Dead shims, superseded plans, draft docs go.
- **Scale the verification.** A check at one size proves one size
  (broadphase 1k-vs-4k lesson). A claim on one compiler proves one
  compiler (clang-zero vs GCC-203 lesson).
- **Never let a test pass by accident.** Sabotage both directions or the
  guard is decoration.
- **Technique needs provenance.** ACM/SIGGRAPH/IEEE > vendor docs >
  arXiv-as-preprint. Record source, claim, novelty, fit/cost, and what
  was measured here (docs/research/).
- **Mature-field rule.** Graphics, physics, and video are mature: survey
  Godot/Cycles/OCCT/Karma/Unreal/Unity (already started in
  docs/comparison-matrix.md + docs/research/) and WyattAu's own repos for
  domain patterns before inventing. Inspiration is expected; novelty
  claims must say what is standard, what combination is unusual, and what
  (if anything) is actually new.

## 3. Where we stand (measured, not claimed)

- **0.1 gate (roadmap):** clone-and-build met + CI-enforced; modules real;
  demo story (record/replay/scrub/save-load) met for render/document
  timeline; signature PT feature met as pause-to-converge (forward→pt→
  forward over socket, noise-floor convergence, flat linear accumulation,
  0.9994 capture correlation).
- **Recently landed:** shared-BLAS instancing (`mesh_index`, bit-identical
  proof), true Duff ONB + Faceforward normals with a committed
  normal-visualization orientation proof, clip strip in the viewport with
  live drag + undo/redo proof, KTX2 BC1/BC3 decode in-tree.
- **Known gaps:** PT is a static snapshot (app-scene geometry landed --
  mannequin/city/sponza traced, actors frozen mid-stride, skinned parts in
  bind pose placed by model*bone; no live/deforming PT); no bloom on PT;
  no denoiser (deliberate v1 deferral, decision recorded);
  no adaptive sampling; transient planner (R6a) unwired — R6b unblocked
  by Loop 13 graph-derived lifetimes but still to wire; physics has no
  rotation/joints, capsule/sleeping/velocity-update partially open;
  scrub restores document, not physics; sub-frame fraction not yet in the
  protocol; no hardware CI runner (RT tests skip on lavapipe); WASM/
  Wayland/adoption (E6) open.

## 4. Track plan (parallel tracks, shared substrate first)

Tracks run in parallel but merge in order: **T0 → T1/T2 → T3–T7 → T8.**
Nothing in T3–T7 may break T0's contracts (determinism, verification).

### T0. Substrate + performance budgets (first, unlocks everything)

- [ ] **R6b: allocate/alias Vulkan images from the plan.** Loop 13
  derived lifetimes from the recorded graph; now create/alias/destroy
  from it. Exit: compose hand-written target list deleted, 4-of-10
  elision holds on-device, history never aliased (fuzz + Lean model
  updated), 0 diagnostics, pixel A/B clean.
- [ ] **Scene pass as a graph node (B2 seam).** Retire the hand-written
  UNDEFINED→COLOR_ATTACHMENT_OPTIMAL barrier + `initial_layout` seeding;
  one graph from scene through compose. Own verification run (touches
  pass framing).
- [ ] **Dynamic rendering for fullscreen passes (Blade stage 1)** so
  passes un-bake; prerequisite for GENERAL-layout PT/bloom layering work.
- [ ] **Perf budgets as gates.** Per-config frame budgets (default,
  bloom, RT, gd, node editor, pt) from real GPU timing (O2 now resolves);
  `scripts/analyze_telemetry.py` extended, never bypassed. Scale
  benchmarks: 10k-instance gd path, 4k/8k physics scenes. A perf
  assertion is a canary, not a gate, on shared/loady machines — record
  load with the number.
- [ ] **Async-compute evaluation.** Queue family is discovered, never
  used. Spike: H-Z reduce and/or bloom downsample off the graphics
  queue; adopt only on measured win with determinism intact.

### T1. Raster core (one render path, then scale)

- [ ] Occlusion-heavy demo content (engine needs no change; content
  question) to show live savings.
- [ ] LOD mesh **generation** (selection exists; nothing generates).
- [ ] CSM evaluation only when a scene needs it; single 2048² map until
  then. SSAO/SSR/TAA stay out unless a consumer proves need.

### T2. Ray tracing / path tracing (usability → live)

- [x] **App-scene PT geometry.** DONE: setup_path_tracing snapshots the
  live scene list (same matching as the RT shadow TLAS walk: cube/ground by
  mesh, mannequin parts via model*dominant-joint bone, city/sponza identity
  over world-baked triangles) instead of the hardcoded cube pair. Startup
  builds through the control-path request on the first frame with a scene
  (the list is per-frame; an eager build snapshots emptiness). Proven by
  `app_scene_general_transforms_contribute` (extra mesh through a general
  transform contributes + deterministic) and live on the RTX 2060:
  mannequin scene 10 objects/11 meshes and city 76/60, 0 VUIDs, captures
  with 0% unwritten converging monotonically. Honest limits: static
  snapshot (animation smears an accumulator), skinned parts are rigid
  placements not vertex skinning, bloom does not layer on PT. Exposed two
  engine bugs, both fixed (see R6b note in roadmap.md): the verify gate's
  absolute-index subset check refused PT-with-bloom-off, and the per-frame
  external descriptor write was VUID-03047 with frames in flight.
- [ ] **RT sub-toggles (R2) + GPU-driven skinned scenes (R3).**
- [ ] **PT + bloom layering** (source already divided by n; design the
  combine, then pin by hash).
- [ ] **Live PT: adaptive sampling** (Cycles 0.01 / Karma Pixel Oracle
  pattern; fixed-budget win) then **in-tree à-trous/SVGF over the
  existing G-buffer** with the recorded constraint: temporal-history
  validation must be a pure function of accumulation state or it breaks
  deterministic stepping. OIDN stays excluded by the dependency budget.
- [ ] Path guiding / Light Tree / MBVH only on measured scene-size need.

### T3. Physics (depth without breaking replay)

- Order: velocity-from-position update → island sleeping (union-find over
  frozen contact graph) → capsule → **rotation** → **joints**, each a
  design arc (see docs/research/physics-solver.md + loops 2/3).
- Keep: frozen-snapshot detection (bit-identical pair sets), substeps +
  iterations as separate knobs, compliance opt-in (default 0 =
  bit-identical replays), determinism double-step validator.
- Broadphase trigger: Morton BVH only on the recorded numeric trigger;
  grid + all-pairs fallback until then.
- Perf: 4k/8k agreement + speedup pinned (today 2.10x/2.89x/2.92x at
  1k/4k/8k for the grid solver).

### T4. Data model + simulation

- ECS stays sparse-set until archetypes earn it on measurement.
- **Scrub restores physics** (checkpoints capture document only today).
- **Sub-frame fraction in the protocol** (D2b parameter exists, hosts
  pass 0) so smooth playback is replayable; then concurrent
  record-and-play.

### T5. Movies / timeline

- Camera/light/render-property tracks; concurrent record + play arms.
- Clip-strip chrome as needed (context menu, zoom/pan UI — window is
  host-set today); block-move already live-proven.
- Capture pipeline follows PT usability (already 0.9994 correlation).

### T6. Games / editor

- G4 script-node UI rows + viewport wiring (.so path already proven
  through the graph); G2 subgraph UI wiring (engine half done with
  rollback); G5 asset browser; scene management.
- Input already in-tree; no new backends without a consumer.

### T7. CAD / modelling

- OCCT lesson first: **face holds a list of triangulations + one
  active** (render vs analysis resolution). Apply to our one-mesh-per-
  object model before any kernel talk.
- Parametric bridge + LOD/decimation generation (shared with T1).
- Full BRep kernel: spike only, out-of-tree on first need (dogfooding
  rule). No silent "CAD support" claims from a mesher.

### T8. Productization (proves the engine from outside)

- [ ] **E3 hardware CI runner** — RT tests currently skip on lavapipe;
  the best subsystem has the least protection.
- [ ] E6: WASM leg, native Wayland, FetchContent/vcpkg adoption, docs
  rebrand, repo rename.
- [ ] Dogfooding splits per AGENTS.md: viewport split as the natural
  first; then a game consumer (WyattAu/warploom-game pattern exists),
  and a movie/CAD consumer only when the capability needs one. Each:
  separate repo + CI, public API only, its existence is the evidence.

## 5. v1 definition (broad, testable)

v1 ships when **all** hold, each with its machine-checked proof:

1. Pause-to-converge **and** live PT with in-tree denoiser + adaptive
   sampling, on app scenes (not cubes-only), at published frame budgets.
2. Deterministic tick + replay + scrub **including physics**, sub-frame
   playback reproducible from the protocol.
3. Timeline: interpolated/eased tracks incl. camera/light/render,
   concurrent arms where claimed, clip strip fully usable.
4. Physics: spheres + capsules, rotation + joints, sleeping, broadphase
   at scale, validator clean.
5. Content pipeline: glTF + KTX2 (GPU-native upload on measured VRAM
   trigger) + asset browser.
6. One render path: scene-through-compose on the graph, transient
   aliasing on-device, 0 diagnostics/leaks, full preset matrix green.
7. Hardware CI runner so RT is regression-protected; packaging/adoption
   (E6) done enough for an external consumer to build on it.

Explicitly **out** for v1: multiplayer/rollback, audio backends,
mobile, ABI stability, MBVH/path-guiding/Light Tree (until scene sizes
demand), OIDN (budget-excluded), BRep kernel (spike only).

## 6. Inspiration queue (survey before inventing)

- Rendering/graphs: Unreal RDG transient allocator, Unity RenderGraph
  descriptor-reuse (know what we beat), Blade tracking-free RHI, idTech
  persistent resources, Cycles/Karma sampling + Pixel Oracle, Lumen
  radiance-cache (why no-denoiser is a defensible architecture).
- Physics: Jolt island rebuild + determinism checkbox, Box2D v3
  speculative contacts, Rapier/Godot determinism claims, XPBD/Small
  Steps/MGPBD lineage in docs/research/physics-solver.md.
- CAD: OCCT triangulation lists + `Poly_MeshPurpose`, Nanite cluster
  LOD (know the shape before claiming any of it).
- WyattAu repos: mine domain patterns (gameplay, tooling, asset
  workflows) for T5–T7 before designing new UI/API. Proposals that skip
  this survey carry the burden of proof.

## 7. Execution order (next 4 commits, smallest first)

1. T0 budgets: publish per-config frame budgets + scale-benchmark
   baselines (no code change to hot paths; gates extended).
2. T0 seam: scene pass as graph node (one verification run).
3. R6b wiring (create/alias/destroy from the plan).
4. T2 app-scene PT geometry (first non-cubes scene through the tracer).

Then re-evaluate per the loop: what unblocked what, what it revealed.
