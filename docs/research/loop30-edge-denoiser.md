# Loop 30: edge-stopped a-trous on the G-buffer signal

Status: SPIKE MEASURED 2026-11-06 (RTX 2060). No render-path consumer yet --
the filter lives in `denoise_edge_gbuffer_spike` (headless dispatch over
live tracer outputs), like the variance-only spike before it.

## Source

- Dammertz, Sewtz, Hanika, Lensch. "Edge-Avoiding A-Trous Wavelet Transform
  for fast Global Illumination Filtering." Proc. High Performance Graphics
  (HPG) 2010, pp. 67-75 (3rd best paper). The edge-stopping combination
  (spatial x geometry weights) is theirs; venue confirmed via the HPG
  best-paper list + Ulm preprint.
- Schied, Kaplanyan, Wyman, Patney, Chaitanya, Burgess, Liu, Dachsbacher,
  Lefohn, Salvi. "Spatiotemporal Variance-Guided Filtering: Real-Time
  Reconstruction for Path-Traced Global Illumination." Proc. HPG 2017,
  DOI 10.1145/3105762.3105770 (best paper). The variance-driven width
  (edge = k*sqrt(v_center)) is theirs. Our temporal term is absent (single
  frame, no history) -- this is the spatial half of SVGF only.

## Claim being borrowed

A single-iteration a-trous tap weight of
w = spatial x exp(-dluma^2/2edge^2) x normal-stop x depth-stop preserves
geometric discontinuities while smoothing interiors, beating the
variance-only weight on edge-dominated residuals.

## Novelty assessment

None. Standard edge-avoiding a-trous with an SVGF-style variance width --
the textbook pairing. The contribution is the measurement on this
codebase/scene, not the filter.

## Why it fits here, and what it costs

The G-buffer landed in the previous step (first-hit normal+depth,
overwritten per round), so the edge signal exists with no new tracer work.
Cost over the variance-only spike: one extra storage-image binding, two
exp() per tap, same 5x5 stride-1 footprint. Determinism contract unchanged
(pure function of (mean, moment, gbuffer); bit-identical re-dispatch
pinned).

## What was measured

Same scene + measure as loop29 s3 (64x64 box, analytic sky, 4spp vs
128spp reference; raw 2.313658e-03 reproduced exactly, variance-only
k=2.0 1.385478e-03 reproduced exactly -- same inputs, same numbers):

- Sweep k in {1.0, 2.0} x ns in {0.05, 0.1, 0.2} x ds in {0.1, 0.3, 1.0}:
  best k=2.0/ds=1.0 at 1.334328e-03 = 1.04x over variance-only, 1.73x
  over raw. Pin: best < var*0.99 (measured 0.963) + best < raw*0.70
  (measured 0.577).
- The ns sweep is FLAT (identical MSEs across ns): planar box faces mean
  in-tap normals never differ, so the normal stop is inert here -- the 4%
  comes from the depth stop at silhouettes. A curved scene is needed to
  exercise the normal term (open).
- Geometry-wire guards, both directions: uniform geometry (+Z, t=1)
  reproduces variance-only to 4.65e-15 MSE (geometry-blind == no new
  signal), and true geometry differs from uniform (wire is not
  decorative). Sabotage-checked live: deleting the rgen G-buffer store
  collapses edge onto variance-only (1.00x) and fails exactly the two
  geometry pins; revert restores 1.334328e-03 bit-exactly (round-trip
  confirmed).
- 0 warnings, 0 validation diagnostics; full suite 740/740, ctest 6/6.

## What it revealed / next

The single-iteration edge stop earns a small but real 4% on top of the
variance-only 1.67x, and the mechanism is identified (depth at
silhouettes, not normals). Two branches open, in order:

1. Render-path consumer: the filter as a compose-stage (or PT-blit
   post-pass) operator on app scenes at published budgets -- the step
   that turns two spikes into the v1 denoiser.
2. Curved-scene spike to exercise the normal term (currently unproven),
   and only then multi-iteration WITH geometry guidance (loop29 s4 closed
   the variance-only loop, not an edge-stopped one).
