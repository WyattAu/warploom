# Research loop 3 — joints/constraints and broadphase alternatives

Status: notes, 2026-10-08.

## Joints via XPBD

**Source.** Macklin, Ten Minute Physics tutorial 25, "Joint simulation made
simple" (matthias-research.github.io/pages/tenMinutePhysics/25-joints.pdf):
"simulate almost any mechanical system stably and accurately" with XPBD —
generic constraints C(x) with compliance α̃ = 1/(k dt²), solved as
Δλ = -C/(∇C·M⁻¹∇Cᵀ + α̃) per iteration, joint bodies corrected proportional
to inverse mass, angular parts via the rotation machinery from loop 2.

**Claim.** One generic constraint primitive (projection + compliance) covers
distance, ball-and-socket, hinge, fixed joints — rather than a bespoke solver
per joint type.

**Novelty.** Standard (XPBD, MIG 2016).

**Fit/cost.** High. Our solver's correction loop is already a projection
(Δx along normal ∝ inverse mass); a distance joint is the same math with an
arbitrary pair of attachment points. The snapshot contract extends: joints
join the frozen candidate set (their rest error is a pure function of the
snapshot too). Deterministic: fixed joint insertion order (Jolt's advice —
"ConstraintSettings::mConstraintPriority is unique per constraint so that it
defines a consistent ordering"). Sequenced after rotation (loop 2), because
ball/hinge joints need angular state.

## Broadphase alternatives (Morton BVH vs our uniform grid)

**Source.** Macklin, tutorial 24 ("Bounding Volume Hierarchies with a
blazing fast implementation" — Morton codes to BVH) and 23 (Sweep and
Prune); tutorial 11 (spatial hashing — the method our grid already uses);
Jolt architecture docs (incremental AABB-tree refit, background rebuild,
widening-until-next-step).

**Findings.**
- Morton-code BVH: O(n log n) build, cache-friendly linear codes, good for
  DEFORMING dynamic bodies because the tree refits by construction.
- SAP: excellent for mostly-static scenes, degrades with dense overlap.
- Our uniform spatial hash: O(1) insertion, cell-size tuned per scene —
  measured 2.10-2.92x over all-pairs, exactly conservative via the frozen
  snapshot.
- Jolt's design: tree refit in place + background rebuild swap, widening
  boxes until the rebuild.

**Assessment.** Our grid is the right primitive at current scales; the
documented failure mode (overflow falls back to all-pairs rather than
dropping pairs) already bounds the worst case. A Morton BVH becomes worth it
when bodies exceed grid-cell density or move with high spatial coherence.
NOT adopted now; recorded as the growth path with tutorial 24 as the
implementation reference. One concrete adoption from Jolt: AABB *widening*
until the next rebuild — our grid recomputes cell membership per step from
positions, which is already conservative, so this only applies if we move to
a tree.

## Spec decision (D3/D1)

Distance joint via XPBD = first joint type, after rotation lands. Morton BVH
recorded as the broadphase growth path with a numeric trigger (bodies > 8k or
cells-per-body > threshold).
