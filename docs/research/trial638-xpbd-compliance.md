# Trial 638 research note: exact-XPBD compliance formulation

Status: ADOPTED (ADR-008). This note is the research-discipline record;
the verdict and numbers live in ADR-008.

## Source

- Muller, Macklin, Chentanez, Jeschke, Kim, "XPBD: Position-based
  simulation of compliant constrained dynamics" (MIG 2016): compliant
  constraints via `atilde = alpha/h^2` and per-constraint Lagrange
  multiplier accumulation, `dlambda = (-C - atilde*lambda)/(w + atilde)`.
- Muller et al., "Detailed Rigid Body Simulation with XPBD" (2020):
  the rigid-body contact treatment this adoption mirrors.
- Jolt Physics architecture + determinism contract (already surveyed in
  docs/research/physics-solver.md): island rebuild pattern, no warm-start
  caches (stored solver state breaks snapshot replay).

All three are peer-reviewed venue publications except Jolt (vendor/open
documentation, labelled as such). Nothing here is claimed as novel.

## The claim being borrowed

Compliant contacts with iteration-count- and dt-independent rest states:
accumulate the constraint impulse across a step's solver passes with the
dt-squared-scaled compliance, instead of shrinking each pass's correction
by a raw alpha.

## Novelty assessment

None. Standard XPBD contact/joint solve. What is codebase-specific: the
sign convention (lambda kept nonnegative as the accumulated *separating*
magnitude, so the atilde term enters with a minus), the unilateral
separation skip for contacts vs bilateral joint solve, and the decision
to leave ground as a stiff clamp. A combination of published technique
with local interface choices — a combination, not an invention.

## Why it fits here, and what it costs

- Fits: the engine already had the knob, the iteration knob, and the
  substep knob; the exact formulation makes the three compose instead of
  fighting (8-iteration gd scene + compliance now means something).
- Determinism: lambda is per-substep scratch, zero-initialised, insertion
  order — a pure function of the step's inputs. Default alpha = 0 is
  bit-identical to the historical solver, so no replay migration.
- Cost: two float vectors sized to contacts/joints per substep (reused
  scratch); one extra multiply-add per correction. No dependencies.
- Deliberately NOT adopted: warm-start lambda carry-over across steps
  (stored solver state breaks snapshot replay — same reason as before),
  compliant ground (no consumer), MGPBD (no deformables).

## What was measured

This codebase, deterministic C++, 2-stack under gravity, 1200 steps:

- Pre-adoption (incumbent, alpha = 1.0): rest pen 6.8e-4 at 1 iteration,
  3e-6 at 8 iterations (200x stiffer), 16x shift across 4x substeps.
- Post-adoption (alpha = 1e-4): pen 9.81e-4 at 1 AND 8 iterations
  (identical to 6dp), 9.79e-4 across 4x substeps. Prediction
  alpha*m*g = 9.81e-4 holds exactly.
- Suite: 625 unit + 15 core + 47 editor green; sabotage both directions
  fails the new guards (plus-sign lambda; raw alpha).
