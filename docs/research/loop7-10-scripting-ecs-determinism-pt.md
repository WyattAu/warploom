# Research loops 7–10 — visual scripting, ECS scheduling, cross-platform determinism, PT denoising

Status: notes, 2026-10-08.

## Loop 7: visual-scripting execution models (G4 editor wiring)

**Sources.** Zikas et al., "Scenior: An Immersive Visual Scripting system…"
(The Visual Computer 36:1965-1977, 2020, DOI 10.1007/s00371-020-01919-0 —
node-based scripting for VR training scenarios, acyclic-graph scenario
representation); Unreal BluePrint's exec-pin model (imperative dataflow with
execution wires); our own node graph (pure-function pull evaluation in
deterministic topological order).

**Finding.** Two families of node-graph semantics:
- *Dataflow/pull* (our graph, Godot's visual shaders): nodes are pure
  functions of their inputs; the engine pulls in topo order. No hidden state;
  evaluation order is an implementation detail. Replay-free by construction.
- *Imperative/exec* (Unreal BluePrint): nodes have execution pins that
  sequence side effects; evaluation order is user-visible state, which is why
  BluePrints have explicit exec wires and latency nodes.

Our deterministic-replay contract REQUIRES the pull model for anything
recorder-visible — an exec-pin model would put evaluation order into the
replay stream. The G4 script node (pure tick(dt, inputs)) fits the pull model
exactly. Spec: the editor UI presents script nodes like math nodes (param row
per in/out pin); no exec wires ever. Confirmed existing design; no change.

## Loop 8: ECS scheduling — our bitmap waves vs archetypes

**Sources.** Bevy scheduler docs + Alice Ryhl's "Bevy's scheduler" talk
(archetype iteration + multi-threaded conflicting-system partitioning);
EnTT's sparse-set backing store; Unity DOTS archetype chunks. Our
SystemScheduler: component-mask intersection + dependency-DAG wave
partitioning (add_system(name, fn, deps, mask)).

**Finding.** Three orthogonal axes: memory layout (archetype vs sparse-set),
scheduling (wave/mask vs dependency graph), and iteration order. Our ECS is
sparse-set-backed (add_component per entity) with mask-based wave
partitioning. Archetype iteration wins when a query touches MOST components
of MOST entities (linear memory walks); sparse-set wins when queries are
sparse and components are added/removed dynamically — which matches our
editor-authoritative document model (objects mutate per edit, not per spawn
burst).

**Spec.** No change now; the measurable question (recorded for the next
research loop): prototype an archetype table for the gd scene's 400 bodies ×
4 components and measure iteration vs our query<Position> walk. Adopt an
archetype path ONLY if the gd draw-loop profile shows query iteration as a
hotspot (it currently does not — compose chain dominates).

## Loop 9: cross-platform float determinism

**Source.** Jolt's CROSS_PLATFORM_DETERMINISTIC build option: identical
results across MSVC2022/clang/gcc, x86/ARM/RISC-V/PowerPC, at ~8% cost.
Mechanism: no FMA contraction, no platform libm (own sin/cos), precise fp
model, fixed rounding. Fiedler's articles add: same-binary determinism
requires only consistent fp state + deterministic op order, which we already
have.

**Finding.** Our same-binary determinism is TESTED (replay fingerprints,
RepeatedRunsAreBitIdentical, the determinism sentinel benchmark). What we do
not have: (a) a documented list of the floating-point hazards (FMA
contraction in release builds — our -ffp-contract status is unset on GCC!;
(b) own libm. GCC emits fused multiply-add under -O2 with
-march=native-style flags unless constrained.

**Spec.** Adopted, small and verifiable: add `-ffp-contract=off` to the
compiler flags (clang AND gcc — this is the single most dangerous
nondeterminism source in release builds) and document the fp contract in
AGENTS.md. Full cross-platform determinism stays out of scope (no
cross-platform requirement yet); the flag costs nothing measurable and
removes a whole bug class.

## Loop 10: PT denoising and the accumulation strategy

**Source.** SVGF (Schied et al., HPG 2017): spatiotemporal variance-guided
filtering — first moment (color) + second moment (variance) buffers, temporal
history validation via depth/normal, edge-aware spatial wavelet passes whose
iteration count is driven by the variance estimate.

**Finding.** SVGF's core inputs are exactly what our deterministic PT
accumulation produces naturally: per-frame color AND running variance (the
accum image's mean and the per-frame difference). But SVGF's temporal
accumulation assumes a continuous camera — our product story is PAUSED
convergence, where accumulation alone converges without any denoiser. The
denoiser only matters for live (unpaused) PT, which is explicitly out of the
v1 spec.

**Spec.** v1 PT ships un-denoised with pause-to-converge as the product
feature (already written into E2). SVGF recorded as the R2 follow-up if live
PT is ever wanted — with the note that its temporal-history validation
conflicts with deterministic stepping unless history validation is also a
pure function of the accum state.

## Cross-loop conclusion

The determinism contract is not a constraint on adopting cutting-edge
techniques — it is a FILTER that picks the techniques whose state is derivable
from the snapshot. Islands (derived from contacts), substeps (pure schedule),
accumulation (pure sum) all pass. Warm-start caches, reservoir streams, and
exec-wire scripting all fail the filter — and the filter is the product.
