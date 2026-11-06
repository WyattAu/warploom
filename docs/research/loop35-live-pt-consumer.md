# Loop 35: live path-tracing consumer + the defects it exposed

Status: MEASURED 2026-11-06 (RTX 2060). The loop34 verdict said the next
denoiser work waits on a live-PT consumer need, so this loop put the
single-iteration filter on app scenes over the wire and measured what a
consumer would actually see — then fixed what the live path exposed.
No new technique; this is plumbing, defect-hunting, and one protocol
correctness fix, recorded here so the numbers are not re-measured later.

## What was measured (loop35 pass, viewport, 1280x720, mannequin scene)

Denoiser on a live app scene at K=16, camera pinned over the wire:

- Laplacian noise floor 0.029089 -> 0.008093 (3.6x smoother) at
  unchanged exposure: LDR mean 0.886119 -> 0.887030, bright fraction
  63.8% -> 64.5%.
- Wire means disagree: raw 0.947791767 vs denoised 0.732693234 (0.773x).
  An instrumented probe (`MARKER_DC_PROBE` in `GetPtMean`, since
  reverted) showed per-channel raw [0.219821, 0.236756, 0.254267] vs
  denoised [0.165482, 0.182827, 0.201211] and max texel 203.57 -> 39.68:
  the DC drop is firefly clipping, not darkening. (The probe's own
  divisor was 4x wrong — it divided by `v.size()`, not texel count —
  which is why it first looked inconsistent with the wire. The wire is
  correct.)
- Adaptive armed at a HELD sample target is a no-op by design: captures
  byte-identical (md5 `5197f6d22b5dd72d6c5177e33b8198ed`), samples
  unchanged.
- Re-entering PT mode does NOT reset the accumulation (the tracer is
  cached; samples stayed 40). `tools/live_proof.py`'s old comment
  claiming "a fresh tracer starts at sample 0" was stale.
- A resize DOES reset it — and the resize dropped two live settings on
  the floor (fixed, in-tree): `apply_resize` now re-applies
  `set_sample_target` and `set_denoise_enabled` after the tracer
  rebuild and clears the stale adaptive snapshot. Verified live:
  denoise survives a resize + back, adaptive reports off with the
  correct extent, stepped target re-converges to exactly 16 and holds.
- Every resize leaked one depth attachment: `create_depth_resources`
  never destroyed the previous view/image/memory (fixed, in-tree).
  Pinned by `VulkanRenderPass.DepthResourceRecreateReleasesTheOldAttachment`
  (16 recreations at 4 alternating extents; asserts after
  `context.cleanup()` because the leak reports at vkDestroyDevice).
  Sabotage-checked: the release block removed fails exactly that guard.
- Engine-contract assertions added in `test_path_tracing_real.cpp`:
  `create()` leaves denoise and adaptive OFF.

## Follow-on defects (this session, same live path)

1. **`clips_info` emitted invalid JSON on any clip with no tracks**
   (`modules/core/.../editor_session.cpp`). The `armed` array was
   appended without closing the `tracks` object, producing
   `"tracks":{,"armed":[]}` — and on clips WITH tracks it nested
   `armed` inside `tracks` instead of beside it. Found because g3 died
   in `json.loads` at the comma. Fixed (close `tracks` first) and
   pinned in `TimelineSession.ProtocolValidationAndInfo`, which now
   asserts the exact bytes for a trackless clip and the absence of
   `"tracks":{,`.
2. **g3's frame contract was wrong for viewports.** The proof assumed
   frame 0 on a fresh host, but a viewport ticks its session on every
   rendered frame (`app.frame_index` is the timeline position), so a
   clip at [0, 6) is born expired within a tenth of a second of arming
   — and `pause` alone does not fix it, because the frozen counter is
   still thousands past the clip. Rewritten frame-relative (in-tree):
   pause first, clip at [0, 1e9), assertions on counts/values not
   positions; tail-hold replay proven by restoring 0.75 over a 999
   sentinel; future-play silence proven by a watch clip recording the
   untouched y channel over the same steps (its count reaching 2 proves
   the ticks ran, x still 999 proves playback applied nothing);
   end-of-clip disarm via a zero-length clip (disarms on the first tick
   at ANY frame); queue-vs-tick races closed by polling counts, never
   by assuming a step has landed. A first probe-clip attempt misread
   the samples array (head/tail VALUES, not offsets) as an absolute
   frame — recorded here so it is not retried: the wire exposes no
   absolute session frame, and the proof must not need one.

## Verification (this session)

- `tools/live_proof.py g3`: 43/43 on a long-lived multi-run viewport
  (old clips present, high frame counter — the hardest condition).
- `tools/live_proof.py all`: **192 passed, 0 failed** (viewport A +
  headless B: w1, g1, e1 incl. the 9 resize checks, t5c, w2, g3).
- 0 validation diagnostics on the viewport host across the whole run.
- Suites: unit 747/747, core 22/22, editor 48/48, ui 19/19, runtime
  10/10, ctest 6/6. All four presets still build warning-free
  (no new warnings introduced; the touched core/viewport TUs compile
  clean under the default preset).
- e1 is now 92 checks; g3 is 43. The old "103/103" and "69/69"
  figures in `docs/rendering-status.md` are superseded (updated).

## Novelty assessment

None. A protocol serializer missing one closing brace, and a live proof
that assumed a counter value the engine never promised. The contribution
is the negative space: the wire exposes no absolute frame, so no live
timeline proof may depend on one — counts, values, and sentinel flips
are the whole observable vocabulary, and g3 now speaks only it.
