# Research index

Status: research notes produced by the 2026-10-08 research loop (10 domains,
sources verified via arXiv API / vendor docs / engine architecture pages /
frame-study articles). Per AGENTS.md each note carries source, claim, novelty
assessment, fit/cost, and what was adopted vs deliberately not.

## Notes

- [physics-solver.md](physics-solver.md) — XPBD + substepping (Small Steps,
  SIGGRAPH 2021; MGPBD, SIGGRAPH 2025; Orientation-in-XPBD, arXiv:2608.23606),
  speculative contacts, island sleeping, Jolt's determinism contract
  (record-rewind-replay check), Fiedler's fixed-timestep canon.
- [rendering-and-graphs.md](rendering-and-graphs.md) — Blade tracking-free
  RHI + global pass barriers (arXiv:2607.26506), idTech 6 frame structure
  (previous-frame reuse, persistent feature resources), real-time path
  tracing (ReSTIR lineage, RTXDI), frame-graph transient allocation.

## Adopted into the spec

| Finding | Spec change |
|---|---|
| Substepping > iteration for stiffness (Small Steps) | D1: compliance-scaled correction + velocity-from-position behind an opt-in that preserves old replays |
| Island sleeping via per-step rebuild | D1: union-find over the frozen contact graph, island-granular rest, wake-on-contact |
| Jolt's per-step determinism validator | D1: `WARPLOOM_PHYSICS_DETERMINISM_CHECK` debug flag (step twice, compare fingerprint) |
| Persistent feature resources, no lazy creation (idTech) | E2: compose chain pre-allocates ALL resources at init; toggles change pass recording, never rebuild |
| Dynamic rendering to un-bake passes (Blade direction, stage 1) | R5: fullscreen passes move to dynamic rendering before any GENERAL-layout work |
| Transient allocation from the graph (frame-graph canon) | R6: compile_graph owns resource declarations/lifetimes; ensure() derives |
| Stepped deterministic path tracing (replay machinery as the denoiser) | R1 spec rewrite: pause-accumulate-K-frames tonemap mode, ReSTIR deferred |

## Explicitly not adopted (with reasons)

- MGPBD multigrid solver — solves high-resolution deformable stiffness; we
  have no deformables.
- ReSTIR — reservoir stream state breaks the deterministic replay contract
  unless serialized into checkpoints; revisit when scene lights exceed ~64.
- Warm-started sequential impulse + contact caches — stored solver state
  conflicts with the snapshot-only replay contract at current scene sizes.
- Cross-platform float determinism build flag — no cross-platform product
  requirement yet; tests pin same-binary determinism.

## Loops 2–10 notes

- [loop2-rigid-rotation.md](loop2-rigid-rotation.md) — rotation in PBD
  (Müller tutorial 22; Tobin/Rucker Lie theory arXiv:2608.23606; PBD-R
  arXiv:2603.14634; PBRBD survey arXiv:2311.09327). Sequenced adoption arc.
- [loop3-joints-broadphase.md](loop3-joints-broadphase.md) — XPBD joints
  (Müller tutorial 25), Morton BVH / SAP / spatial-hash comparison. Distance
  joint adopted-after-rotation; Morton BVH recorded with a numeric trigger.
- [loop4-6-transient-determinism-pt.md](loop4-6-transient-determinism-pt.md)
  — transient allocation from the graph, the per-step determinism validator
  (Jolt's record-rewind-replay check), PT accumulation as a product feature.
  The cross-loop conclusion: pre-allocation beats create-when-enabled, and
  the determinism contract is a FILTER that picks techniques whose state is
  derivable from the snapshot.
- [loop7-10-scripting-ecs-determinism-pt.md](loop7-10-scripting-ecs-determinism-pt.md)
  — dataflow vs exec-wire scripting (our pull model is the replay-correct
  one), ECS scheduling (sparse-set fits; archetype only on measured need),
  -ffp-contract=off (ADOPTED — the most dangerous cross-build
  nondeterminism source), SVGF denoising (v1 PT ships pause-to-converge).

## Follow-up searches for the next loop

- Visual-scripting execution models (dataflow pull vs imperative push, and
  Unreal BluePrint's graph re-instance costs) for the G4 script node's
  editor wiring.
- Asset streaming/browser patterns (G5) — idTech 6's mega-texture feedback
  buffer (render pass reports which tiles are needed) is the closest shipped
  analogue to a browser that reflects runtime state.
- ECS scheduling — archetype (Bevy/Unity DOTS) vs sparse-set (EnTT) vs our
  bitmap masks, against the SystemScheduler's wave partitioning; the question
  to research is whether component-mask waves lose to archetype iteration at
  our entity counts.
- ReSTIR DI/GI with serialized reservoirs — revisit if the replay format
  gains a generic blob-payload section for solver state.
