# Render-graph and real-time rendering research — barrier placement, mode switching, PT

Status: research notes (2026-10-08). Feeds E2 (R1 path tracing) and the
compose-chain rebuild problem recorded in the bloom-toggle revert.

## R7. The bloom-toggle problem is a *known* problem with a known family of solutions

**Our recorded failure.** Runtime bloom toggle: (a) full recreate invalidates
the HDR render pass that scene pipelines bake (12 VUIDs, stale pass);
(b) the light path (create bloom side in place, rewrite binding 1) rendered
black because the rebuild did not interleave with the graph's cross-frame
layout tracking — the up stage writes the HDR image the tonemap is about to
sample.

**Source A — Blade / wgpu study.** Malyshau, "Global Pass Barriers Without
Per-Resource RHI Tracking: A Cross-Vendor Study with Blade"
(arXiv:2607.26506, 2026). Blade keeps Vulkan images in GENERAL permanently,
tracks NO per-resource state, and issues global pass-boundary barriers with
scope derived from the pass kinds around each boundary. Measured: removing 15
redundant barriers from 16 independent compute passes cut GPU span 29-32% on
RTX 5070 / RX 7900 XT. The closing direction is "lightweight aggregate
pass-kind state in a tracking-free RHI: an upstream render graph selects
global dependency cuts."

**Claim.** Per-resource layout tracking (what our compile_graph does) is one
point in a design space; the other endpoint — persistent GENERAL + global
barriers — is measurably *faster* in compute-heavy chains and eliminates the
entire class of "layout invalidation breaks pipelines that baked the pass"
failures, because GENERAL is valid for every usage.

