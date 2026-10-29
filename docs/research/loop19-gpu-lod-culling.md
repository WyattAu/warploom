# Loop 19 — GPU-driven LOD selection bounds (research loop 9 of 10)

Status: notes + fix, 2026-10-26.

## Sources

- **Clark, "Hierarchical geometric models for visible surface
  algorithms", CACM 19(10), 1976, DOI 10.1145/360349.360354**
  (peer-reviewed, archival): the LOD original — fidelity follows
  projected size; coarser variants substitute as screen coverage shrinks.
  Our three shaders implement exactly this vocabulary (projected sphere
  size vs. per-level pixel thresholds). Nothing borrowed beyond the
  problem statement; the paper predates GPUs by decades.
- **Greene, Kass & Miller, "Hierarchical Z-buffer visibility",
  SIGGRAPH '93, DOI 10.1145/166117.166147** (peer-reviewed, archival):
  hierarchical occlusion via a depth pyramid, front-to-back traversal,
  temporal reuse of visibility. Related, NOT adopted: our occlusion path
  (`cull_and_draw_lod_occlude.comp` + `cull_hiz_sampled.comp`) samples a
  previous-frame pyramid conservatively but has no pyramid-reuse proof
  and no front-to-back ordering claim — citing Greene here would dress a
  one-sample conservative test in a hierarchy's clothes. Recorded as the
  obvious next loop if occlusion gets its own budget.
- **Unreal HLOD / per-mesh LOD screen-size docs** (vendor documentation,
  dev.epicgames.com — labelled as such): LOD transitions keyed to
  on-screen size thresholds, same vocabulary as our `lod_thresholds`
  (screen-space px, descending). Convergent choice, not a borrow.
- **The GLSL/Vulkan out-of-bounds contract** (spec, cited without
  section number deliberately): indexing an array out-of-bounds is
  undefined behaviour. `robustBufferAccess` — which would at least define
  OOB *buffer* reads as zero — covers buffers, not push-constant array
  indexing, and is NOT enabled anywhere here (0 matches in-tree,
  measured). So an over-large host count is not "reads zero", it is UB
  the compiler may exploit (e.g. unrolling on the assumption `t < 8`).
- **Our code**: `lod_select.comp` (`thresholds[8]`), `cull_and_draw_lod`
  + `_occlude` (`lod_thresholds[4]`) — three shaders, two capacities, no
  enforcement; prod hosts (viewport pushes `count = 1`, tests `2-3`).

## Claims being borrowed

1. LOD level is a pure function of projected screen size (Clark). No
   hidden state; selection stays replay-compatible.
2. A host-provided array count the shader does not clamp is a
   latent UB read (the spec contract above), not a latent zero.

## Novelty assessment

None. The fix enforces the arrays' own declared capacities at the use
site — the same shape as Loop 18's cap placement (bound checked before
use, identity on all in-tree inputs).

## What fits Warploom, and what it costs

**The defect: threshold counts trusted against fixed arrays.**
`lod_select.comp` loops `t < threshold_count` over `thresholds[8]`;
both cull shaders loop to `lod_threshold_count` over `[4]`. Any host
passing more levels than the array holds (a 5-level LOD chain is the
natural next asset) reads past the array: in `lod_select` the words
past index 7 are `camera_position`, which compare as 0.0 px — a sphere
below every real threshold then "meets" the phantom threshold and comes
back visible with a LOD naming no variant. Fix: `min(count, bound)` at
each selection site (`8u` / `4u`), sentinel logic unchanged. For every
in-tree count the `min` is identity, so behaviour is bit-identical
unless the input was previously UB.

**The live-fire test** (`LodSelectClampsThresholdCountToArrayBound`,
RTX 2060): one sphere at depth 96 (~6.5 px) with 8 descending thresholds
{200..8}, hostile `threshold_count = 10`, camera at origin (OOB words
deterministically 0.0). Fixed shader: sentinel survives → culled
(`vis=0, lod=0, count=0`). Sabotage A/B by real shader rebuild (clamp
removed): `vis=1, lod=8, count=1` — all three assertions fail, exactly
the predicted phantom selection. The validation layer is silent either
way (UB, no VUID) — which is why the test, not the layer, is the
instrument here. This is also the shape the cull-shader clamps share,
but their OOB words lie PAST the push range (truly driver-undefined),
so no deterministic live-fire exists for them: they are verified by
recompile + unchanged existing GPU tests (identity on counts ≤ 4) +
the Lean bound, and the doc says so instead of pretending otherwise.

**Audited, no change:** `lod_count == 0` payload underflow (`level =
lod_count - 1u` wraps) — unreachable: every in-tree payload writer
emits `lod_count = 1` (viewport `u[22] = 1U`, tests likewise); a
zeroed-payload dispatch is garbage-in outside the host contract, and
"fixing" it would invent shader behaviour for a state nothing produces.
Recorded as open, not silent. Frustum-plus-near cull order, NaN sphere
behaviour (comparisons false → full-detail draw, fail-safe direction),
atomic `visible_count` order-independence, and the lod_select
sphere/result non-overlap (offsets from one layout) all read correct.

## What was measured

- New GPU test passes on the fixed shader (RTX 2060, DISPLAY=:0);
  sabotaged shader fails all three assertions (`1/1/8` vs `0/0/0`).
- Existing LOD suites green on the fixed tree: `LodSelectByDistance`
  plus the `test_gpu_driven_*` / `test_lod_integration` set below.
- All three shaders recompile (74-shader build, 0 warnings).
- Lean `proofs/LodSelect.lean`: `select_le` (clamped ⇒ every read
  in-bounds), `unclamped_selects_bogus_lod` + `clamped_select_culls`
  (the exact GPU-test scenario as `native_decide` exhibits). No axioms.
