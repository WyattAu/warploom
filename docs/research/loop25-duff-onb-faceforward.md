# Loop 25 — true Duff ONB + Faceforward hit normals (path-tracer correctness)

Technique, not plumbing: two textbook fixes to `pt_real` that the previous
"proofs" had been papering over. Per the research discipline: source, claim,
novelty, fit, cost, and what was measured.

## Sources

- Duff, Burgess, Christensen, Hery, Kensler, Liani, Nederhouser,
  "Building an Orthonormal Basis, Revisited", Journal of Computer Graphics
  Techniques 6(1):1–8, March 2017. Stable URL:
  https://jcgt.org/published/0006/01/01/ (peer-reviewed journal).
- Pharr, Jakob, Humphreys, "Physically Based Rendering", 4th ed., MIT Press
  2023 — the Faceforward rule (shading normal faces the incident ray).
  Standard practice, cited as the textbook rather than as evidence.
- Möller–Trumbore ray-triangle test (J. Graphics Tools 2(1), 1997,
  DOI 10.1080/10867651.1997.10487468) — the orientation both the rchit and
  the fp64 reference share.

## The claims being borrowed

1. Duff's branchless basis (`sign = copysign(1,n.z)`, `a = -1/(sign+n.z)`,
   `b1 = (1+sign·nx²·a, sign·b, −sign·nx)`, `b2 = (b, sign+ny²·a, −ny)`) is
   orthonormal for every unit input, with the denominator provably in
   [1,2]/[−2,−1) — never zero.
2. A closest-hit shading normal must face the incident ray
   (`if dot(n, rayDir) > 0: n = −n`), regardless of winding.

## Novelty assessment

None. Both are standard. The old rgen comment *claimed* Duff but implemented
a naive projection — the novelty here is nil, the correction is the work.

## Why it fits here, and what it costs

- The rgen samples a cosine-weighted hemisphere around the hit normal every
  bounce. A degenerate basis collapses the lobe; a wrong-sided normal aims
  it into the solid. Either silently corrupts every accumulated pixel.
- Cost: ~10 ALU in the raygen (no extra normalizes — Duff vectors are unit
  by construction), one dot+select in the rchit. No new dependencies,
  buffers, or pipeline changes. Determinism unaffected (pure function of
  pixel, frame, and hit).

## What was measured (RTX 2060, this codebase)

- Naive basis degenerates exactly for n = ±Z (zero vector, normalized):
  `n=(0,0,±1) → ax=(0,0,0), len=0`. True Duff verified orthonormal
  (|b1|=|b2|=1, all dots 0) for ±Z, ±Y, and a tilted normal (fp32 harness).
- With the fix, wall probes move 0.8000 (degenerate fingerprint: exactly
  albedo, zero variance) to 0.585/0.579 vs fp64 references 0.612/0.562;
  slab moves 0.3868 (through-solid transport, see below) to 0.6414 vs
  0.6073. `path_tracing_real.*` 6/6, full default suite green.
- Normal orientation, measured directly (normal-as-color through the
  accumulation path): slab top (0.4,0.8,0.4) → n=+Y outward, wall −z
  (0.4,0.4,0.8) → n=+Z outward. Before the flip the same instrument read
  (0.4,0.0,0.4)/(0.4,0.4,0.0) — inward on both faces. The winding claim the
  old comment made (cross points outward) is therefore retracted: for this
  repo's box winding the raw cross points inward, measured on top (+Y face
  reports −Y) and bottom faces.
- Honest residual (recorded, not hidden): negating the Faceforward rule
  (normals facing away) yields slab 0.54 / walls 0.64 — inside the 0.40 MC
  tolerance — via through-solid transport (two 0.8 bounces out the far
  side: 0.8² = 0.64). The committed guards catch trapping (exactly 0 via
  `> 0.15`) and lobe collapse (exactly 0.8 via `< 0.79`) but cannot separate
  face-toward from face-away numerically in this all-grey scene; orientation
  rests on the normal-encoding measurement above plus the analytic anchor
  (CPU room model uses independent outward AABB normals and agrees at
  0.64/0.59). A two-tone scene (per-instance albedo the rgen actually
  reads) would make the leak observable — future work, not this step.
