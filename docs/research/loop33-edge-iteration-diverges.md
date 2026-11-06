# Loop 33: multi-iteration WITH geometry guidance still diverges

Status: MEASURED 2026-11-06 (RTX 2060). Negative result, and a sharp one:
edge stops do NOT save iteration. The multi-iteration branch is now
closed in both its forms (variance-only loop29 s4, edge-stopped here).

## Source

Same two claims as loop29/30/32 (Dammertz et al. HPG 2010; Schied et
al. HPG 2017, DOI 10.1145/3105762.3105770). This spike tests the obvious
rescue of the s4 verdict: the s4 divergence mechanism is stale guidance
(E2_orig against an increasingly smooth mean keeps the width wide)
plus a growing footprint (9x9 at K=2, 17x17 at K=3) smearing hard
edges. The G-buffer never goes stale (first-hit geometry does not
move), so edge stops should hold the true edges while the footprint
grows -- or so the hypothesis went.

## Claim being borrowed

Standard a-trous ping-pong with stride doubling (strides 1,2,4),
iteration 0 on live sums (divisor 4), later iterations on normalized
ping-pong (divisor 1), moment always on sums (divisor 4 -- iterations
>0 guide on E2_orig minus filtered_mean^2, the documented s4
approximation this measurement owns), edge weights at the loop30
planar best (ns=0.1/ds=1.0), k swept per K s4-style.

## Novelty assessment

None. The contribution is the closed branch: two measured verdicts
(variance-only diverges, edge-stopped diverges) from this codebase.

## What was measured

Box scene + measure identical to loop29 s4 / loop30 (64x64, 4spp vs
128spp, analytic sky -- K=1/k=2.0 reproduces the loop30 best
1.334328e-03 EXACTLY, which validates the chain harness end to end):

- K=1: k=0.5 -> 1.719080e-03, k=1.0 -> 1.615935e-03,
  k=2.0 -> 1.334328e-03 (the standing bar).
- K=2: k=0.5 -> 2.286227e-03 (0.58x of K=1, 1.01x raw), k=1.0 ->
  4.692186e-03 (0.30x), k=2.0 -> 1.133212e-02 (0.12x). Best is barely
  above raw; larger k hurts more.
- K=3: k=0.5 -> 4.545933e-03 (0.51x raw), k=1.0 -> 3.35e-02,
  k=2.0 -> 1.16e-01. Catastrophic at large k.

Pins (all structural -- the verdict is a measurement, not an
assertion): chain re-dispatch bit-identical (pure function of inputs);
K=2 != K=3 (iteration 2 is not a no-op); stride (1,2) != (1,1)
(doubling live); geometry-wire both legs K-matched -- K=1 uniform ==
variance-only to 4.65e-15, and K=2 uniform != K=2 true (binding 2
rebound every iteration). Sabotage-checked: hardcoded stride 1 fails
exactly the stride guard and only it; the memcmp instrument's
sensitivity is proven by the K-wire guard itself (different chains
compare unequal, so the purity pin's equality is meaningful). The
all-miss-store sabotage family is inherited from loop30/32 on the same
shader (collapses onto variance-only, fails the K=1 bars).

## What it revealed

1. The divergence is NOT an edge-leakage problem. K=2/k=2.0 edge is
   1.133e-02 vs s4 variance-only K=2/k=2.0 1.154e-02 -- nearly
   identical. At large k the stale variance width dominates so
   completely that perfect edge stops change nothing. The mechanism is
   the variance guide itself: E2_orig minus an increasingly smooth mean
   keeps every width wide while the footprint grows, and no spatial
   stop can fix a width that is wrong. Do NOT build the render-path
   filter loop on EITHER signal.
2. A guard-design trap worth recording: the first version of this test
   compared the K=2 uniform chain against K=1 variance-only and failed
   on its own guard (5.21e-3, correctly). A K=2 chain is a different
   algorithm than K=1 single-shot -- wire guards must be K-matched on
   both legs. The test now says so in its own comment.
3. Iteration is not automatically virtuous even where the single-shot
   filter earns: K=1 buys 1.73x over raw, K=2 keeps 1.01x, K=3 keeps
   0.51x. The remaining denoiser direction with a live hypothesis is
   the per-iteration variance update (recompute the guide from the
   current mean instead of E2_orig) -- undecided, not started.

## What remains (in order)

1. The per-iteration variance update alternative (undecided, not started).
2. The render-path loop question is settled by this verdict: no loop.
   Single-iteration stays the v1 filter (loop31 consumer stands).
