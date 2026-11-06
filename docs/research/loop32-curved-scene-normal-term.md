# Loop 32: curved-scene spike for the normal term

Status: MEASURED 2026-11-06 (RTX 2060). The loop30 open spike is closed:
the normal stop is exercised and live on curvature, with the gain
composition inverted vs the planar scene.

## Source

Same two claims as loop29/loop30, unchanged:

- Dammertz, Sewitz, Hanika, Lensch. "Edge-Avoiding A-Trous Wavelet
  Transform for fast Global Illumination Filtering." Proc. HPG 2010.
- Schied et al. "Spatiotemporal Variance-Guided Filtering." Proc. HPG
  2017, DOI 10.1145/3105762.3105770 (as recorded in loop30).

## Claim being borrowed

Same spatial filter, new scene: a tessellated sphere (r=0.5, 24x48
lat/long, 2208 tris) at the origin under the loop30 camera/bounces/sky,
so first-hit normals vary across every 5x5 tap and the normal weight
has something to read. Faceted soup is deliberate: the G-buffer stores
geometric face normals, which is exactly the signal the weight reads.

## Novelty assessment

None. Same textbook filter; the contribution is the measurement and the
scene-selection finding (below).

## What was measured

Same sweep grid as loop30 (k in {1,2} x ns in {0.05,0.1,0.2} x ds in
{0.1,0.3,1.0}, 4spp vs 128spp, luma MSE):

- raw 2.894761e-03, var(k=2) 1.479378e-03 (1.96x over raw).
- Best edge k=2.0/ns=0.2/ds=0.1: 1.435363e-03 = 0.970x var (1.03x),
  0.496x raw (2.02x). Pinned: < raw*0.70, < var*0.99 (measured 0.970,
  2-point margin, same style as loop30's 0.963 vs 0.99).
- Composition INVERTED vs planar: the ds sweep is flat (~0.05% -- small
  silhouette, interior-dominated) while the ns sweep moves 2%
  (ns=0.05 over-stops at 1.463669e-03 vs best 1.435363e-03). The normal
  weight is live here and was inert there; the depth stop is live there
  and is inert here. Pinned by an ns-slope guard (sweep index 9,
  k=2/ns=0.05/ds=0.1, must exceed best by >1%: measured 1.0197x) that a
  dead normal weight fails by construction.
- Wire guards both directions, same as loop30: uniform geometry ==
  variance-only to 4.66e-15; true geometry differs; re-dispatch
  bit-identical (pure function of inputs).
- Sabotage round-tripped: G-buffer store forced to all-miss ->
  edge == var EXACTLY (1.00x) and BOTH efficacy pins fail (0.99 pin and
  the ns-slope pin independently); revert restores bit-exactly.

## What it revealed

Scene selection matters, twice over:

1. A larger sphere (r=1.0, tried the same day) is a WORSE spike: raw
   3.28e-02, the frame is variance-dominated multi-bounce indirect and
   single-iteration buys 1.02x over var. The r=0.5 scene earns 1.03x
   over var / 2.02x over raw because the direct-sky shading dominates
   and the residual is edge-shaped. Recorded so the r=1.0 variant is
   not retried.
2. Neither scene shows a large normal-term gain (2% slope here, flat on
   planar). The single-iteration edge stop's total geometry gain is
   3-4% on both scenes, just composed differently. If a bigger normal
   gain exists, it is in multi-iteration WITH geometry guidance (still
   open -- only variance-only iteration is closed, loop29 s4) or in the
   per-iteration variance update, not in wider single-iteration ns
   sweeps: ns=0.05 already over-stops.

## What remains (in order)

1. Multi-iteration WITH geometry guidance (the loop29 s4 closure covered
   variance-only iteration, which diverges; the edge-stopped loop is
   untested).
2. The per-iteration variance update alternative (undecided, not started).
