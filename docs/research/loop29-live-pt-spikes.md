# Loop 29: live-PT stepping discipline, adaptive spike, denoiser spike

Status: stepping discipline LANDED 2026-11-03 (RTX 2060); adaptive and
denoiser SPIKES measured the same day. No build beyond what the spikes
justify -- the adaptive build and the full denoiser are specified, not
started.

## 1. Per-frame stepping discipline (build)

Problem: `set_camera` + `record_round(clear_first=false)` let a caller
accumulate a new pose onto an old sequence -- the pose-smear
(AGENTS.md: "every round traced a different pose and averaged a smear").
The viewport worked around it by holding the camera; the API itself
allowed the defect.

Rule, now in `VulkanPathTracer`: `set_camera()` detects a real pose/fov
change (field-wise `==`, frame seed word excluded) and marks the sequence
stale; the next `record_round()` restarts it (both images cleared, count
to 1) even with `clear_first=false`, and releases the sample-target hold
for the same reason. Re-uploading identical values (the viewport's PT
hold) is a no-op. The restart is a pure function of the call sequence,
so replay is unaffected.

Proof: `path_tracing_real.camera_move_restarts_sequence` -- counts
(5 with the hold path, 1 after the move), forced-vs-explicit-clear
bit-identity (mean AND moment), hold no-op image-identity, restart
through the hold with a reposed image. Sabotage-checked both
directions: dropping the forcing leaves count at 6 (smear back);
always-dirty breaks the hold path (1 instead of 5).

Finding, kept: cross-tracer bit identity does NOT hold (132/4096 floats
differ, max abs 1.0, between two identically-configured tracers at the
same pose and seed -- each builds its own BLAS/TLAS at different device
addresses and edge-tie traversal differs). The replay contract is per
tracer instance. Same-tracer forced-vs-explicit memcmp is 0.

## 2. Adaptive spike (measure)

Question: does noise concentrate enough for per-pixel sample allocation
to beat uniform? Measurement (`variance_concentration_spike`, 64x64 box,
16 spp, deterministic bit-identical across runs): top 1% of pixels hold
40.5%, top 5% hold 92.4%, top 10% hold 100% -- the rest read exactly
zero (true variance below float rounding at 16 spp). Noise lives at
discontinuities (hit-vs-miss binary variance); smooth interiors are
already converged.

Verdict: JUSTIFIED for this scene class. Uniform spending wastes ~90%
of rounds on converged pixels. Pinned: top1% > 0.30, top5% > 0.80,
top10% > 0.95. The adaptive BUILD (masked re-trace + per-pixel sample
counts) is the next T2 step, not this loop.

## 3. Denoiser spike (measure)

Question: what does variance-only guidance buy without a G-buffer?
Implementation: `assets/shaders/denoise_variance_atrous.comp` -- one
5x5 a-trous iteration, edge width k*sqrt(v_center) from the (mean,
moment) pair, spatial sigma 1.5px, rgb filtered on luma weights.
Measurement (`denoise_variance_spike`, 4spp vs 128spp reference):
raw MSE 4.16e-04; k=0.5 -> 1.93e-04; k=1.0 -> 1.60e-04;
k=2.0 -> 1.55e-04. A 2.68x MSE reduction -- a ~2.6x effective sample
multiplier with no G-buffer. Pinned: best < raw/2.

**Correction 2026-11-04: that pin did not reproduce, and the spike's own
test form was the reason.** The committed test built its scene with the
LEGACY CONSTANT sky, so every path returned the same radiance and the
4spp image carried almost no Monte Carlo noise (raw MSE 1.65e-05, which
is silhouette jitter). No filter can halve that, so the test failed
while the shader was fine. The measurement above came from a run with a
noisy environment that the committed test did not keep -- a docs claim
true of one configuration and false of another, the exact shape this
repository keeps paying for.

Re-measured with the analytic sky restored (RTX 2060, 64x64 box,
4spp vs 128spp): raw 2.313658e-03; k=0.5 -> 1.677682e-03;
k=1.0 -> 1.649923e-03; k=2.0 -> 1.385478e-03. **1.67x**, not 2.68x.
The honest headline is 1.67x at 4spp with no G-buffer. The pin is now
`best < raw * 0.70` (measured ratio 0.599). Reaching 2x would need a
better variance estimate (which needs the G-buffer this spike set out to
avoid) or more iterations; neither is justified by the numbers yet, so
the build stays where the measurement is.

Constraint check, pinned: re-dispatch over identical inputs is
bit-identical (filter state is a pure function of accumulation state,
nothing hidden) -- the temporal-history requirement holds by
construction.

History: the pin caught a real bug during development -- the neighbor
tap read unnormalized sums against a normalized center (dm = 3x the
mean at 4 spp), collapsing every weight including the center's, and the
test failed three runs (MSE ~0.95, outputs ~0) before the one-line fix.
Like the H-Z barrier lesson: a picture that looks reasonable is not
evidence; the MSE against the reference is.

Open for the full build: the G-buffer (normals exist only in the hit
payload, albedo is a raygen constant, depth is never stored). The spike
says: build the filter loop first (multi-iteration a-trous on the
variance signal, still no G-buffer), measure against this 2.68x bar,
and add the G-buffer only when edges -- not interiors -- dominate the
residual.

## Cost

- Determinism: none. Stepping is call-sequence-pure; spikes read back
  but change no engine state.
- Devices: ray-tracing gate, like all PT tests (skip with reason).

## What was measured

All on the RTX 2060, 2026-11-03: stepping test green both sabotage
directions; concentration top1/5/10% = 0.405/0.924/1.000 (identical
across runs); denoise raw/best = 4.16e-04/1.55e-04; 0 validation
diagnostics throughout.

Re-measured 2026-11-04 with the noisy sky the test now actually builds:
denoise raw/best = 2.313658e-03/1.385478e-03 (1.67x), same test, same
device, 0 validation diagnostics. The earlier pair is kept above because
it is what the spike recorded; it is not reproducible from the committed
test and is superseded by this one.
