# Research loop 13 — frame-graph lifetimes derived from the node sequence (Loop 2 of 10)

Status: executed 2026-10-26. Competitor-first per user direction.

## Problem in the spec

The comparison matrix (§4.2) states R6b is "gated behind a prerequisite that
does not exist yet: the renderer declares resources against a fixed pass
list, not a graph, so 'a resource live on passes [i, j]' does not yet
describe anything real". `plan_resource_allocations` (R6a) was pure, tested
and sabotage-validated — but its INPUT was hand-written. This loop closes
exactly that prerequisite: lifetimes are now derived from the
`GraphNode` sequence the renderer actually records.

## Papers pulled

- P1. Sandu & Shcherbakov, "A Resource Allocation Algorithm for a
  History-Aware Frame Graph", Journal of WSCG 31(1-2):63–70, 2023 (already
  the R6a basis). Re-read for the placement rule: lifetimes derive from
  graph position, not from per-resource declarations.
- P2. Malyshau, "Blade" tracking-free RHI (arXiv:2607.26506). Claim:
  global pass-boundary barriers without per-resource tracking, measured
  29–32% GPU span reduction from removing redundant barriers. Relevant as
  the R5 direction (dynamic rendering first); NOT adopted here — our
  compile_graph already computes per-resource barriers deterministically,
  and this loop is about lifetimes, not barriers.
- P3. Stołecki, "Frame Graph: Production Engines" (Deep Spark, Feb 2026).
  Independent survey confirming the taxonomy below across shipped engines.

## Competitor repos (source-read or vendor-doc, labelled)

- Unreal RDG (Epic docs, UE 5.8, vendor documentation): resources are
  Transient (lifetime constrained to the graph, memory may alias) vs
  External (owned outside). "Allocation of transient resources with optimal
  lifetimes and memory aliasing" is a headline RDG feature; pools are the
  fallback. Our history flag == their External; our transient == theirs.
- AMD RPS (GPUOpen tutorial part 2, updated 2024-06, vendor documentation):
  four classes — external, persistent, transient, temporal slices. External
  and temporal slices are implicitly persistent; the runtime "automatically
  creates and destroys transient resources within graphics API heaps" and
  "can efficiently alias such resources". Our derivation takes exactly the
  RPS split: graph-derived lifetimes + caller-declared persistence.
- Godot (source-read, matrix §1.1): `rendering_device_graph.cpp` (2885
  lines) does topological sort + barrier synthesis + command-buffer
  partitioning — but has NO transient allocator (`find *transient*` empty
  outside TBDR lazy-attachment bit). Deriving lifetimes is the step Godot
  never took; we take it without claiming the allocator.
- Unity RenderGraph (public docs): descriptor-matched pooling ("reuse
  allocated memory if a texture has the same properties as an earlier
  texture") — Epic's docs name exactly this as what transient aliasing
  beats. We implement neither pooling nor aliasing yet; the planner stays
  the honest midpoint.

## Best-solution analysis

Three candidates, one winner:

- (a) Derive lifetimes from nodes (CHOSEN): mechanical walk, pure,
  testable without a device, unblocks R6b. Risk: nil — additive, no
  existing callers change.
- (b) Full R6b Vulkan wiring (allocate/alias images from the plan):
  touches compose-chain ownership, resize paths, capture-stack sizing —
  the exact class of change that caused the capture resize device-loss.
  DEFERRED to a dedicated change with its own verification run.
- (c) Transient aliasing in the planner (pack into shared memory):
  the planner already answers WHO may share; physical aliasing needs (b)
  first. DEFERRED behind (b).

Nobody solves (a) better: RPS derives lifetimes inside its scheduler
(closed runtime, not a separable pure function); Godot doesn't derive at
all. A standalone tested derivation is the unusual — and replay-correct —
shape: lifetimes become a deterministic artifact, not runtime state.

## Novelty assessment

Nothing novel. Interval derivation from a pass sequence is textbook
(RPS build callbacks do it inline). The combination — derivation as a
separable pure function + history as caller knowledge + Lean proof of the
planner invariant — is a combination, not an invention.

## Fit/cost

`derive_frame_resources(nodes, size_of, is_history, tag_of)`:
first-touch order fixes names, walk order fixes intervals, NULL uses
skipped. Function-pointer callbacks match `execute_graph`'s style: no
allocation, no capture. Cost: callers must supply size/history/tag — new
API surface, but explicit and auditable (RPS-persistent classes are policy,
not graph facts).

## What was measured

3 new Loop13 tests over a compose-shaped 5-node sequence (scene, bloom
down/up, present, H-Z compute): lifetimes match first/last touch per
image; scratch2 [3,3] aliases scratch1 [1,1] (same size, disjoint, most
recent); bloomB [2,3] stays Dedicated (overlaps bloomA at pass 2 despite
same size); all three history images (swap, pyramid, PT accum) Dedicated;
derivation deterministic across runs; null-only pass contributes nothing.
Sabotage (never extend last_use) fails the 2 behavior tests and correctly
passes the determinism-only one. 10/10 FrameAllocator. Lean
`proofs/FrameAllocator.lean` (`lake build` clean): most-recent-fit search
modelled; history always plans Dedicated; every Aliased entry names a
compatible, interval-disjoint owner.

Still open (R6b): the Vulkan side — create/alias/destroy from the plan.
The plan's input is now derived; the wiring is the scheduled next step.