**Novelty.** Standard technique (Blade is wgpu's production RHI lineage);
the cross-vendor measurement study is the new contribution.

**Fit/cost.** The honest cost is pipeline efficiency: GENERAL may disable
compressed formats on some tilers (the paper measures DCC retention on RDNA
under their conditions, FMASK loss). For our desktop-only target the wins
would be: bloom toggle becomes a re-record (no rebuild, no invalidation),
the capture chain's second instance needs no pass dance, and
`ensure_compose_resources` stops being a rebuild. BUT: our pipelines bake
render passes too (fullscreen tonemap bakes present pass), so GENERAL images
alone do not remove pass-baked state — the full solution is
dynamic-rendering-style (VkCommandBufferBeginRenderPassless /
local-draw-rendering), not just GENERAL. Adopted as R5 spec direction with a
two-stage plan: (1) move fullscreen passes to dynamic rendering so passes stop
being baked; (2) then evaluate a GENERAL-layout compose chain. Neither stage
lands without a pixel A/B + VUID gate.

**Source B — idTech 6 frame structure.** Courrèges, "DOOM (2016) Graphics
Study" (adriancourreges.com, 2016) + Sousa/Geffroy SIGGRAPH 2016: the shipped
frame is a long list of decoupled buffers (depth pre-pass + velocity map,
clustered forward, SSAO, SSR, probe reflections, particle lighting atlas,
blur chain, TAA, luminance, bloom) with heavy previous-frame reuse (static
shadow depth maps cached until their dynamics change). The relevant lesson
for our compose chain: DOOM does not toggle features by rebuilding the frame —
each feature is a pass whose COST is skipped when disabled, reading
conservative defaults from persistent resources (e.g. the blur chain always
exists; glass picks blur levels from it).

**Fit.** That is the correct shape for our bloom toggle: the bloom target and
blur chain should ALWAYS exist (they are ¼-res, cheap), and the toggle should
skip the down/up passes and bind the black fallback persistently — no
invalidation, no rebuild, because no resource is created or destroyed. Our
current chain creates bloom resources lazily-when-enabled, which is the
anti-pattern. Adopted: restructure so ALL compose resources exist from
initialize (bloom target is ¼-res RGBA16F — trivially small), and runtime
toggles only change which passes record. This inverts my light-path design
into a pre-allocated design and removes the interleaving problem by
construction.

## R8. Real-time path tracing (R1)

**Source.** Bitterli et al., "Spatiotemporal reservoir resampling for
real-time ray tracing with dynamic direct lighting" (ReSTIR, SIGGRAPH 2020)
and its ReSTIR-GI successors (Lin et al. 2022, Bitterli et al. 2022) —
SIGGRAPH/TOG, not arXiv; see Bitterli's publication page. Industry state:
idTech/Radeon/Intel ship ReSTIR-DI/GI variants; the SVGF/denoiser stack
(Schaidauer et al. 2021) completes the pipeline.

**Claim.** One-bounce PT with reservoir-based spatiotemporal reuse of light
samples achieves direct lighting at real-time rates; RTXDI packages the
same for engines. Full PT then needs a denoiser + accumulation, not per-pixel
convergence.

**Novelty.** Standard in industry now (ReSTIR is four years past
introduction; NVIDIA RTXDI is a shipped SDK).

**Fit/cost.** Our existing assets: RT pipeline + SBT infrastructure
(vulkan_rt_pipeline), a TLAS per frame, the pt_pathtrace rgen/chit/miss
shaders with a deterministic PCG contract, and accumulation semantics
(clear-once, add-per-frame) already implemented at test level. The gap is a
PRODUCT gap: the viewport has no PT mode because (a) the compose graph has no
storage-image stage, (b) no denoiser. The honest R1 spec, informed by the
above: PT mode = trace into a rgba32f accumulation image with the existing
deterministic contract → temporal accumulation over paused/stepped frames
(our protocol already pauses and steps!) → a tonemap pass reading the
accum image instead of the HDR intermediate (the compose chain's tonemap
bind is already abstracted; this is one descriptor rewrite at mode switch).
ReSTIR explicitly NOT adopted: our scene has ≤32 lights and the deterministic
PCG contract would be broken by reservoir stream state unless serialized into
the replay — a cost the deterministic-replay product cannot pay yet. The
spec for R1 is "offline-quality stepped PT": pause, accumulate K frames,
read back the converged mean — which our existing record/replay machinery
makes deterministic, and which no other engine offers because they do not
have our replay contract.

## R9. Frame graph orchestration (what compile_graph should grow into)

**Source.** The DOOM study (above) + the standard frame-graph lineage
(Thibieroz/Yong primer; AMD FrameGraph sample; BGFX's transient resource
system).

**Claim.** A frame graph that owns TRANSIENT resource allocation (allocate the
HDR intermediate, bloom chain, SSAO from the graph description; alias across
unused passes) removes the manual "create target in ensure()" class of
resource plumbing that caused the capture-chain and bloom-toggle complexity.

**Novelty.** Standard (every modern engine has one; BGFX's is the cleanest
public description).

**Fit/cost.** Our compile_graph already computes edges and barriers; the
missing half is transient-memory allocation (an allocator pass over the
lifetime intervals of the GraphPass-declared images). Bounded: a lifetime-
interval allocator over ~10 resources is a small, testable feature (no
aliasing needed initially — just allocation from the existing allocator with
exact sizes). Adopted as R6: compile_graph gains `resources` declarations and
`ensure_compose_resources` becomes derived, eliminating the manual
width/height plumbing that the capture chain duplicated.

## R10. What the idTech study says about our gd/renderer split

The velocity map produced during DOOM's depth pre-pass (dynamic objects write
per-pixel velocity; static inferred from depth + camera) is the standard TAA
input — and our H-Z packed pyramid already sits in exactly the same
"built-from-scene-depth, consumed-later" slot as their shadow/depth atlas.
No adoption needed now; recorded because R2 (RT reflections/AO) will want
the same velocity/depth pre-pass structure, and because it validates our
pyramid-as-first-class-resource direction from the rendering side.
