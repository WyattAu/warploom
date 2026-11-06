# Loop 31: render-path denoiser consumer

Status: LANDED 2026-11-06 (RTX 2060). The spikes (loop29 variance-only,
loop30 edge-stopped) now have a render-path consumer: `record_denoise()`
on the tracer, toggled live by `set_denoise` (protocol v1.18), tonemapping
the filtered MEAN with inverse_samples = 1.

## Source

Same two claims as loop29/loop30, unchanged:

- Dammertz, Sewtz, Hanika, Lensch. "Edge-Avoiding A-Trous Wavelet
  Transform for fast Global Illumination Filtering." Proc. HPG 2010.
  Claim borrowed: spatial x geometry edge-stopping weights.
- Schied et al. "Spatiotemporal Variance-Guided Filtering." Proc. HPG
  2017, DOI 10.1145/3105762.3105770. Claim borrowed: per-pixel variance
  drives the filter width.

## Claim being borrowed

A single-iteration edge-stopped a-trous over (mean, moment, G-buffer)
preserves discontinuities while smoothing interiors -- now with the ONE
change the render path needs: per-texel divisors from the count image
instead of scalar push-constant counts, so held texels under adaptive
masking keep their own (older, smaller) counts and the filtered mean and
variance stay exact per texel.

## Novelty assessment

None. Same textbook pairing as loop30; the count-sourced divisor is a
plumbing change, not a technique. What is new is only the measurement on
this codebase (below) and the lifecycle (below).

## Why it fits here, and what it costs

The tracer already owns all four inputs with matching extents, barriers
and readback, so the consumer lives IN the tracer: one output image
(same creation path, so it carries the SAMPLED bit the tonemap needs),
one compute pipeline, one descriptor set, `record_denoise()` +
`read_denoised()`. Resize/recreate rebuilds it with the stack (no second
lifecycle). The compose chain's external-source barrier takes the
producer scope with the image (ray-trace write by default, compute write
when the viewport points at the filtered mean), so the same-submit
tonemap AND the cross-submit capture read stay ordered -- a scope that
travelled separately could drift from its image, and a wrong scope is
silent (a zero-scope barrier is legal and orders nothing).

Cost: one rgba32f image + one 5x5 stride-1 dispatch per frame when
enabled (default off: every existing caller tonemaps raw exactly as
before). Determinism contract unchanged: pure function of
(sums, moment, gbuffer, counts).

## What was measured

Same scene + measure as loop29/loop30 (64x64 box, analytic sky, 4spp vs
128spp reference):

- Uniform: raw 2.313658e-03 reproduces exactly; engine-filtered
  1.334328e-03 reproduces the loop30 best EXACTLY (1.73x over raw),
  bit-identical to the frozen spike shader over the same accumulation
  (pinned by `denoise_render_path_uniform_parity`).
- Adaptive (4-uniform + mask(0.25) + 4-masked): raw-adaptive
  2.313659e-03, filtered 1.334325e-03; uniform-overwritten geometry
  1.385475e-03 collapses onto variance-only 1.385478e-03 (the loop30
  geometry-blind equivalence, through the new shader). Pins mirror
  loop30's margins: < raw*0.70, < geometry-blind*0.99 (measured 0.963,
  the same 4% depth-stop gain on the masked stack).
- Sabotage-checked live: re-create on a live tracer (the resize path)
  FAILED before the fix below and passes after; all-held zero stack
  filters to exact zeros; re-dispatch over unchanged inputs is
  bit-identical; an extra round moves the output.
- Live (RTX 2060): e1 79/79 including denoise on/off, reads-back-on/off,
  denoised-mean-differs-from-raw at a held 16 samples, refused-outside-PT,
  missing-enabled rejected, and the resize round trip WITH the denoiser
  rebuilt; headless `all` 108/108 (3 new parser checks).

## What it revealed

`VulkanPipeline::cleanup` nulled every shader handle except
`compute_shader_`, so the SECOND `create()` on a live tracer (the resize
path -- the only re-entrant caller) refused with "denoise shader load
failed" and killed the live e1 at the first resize. The headless suite
never calls create() twice, so no test could catch it; the new pin
(re-create + re-dispatch bit-identical) closes exactly that hole, and
the sabotage round-trip (revert one line -> the pin fails with the
production message -> re-apply) confirms the guard traps the real
defect. An asymmetric cleanup (four handles nulled, one not) is worth a
grep whenever a new handle joins the struct -- which is also why the
fix is one line, not a redesign.

## What remains (in order)

1. Curved-scene spike for the normal term (still unproven; planar
   scenes leave it inert).
2. Multi-iteration WITH geometry guidance (only variance-only
   iteration is closed -- loop29 s4).
3. The per-iteration variance update alternative from the roadmap
   (undecided, not started).
