# Research loop 4 — render-graph transient allocation, loop 5 — determinism validator, loop 6 — PT accumulation

Status: notes, 2026-10-08. Three tightly-coupled loops around the compose
chain and its rebuild problem.

## Loop 4: transient resource allocation from the graph

**Source.** The frame-graph canon (Thibieroz/Yong; AMD FrameGraph sample;
BGFX transient buffers — the cleanest public description: transient resources
live for one frame, the graph computes their lifetimes, and an allocator
packs them into arena memory with reuse across non-overlapping lifetimes).
The DOOM study (adriancourreges.com 2016) shows the shipped result: ~50
render targets per frame, most transient, packed into atlases.

**Claim.** When the graph owns resource LIFETIMES, allocation becomes derived
state: sort intervals, pack into an arena, reuse after last use. Resource
creation/destruction code disappears from feature code — the class of
plumbing that made the capture chain and bloom toggle complex.

**Fit/cost.** Our compile_graph already computes edges; GraphPass already
declares attachments and sampled images with layouts. The missing piece is
an `outputs` declaration with format+size so compile can compute intervals.
Bounded: an interval-packing allocator over ~10 resources, allocated from the
existing VulkanMemoryAllocator. No aliasing in v1 (allocate distinct; alias
later — the arena reuse gives most of the win). ADOPTED as R6.

## Loop 5: the per-step determinism validator

**Source.** Jolt samples' "Check Determinism" checkbox: "Before every
simulation step we will record the state using the StateRecorder interface,
rewind the simulation and do the step again to validate that the simulation
runs deterministically."

**Claim.** A double-step-and-compare check built INTO the engine catches
nondeterminism the moment it is introduced, rather than when a test happens
to run. It is structurally the same record-rewind-replay proof our W2 track
ships — applied per-step instead of per-session.

**Fit/cost.** PhysicsWorld already has `position_fingerprint()`. A
`WARPLOOM_PHYSICS_DETERMINISM_CHECK` debug flag: after each step_single,
snapshot the fingerprint, step the same dt again from the pre-step state, and
assert equal fingerprints — 2x physics cost in debug builds only, zero in
release. This would have caught the scheduler race class, the all_ok bug, and
any future float-contract violation the day it lands. ADOPTED as a D1 debug
flag with an escape hatch (replay tests need the flag off to measure single
cost).

## Loop 6: PT accumulation as a product feature

**Source.** ReSTIR lineage (Bitterli et al., SIGGRAPH 2020; RTXDI) for
sampling; SVGF (Schied et al., HPG 2017) for denoising; our own W2
record/replay machinery for determinism.

**Claim.** Real-time PT pipelines ship as: sparse sampling + temporal
accumulation + denoiser. Our replay contract adds something the others do not
have: PAUSED ACCUMULATION. The viewport can pause time, step the same frame
repeatedly (the protocol already does this), accumulate K samples into the
rgba32f image, and display the converged mean — offline-quality converged PT
as an interactive product capability.

**Fit/cost.** Missing pieces: (a) the PT shaders exist but the compose chain
has no storage-image stage — spec: the tonemap pass gains an alternatesource
descriptor (accum image vs HDR target); (b) the accumulation clear/add is
already the shaders' design; (c) the viewport protocol gains
`set_render_mode path_trace` + the accumulation happens while paused.
Denoising: SVGF is a real arc — v1 ships un-denoised (converged-by-pausing
is the product story; live mode stays forward). ADOPTED as the R1 spec,
already written into the roadmap.

## Cross-loop conclusion

The three loops share one root insight: the compose chain and the physics
world both treat RESOURCES as features (create-when-enabled) when the
cutting-edge pattern treats them as PRE-ALLOCATED STATE whose USE is
conditional. Pre-allocation removes: the bloom rebuild, the capture-chain
second instance, and the ensure()-clobbering bug class. It is the single
highest-leverage refactor in the render layer.
