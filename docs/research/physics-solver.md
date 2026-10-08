# Physics solver research — XPBD, substepping, and where our solver sits

Status: research notes (2026-10-08). Adopted pieces feed D1's roadmap. Sources
listed per finding; arXiv IDs verified via the arXiv API, SIGGRAPH papers
cited by venue/DOI.

## R1. Our current solver, classified

`PhysicsWorld::step_single` is a **non-iterative projection scheme**: integrate
(semi-implicit Euler) → detect contacts against one frozen snapshot → one
positional correction per contact (stiff projection, not force-based) → one
normal impulse from the snapshot's relative velocity. It is deterministic by
construction (frozen snapshot + fixed pair order), which is the property the
broadphase equivalence and replay tests pin.

Where it sits in the literature: it is closest to a Verlet/position-based
scheme with a single solver iteration — the configuration "Small Steps in
Physics Simulation" (Fang et al., SIGGRAPH 2021) identifies as the reason
position-based solvers lose stiffness at large time steps. Our measured
symptom is the same one the paper names: stacks sink, and the iteration knob
we added is the paper's "more iterations" half-measure.

## R2. The cutting-edge alternative: XPBD with small substeps

**Source.** Macklin, Müller, Chentanez — "XPBD: position-based simulation of
compliant constrained dynamics" (MIG 2016) introduced the compliance
formulation; Fang et al., "Small Steps in Physics Simulation" (SIGGRAPH 2021)
established that *small dt with a few iterations beats large dt with many
iterations*; Li et al., "MGPBD: A Multigrid Accelerated Global XPBD Solver"
(SIGGRAPH 2025, DOI 10.1145/3721238.3730720) removes XPBD's remaining
stiffness ceiling with unsmoothed-aggregation AMG + PCG (convergence rates
comparable to adaptive smoothed aggregation at lower cost, enabling stable
high-resolution, high-stiffness scenes).

**Claim being borrowed.** Substepping (not iteration) is the primary lever for
stiffness: simulate dt/n with a couple of solver iterations per substep rather
than dt with many. The 2026 follow-up "Orientation in Extended Position-Based
Dynamics" (Tobin & Rucker, arXiv:2608.23606) additionally derives rigid-body
constraint gradients on the rotation manifold via Lie theory and reports a
>10^4 improvement in dynamic consistency of constrained rigid-body XPBD over
the prior state of the art — i.e. the frontier has moved to *rigid* bodies in
XPBD, not just cloth.

