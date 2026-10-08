# Research loop 2 — rigid-body rotation in PBD (the solver's biggest gap)

Status: notes, 2026-10-08. Our PhysicsBody has no rotation at all — position
only, no inertia tensor, no orientation. Every mainstream engine simulates 6
DOF. This loop researches whether PBD-family methods make rotation tractable
for a deterministic core.

## Sources

- Macklin (matthias-research.github.io, Ten Minute Physics tutorial 22, "How
  to write a basic rigid body simulator using position based dynamics") —
  the PBD author's own minimal rigid-body recipe.
- Tobin & Rucker, "Orientation in Extended Position-Based Dynamics:
  Application to Rigid Bodies and Cosserat Rods" (arXiv:2608.23606, 2026) —
  Lie-theoretic constraint gradients on the rotation manifold; reports >10^4
  improvement in dynamic consistency of constrained rigid-body XPBD over the
  prior state of the art.
- Abderezaei et al., "Physically Accurate Rigid-Body Dynamics in
  Particle-Based Simulation" (arXiv:2603.14634, 2026) — PBD-R: a
  momentum-conservation constraint + modified velocity update that makes PBD
  rigid bodies competitive with MuJoCo on a solver-agnostic analytical
  benchmark.
- Seabra, Lopes, Pereira, "Survey of Rigid Body Simulation with Extended
  Position Based Dynamics" (arXiv:2311.09327, 2023) — benchmarks PBRBD vs
  PhysX and Havok: PBRBD is stable and excels at energy-level maintenance but
  is limited at stable stacks of rigid bodies.

## Findings

1. Rotation in PBD is a solved, teachable problem (Müller's tutorial 22
   derives it in ten minutes): store orientation as a quaternion + inverse
   inertia tensor in body space; constraints apply angular corrections via
   Δω = I⁻¹(r × λn).
2. The 2026 state of the art improves the *dynamic consistency* of that
   formulation by 10^4 (Tobin/Rucker) by doing constraint math on the
   rotation manifold with Lie algebra instead of naive quaternion deltas.
3. PBD-R's momentum-conservation constraint addresses PBD's known physical
   accuracy gap (angular momentum drift) and is benchmark-validated against
   MuJoCo.
4. The honest limitation, from the survey: PBRBD "is limited in its handling
   of stable stacks of rigid bodies" — the exact feature our gd scene needs.

## Fit/cost for Warploom

Adding rotation means: quaternion + inverse inertia in PhysicsBody, angular
velocity in the snapshot (frozen like the linear state), torque accumulation,
and the correction/impulse math gains a rotational term. The frozen-snapshot
contract is preserved (snapshot the angular state identically). Replay
fingerprints change — the same declared-acceptable break as D1a.

Cost estimate: 2-3 focused sessions (body state, snapshot/impulse math,
contact torque from ground friction, tests with an analytic pendulum/tumbling
case). The Lie-theoretic formulation is the *correct* target but is second —
ship the naive quaternion version first, benchmark against tutorial 22.

## Spec decision (D1)

Adopted as a sequenced arc: (1) quaternion orientation + inverse inertia +
angular velocity in snapshot; (2) ground contact friction torque; (3)
sphere-sphere contact with rotational impulse; (4) then evaluate
Lie-formulation constraints from Tobin/Rucker for joints. PBD-R's
momentum-conservation constraint noted as the accuracy fix if drift appears.
