# Loop 34: per-iteration variance update still diverges

Status: MEASURED 2026-11-06 (RTX 2060). Negative, and the branch is now
exhausted: the variance update is the last open denoiser direction from
loop29/30/32/33, and it does not save iteration. Single-iteration stays
the v1 filter; there is no remaining filter-loop hypothesis on the table.

## Source

Same pair as loop29/30/32/33 (Dammertz et al. HPG 2010; Schied et al.
HPG 2017, DOI 10.1145/3105762.3105770). This spike tests the mechanism
the loop33 verdict indicts: iterations >0 guide on E2_orig minus an
increasingly smooth mean, so every width stays wide while the footprint
grows (K=2/k=2 edge == variance-only to 2%). The rescue is to recompute
the guide per iteration: smooth E2 with the same spatial x geometry
kernel, then run iteration 1 on v = E2smooth - M1^2.

## Claim being borrowed

Standard practice, not a paper claim: filter the moment alongside the
mean and recompute the variance from the filtered pair. What is
measured is whether that rescue beats the stale form on this codebase.

## Novelty assessment

None. The contribution is the closed branch: three measured verdicts
(variance-only diverges, edge-stopped diverges, variance-updated
diverges) from this codebase.

## What was measured

Box scene + measure identical to loop29 s4 / loop30 / loop33 (64x64,
4spp vs 128spp, analytic sky -- K=1/k=2.0 reproduces the loop30 bar
1.334328e-03 EXACTLY, which validates the chain harness end to end).
it0 fixed at the loop30 best (k=2.0/ns=0.1/ds=1.0); it1 sweeps k
s4-style on BOTH legs (stale E2orig vs updated E2smooth):

- updated K=2: k=0.5 -> 2.673007e-03 (0.52x var), k=1.0 ->
  3.385240e-03 (0.41x), k=2.0 -> 2.958707e-03 (0.47x). Best is 0.50x
  of K=1 and 0.87x of raw -- worse than no second iteration at all.
- stale K=2 (same it0): k=0.5 -> 3.305434e-03, k=1.0 -> 7.054367e-03,
  k=2.0 -> 1.133212e-02. The update beats stale at every k (1.24x at
  k=0.5, 2.1x at k=1.0, 3.8x at k=2.0) -- the width correction matters
  most where the stale width is most wrong -- yet the best updated leg
  still loses to K=1 by 2x.

The smoother itself is characterized, not assumed: E2s preserves E2 DC
(ratio 1.0018, pin < 1.02) while cutting neighbor energy to 0.79x (pin
< 0.99x). A copy instead of a smoother fails the energy pin; zeros fail
the DC pin.

Pins (all structural -- the verdict is a measurement, not an
assertion): chain re-dispatch bit-identical (pure function of inputs);
updated != stale at the same k (the update is live); zeroed guide !=
E2s guide (binding 1 live); E2s true != E2s uniform (smoother reads its
gbuffer); K=2upd != K=1 (iteration 1 is not a no-op); strides (1,2) !=
(1,1) (doubling live in the updated form); chain geometry-wire both
legs K-matched (the loop33 trap); 0 validation diagnostics.
Sabotage-checked, each failing exactly its observers: updated:=stale
fails only the update-live guard; updated:=zero fails only the
guide-live guard; true-leg smoother:=uniform-geometry fails only the
smoother-geometry guard; a delta-kernel smoother (sigma 1e-9, output ==
E2 bit-exactly) fails exactly the three smoother-output observers
(energy, update-live, smoother-geometry) and nothing else; a zeroed
smoother input fails exactly the three E2s-value observers (DC,
guide-live, smoother-geometry) and nothing else. Revert restores the
measurement bit-exactly (all six MSEs identical across the round trip).

Two traps worth recording:

1. The huge-k reuse was considered and rejected before any dispatch:
   reusing denoise_edge_atrous with k=1e6 to smooth E2 no-ops on the
   sky, because the E2 image's own pseudo-variance
   luma(E2) - luma(E2)^2 clamps to zero wherever luma(E2) >= 1.
   denoise_moment_smooth.comp exists because that reuse is wrong --
   the smoother is deliberately variance-blind (spatial x geometry
   only).
2. An over-stopped smoother (ns/ds 1e-9) does NOT collapse the
   smoothing and passes every pin: sky-sky taps bypass geometry stops
   by design (w_geo = 1), and the sky dominates E2 neighbor energy, so
   the perturbation is a small hit-region copy the updated chain
   absorbs. A no-op sabotage is not evidence; the delta-kernel
   sabotage (sigma 1e-9) is the one that actually collapses the output
   to a bit-exact copy, and it is what the energy pin is validated
   against.

## What it revealed

The update helps (1.2x-3.8x over stale, same k) but the footprint
growth dominates anyway: at K=2 the 9x9 footprint smears hard edges
faster than corrected widths can compensate, and the best updated leg
(2.67e-03) sits 2x above the single-iteration bar it started from
(1.33e-03). The non-monotonic k ladder in the updated leg (k=1.0 worse
than both 0.5 and 2.0) says width/noise interplay, not a tunable
optimum -- the margin to K=1 is 2x, far beyond what it0-k tuning could
close. Iteration is not automatically virtuous even with a live,
recomputed variance guide.

## What remains (in order)

1. The multi-iteration branch is CLOSED in all three forms
   (variance-only loop29 s4, edge-stopped loop33, variance-updated
   here). No render-path filter loop; single-iteration stands as the
   v1 filter (loop31 consumer unchanged).
2. No further denoiser spikes are queued. The next denoiser work, if
   any, waits on a live-PT consumer need at published budgets -- not
   on another filter variant.

Process note, because it nearly cost this loop its test: `git checkout
-- <file>` as a sabotage-revert destroyed the uncommitted test (910
new lines, gone in one command) -- the revert discipline is a file
COPY to /tmp before the sabotage and a cp back after, verified with
diff, never a git checkout on uncommitted work. The test was rewritten
from the session record with all fixes baked in; the post-rewrite green
run reproduces every MSE bit-exactly, and the sabotage battery was
re-run against the rewritten file (not assumed from the first pass).