**Novelty assessment.** Standard. XPBD is published, taught, and shipped
(Jolt's soft bodies are XPBD per its architecture doc). Nothing here would be
novel research; it is adoption engineering.

**Fit/cost for Warploom.** High fit: our `step_single` already *is* a substep
(function of dt, integrate→detect→resolve); `step(dt)` already loops it
(`set_substeps`). What is missing for true XPBD: (a) compliance `α̃ =
1/(k·dt²)` in the correction — our correction is currently stiffness-∞
(full projection), (b) velocity update *from the corrected positions*
(v = Δx/dt) instead of a separate impulse pass, (c) rotation, which we do not
simulate at all. Cost: the impulse pass rewrite is the risky part (restitution
semantics change → every recorded physics replay changes output — the same
class of break as the D1a snapshot change, which we already declared
acceptable *with fingerprint bumps*).

**Spec decision.** Adopted in two stages: (1) keep the projection solve, add
the compliance-scaled correction as an opt-in (`set_contact_compliance`),
defaulting to stiff — so existing replays stay valid; (2) velocity-from-
position update behind the same opt-in. Substeps/iterations knobs stay as the
scheduling layer. MGPBD-style multigrid explicitly *not* adopted — we have no
high-resolution deformables; it solves a problem we do not have.

## R3. Speculative contacts (what our max-depth pyramid culling assumes)

**Source.** Catto's Box2D / GDC sequential-impulse series (2005-2015);
speculative contacts as shipped in Box2D v3 and in Bullet's speculative
margin.

**Claim.** Instead of resolving current penetration, solve the *speculative
TOI* contact: allow the contact constraint to also stop motion that WOULD
penetrate before the next step, eliminating tunneling without CCD passes.

**Novelty.** Standard (a decade of shipped engines).

**Fit/cost.** Moderate: our sphere-vs-plane/sphere contact generation would
grow the contact set with predicted-TOI pairs (the uniform grid's query
radius grows by v·dt). Deterministic — the predicted set is a pure function
of the same frozen snapshot. Cost: broader candidate pairs (the grid query
radius is velocity-aware), and the solver needs the speculative margin as a
limit on the allowed approach velocity. This is the correct fix for the
"fast bodies tunnel" class; our current engine does not have a tunneling
*proof* because the gd scene's velocities are low — this stays a roadmap item
with a tunneling stress test as its acceptance gate.

## R4. Islands and sleeping (Jolt's answer to our "no sleep" gap)

**Source.** Jolt Physics architecture docs (jrouwe.github.io/JoltPhysics):
bodies are grouped into islands (contact- or constraint-connected); islands
at rest sleep atomically; islands are REBUILT EVERY STEP, O(N), lock-free, so
concurrent insertion never invalidates cached island data.

**Claim.** Island-granular sleeping is deterministic and cheap when islands
are rebuilt per step from the (deterministic) contact graph rather than
cached.

**Novelty.** Standard across all major engines; the per-step rebuild is the
notable part (most engines cache islands).

**Fit/cost.** High value: our 400-body gd scene currently simulates all
bodies forever; sleeping would make the settled pile nearly free. The
contact graph we already build (frozen snapshot, deterministic order) is the
island-formation input — union-find over the pair list is O(pairs·α).
Determinism: union-find with fixed insertion order is deterministic. Cost:
wake-on-contact logic and a rest criterion; replay-safe because sleep state
is derived from the deterministic contact list, not stored.

**Spec decision.** Adopted for D1: island sleeping after contact-graph
union-find, rest threshold per island, wake on new contact with a non-sleeping
island. Acceptance: 400-body pile settles → all islands asleep → step cost
collapses; replay fingerprints unchanged (sleep is derivable state).

## R5. The determinism contract, compared to Jolt's

**Source.** Jolt "Deterministic Simulation" section + Fiedler, "Fix Your
Timestep" (gafferongames.com, 2004; the accumulator/interpolation canon).

**What Jolt guarantees that we do not, yet.** Cross-platform determinism as an
opt-in build (`CROSS_PLATFORM_DETERMINISTIC`, ~8% slower) making results
identical across compilers/architectures — we only claim same-binary. A
**record-rewind-replay double-step check** built into their samples: before
every step, record state, step, rewind, step again, and compare — a built-in
per-step determinism validator rather than a test-suite-only property.

**Fit/cost.** The double-step validator is cheap and directly applicable: a
debug-mode flag on PhysicsWorld that replays each step once and asserts the
fingerprint — it would have caught every physics determinism regression this
roadmap records, at 2x physics cost in debug only. Adopted for D1 as
`WARPLOOM_PHYSICS_DETERMINISM_CHECK`. Cross-platform float determinism stays
out of scope until a cross-platform product requirement exists (our CI is
lavapipe + this machine's GPU; the *tests* already pin same-binary
determinism).

Fiedler's article validates what we already shipped (fixed dt + accumulator +
interpolation in D2a), with one nuance we should adopt: their interpolation
alpha is `accumulator/dt` — our `sub_frame` equivalent — and our D2a easing
extension is a superset. No change needed; the citation is the receipt.

## R6. What we deliberately do NOT adopt

- **Sequential impulse with warm starting + contact caches** (Jolt/Box2D
  core): warm starting caches impulses across steps keyed on persistent
  manifolds — that is stored state, and stored solver state is exactly what
  our replay contract forbids unless it lives in the snapshot. A warm-start
  cache derived from the previous step's contacts would have to be serialized
  into every checkpoint. Cost > benefit at our current scene sizes. Revisit if
  ragdolls/vehicles land.
- **Mesh/heightfield shapes**: our collision pipeline is analytic
  (sphere-vs-plane/sphere); mesh shapes imply GJK+EPA and a contact-manifold
  builder — a separate project. The roadmap keeps "shapes" honest: next is
  capsule (cheap, useful for characters), not mesh.
- **Blade-style tracking-free RHI** (Malyshau, arXiv:2607.26506 — global
  pass-boundary barriers without per-resource tracking, measured 29-32% GPU
  span reduction removing redundant barriers): fascinating for the render
  layer, and directly relevant to the bloom-toggle rebuild problem (keeping
  images in GENERAL would have made the toggle a pure re-record). But it is a
  render-layer architecture change (wgpu/Blade lineage), and our compose graph
  already computes per-resource barriers deterministically. Recorded as an R5
  direction with the citation, not adopted now.
