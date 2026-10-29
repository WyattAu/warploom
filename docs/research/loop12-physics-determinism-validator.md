# Research loop 12 — physics determinism validator + XPBD compliance (Loop 1 of 10)

Status: executed 2026-10-26. Competitor-first per user direction.

## Problem in the spec

`docs/research/README.md` and `docs/roadmap.md` D1 list three items as
ADOPTED:

1. contact-compliance opt-in (XPBD alpha) + velocity-from-position,
2. island sleeping via per-step union-find rebuild,
3. `WARPLOOM_PHYSICS_DETERMINISM_CHECK` double-step validator.

`code_search` for `DETERMINISM_CHECK|contact_compliance|island|sleeping`
returns ZERO hits in `modules/physics/` — only the docs mention them.
The spec claims adoption; the code has none of the three. This loop
closes item 3 (validator) and stages item 1 (compliance knob) as the
spec-correct next step, with papers + competitor evidence.

## Papers pulled

- P1. Macklin, Müller, Chentanez, "XPBD: position-based simulation of
  compliant constrained dynamics" (MIG 2016). Claim borrowed: compliance
  `α̃ = 1/(k·dt²)` in `Δλ = -C/(∇C·M⁻¹∇Cᵀ + α̃)`; stiffness-∞ is
  compliance 0. Our solver is the α̃=0 special case.
- P2. Fang et al., "Small Steps in Physics Simulation" (SIGGRAPH 2021,
  https://mmacklin.com/smallsteps.pdf). Claim: small dt with few
  iterations beats large dt with many for stiffness; 30 substeps × 1
  iteration at 13.5ms shows far less stretch. Our `set_substeps` +
  `set_solver_iterations` knobs are this paper's scheduling layer.
- P3. Li et al., "MGPBD: A Multigrid Accelerated Global XPBD Solver"
  (SIGGRAPH 2025, DOI 10.1145/3721238.3730720). Claim: unsmoothed-aggregation
  AMG + PCG removes XPBD stiffness ceiling for high-resolution deformables.
  NOT adopted: we have no deformables (same reason as loop-11 notes).
- P4. Tobin & Rucker, "Orientation in Extended Position-Based Dynamics"
  (arXiv:2608.23606, 2026). Claim: Lie-theoretic constraint gradients on
  rotation manifold give >10⁴ dynamic-consistency gain for constrained
  rigid-body XPBD. Relevant to rotation arc (loop 2), not this loop.
- P5. Abderezaei et al., "PBD-R" (arXiv:2603.14634, 2026). Claim:
  momentum-conservation constraint makes PBD rigid bodies competitive with
  MuJoCo. Noted as accuracy fix if drift appears; not this loop.
- P6. Fiedler, "Fix Your Timestep" (gafferongames.com, 2004). Canon we
  already follow (fixed dt + accumulator); no change.

## Competitor repos (source-read, not docs-only)

- Jolt Physics (jrouwe/JoltPhysics, used by Horizon Forbidden West):
  architecture doc confirms island-granular sleeping with islands REBUILT
  EVERY STEP, O(N), lock-free; samples have "Check Determinism" =
  record state via StateRecorder, rewind, step again, compare — the exact
  double-step validator this loop implements. Determinism is opt-in build
  (`CROSS_PLATFORM_DETERMINISTIC`, ~8% slower); we claim same-binary only.
- Box2D v3 (Catto): speculative contacts solve TOI-TOI pairs, eliminating
  tunneling without CCD. Our solver has no speculative margin; loop-1 keeps
  this as roadmap (needs tunneling stress test as acceptance gate).
- Godot 4.6: Jolt is now the migration path (docs.godotengine.org,
  strayspark.studio 2026-03-31 migration guide); Godot-physics itself makes
  no determinism claim. Rapier-for-Godot plugin documents same-initial-
  conditions determinism explicitly. Our contract (frozen snapshot + fixed
  pair order + fingerprint) is stronger than Godot core, on par with Rapier
  plugin claim.
- PositionBasedDynamics lib (InteractiveComputerGraphics): reference XPBD
  implementation; confirms compliance formulation is standard adoption
  engineering, not research.

## Best-solution analysis

Three candidates, one winner for this loop:

- (a) Double-step validator: 2x physics cost in debug only, zero release
  cost, catches every determinism regression the day it lands. Jolt ships
  exactly this as a sample checkbox. Risk: nil (off by default, no replay
  change). ADOPTED + IMPLEMENTED this loop.
- (b) Compliance opt-in: correct XPBD staging (default 0 = historical stiff
  projection, so existing replays stay valid). Risk: low, but changes
  correction math → needs fingerprint-bump tests. STAGED: knob + tests this
  loop, default-preserving.
- (c) Island sleeping: highest perf value (settled 400-body pile → near-zero
  step cost) but touches step scheduling + wake-on-contact + rest criteria.
  DEFERRED to Loop 2 of 10 with its own verification run (pile-settles test).

Nobody else solves (a) better: Jolt's is sample-only, not a CI gate; ours
will be a unit-test-pinned debug flag, which is stronger.

## Novelty assessment

Nothing novel. Validator = Jolt's sample pattern as a test-pinned API.
Compliance knob = XPBD MIG 2016 special-case (α̃=0 is our status quo).
Combination (validator guarding compliance opt-in) is a combination, not
an invention.

## Fit/cost

- Validator: `set_determinism_check(bool)` + `step()` snapshots fingerprint,
  re-runs substeps from saved state, asserts equal. Deterministic by
  construction (same input → same output); failure = abort in debug via
  assert, test-pinned via `EXPECT_DEATH`-free design (returns bool count).
  Actually implemented as: `step_with_check()` returning bool + counter,
  so tests pin it without death-tests; `step()` unchanged in release.
- Compliance: `set_contact_compliance(float alpha)` default 0; correction
  `corr = penetration / (inv_sum + alpha)` scaled per XPBD. Default 0
  reproduces historical output bit-identically (test-pinned).

## What was measured

See `tests/unit/test_physics_world.cpp` additions (Loop12*): validator
passes on settled pile, compliance-0 bit-identical fingerprint, compliance>0
softens tower sink vs stiff. Numbers from this codebase, not the papers.
