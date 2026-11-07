# Physics solver research: XPBD, substepping, and where our solver sits

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
scheme with a single solver iteration; the configuration "Small Steps in
Physics Simulation" (Fang et al., SIGGRAPH 2021) identifies as the reason
position-based solvers lose stiffness at large time steps. Our measured
symptom is the same one the paper names: stacks sink, and the iteration knob
we added is the paper's "more iterations" half-measure.

## R2. The cutting-edge alternative: XPBD with small substeps

**Source.** Macklin, Müller, Chentanez; "XPBD: position-based simulation of
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
the prior state of the art; i.e. the frontier has moved to *rigid* bodies in
XPBD, not just cloth.

**Novelty assessment.** Standard. XPBD is published, taught, and shipped
(Jolt's soft bodies are XPBD per its architecture doc). Nothing here would be
novel research; it is adoption engineering.

**Fit/cost for Warploom.** High fit: our `step_single` already *is* a substep
(function of dt, integrate→detect→resolve); `step(dt)` already loops it
(`set_substeps`). What is missing for true XPBD: (a) compliance `α̃ =
1/(k·dt²)` in the correction; our correction is currently stiffness-∞
(full projection), (b) velocity update *from the corrected positions*
(v = Δx/dt) instead of a separate impulse pass, (c) rotation, which we do not
simulate at all. Cost: the impulse pass rewrite is the risky part (restitution
semantics change → every recorded physics replay changes output; the same
class of break as the D1a snapshot change, which we already declared
acceptable *with fingerprint bumps*).

**Spec decision.** Adopted in two stages: (1) keep the projection solve, add
the compliance-scaled correction as an opt-in (`set_contact_compliance`),
defaulting to stiff; so existing replays stay valid; (2) velocity-from-
position update behind the same opt-in. Substeps/iterations knobs stay as the
scheduling layer. MGPBD-style multigrid explicitly *not* adopted; we have no
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
radius grows by v·dt). Deterministic; the predicted set is a pure function
of the same frozen snapshot. Cost: broader candidate pairs (the grid query
radius is velocity-aware), and the solver needs the speculative margin as a
limit on the allowed approach velocity. This is the correct fix for the
"fast bodies tunnel" class. Gate test landed 2026-11-06
(`PhysicsWorld.FastBodyTunnelsOnSomePhasesR3Gate`): 12 launch phases at
S = 3W through a static sphere, 8/12 tunnel — 4 clean misses (no sample in
the window) plus 4 far-side ejections (sample lands past center, projection
exits forward, the separating contact draws no impulse so full speed is
kept; colliders project to the near face and rebound via the bias). The
trigger is met and the failure is sharper than "misses fast bodies": on
far-half samples the solver throws the body through. Speculative contacts
must drive the count to 0/12 without changing the colliding phases'
near-face outcome;per-step contact telemetry (not final poses) is how the fix is verified,
since ejections and misses are indistinguishable post-hoc.

**Outcome (loop52).** Landed as an opt-in (`set_speculative_contacts`,
default off): frozen-snapshot swept test for sphere-sphere (entry root of
|c0 + d*s| = R; start-inside pairs stay discrete, exit root deliberately
unsolved), TOI rewind split by inverse mass in pass 0, snapshot impulse
unchanged. The grid query grows by v·dt exactly as predicted (swept
insert + swept query, trailing sort+unique keeps grid order == nested
order structurally). Measured: 0/12 cross, every phase shows a contact
event (the 4 historical clean misses now fire speculative contacts), the
4 colliding phases keep near-face + rebound to 1e-3 (2e-5/1.5e-4 measured
gaps — different op sequences, same physics), swept grid agrees with
all-pairs at 200/600/1200 with TOI traffic (mixed sphere/capsule scene
since loop61 — every other fast body is a capsule, so the bound-keyed
gathering and the segment TOI path are covered at scale, not just the
gate's two-body case). Gate sabotage-validated
(TOI suppressed fails the gate; revert re-greens). Limits, documented in
code: spheres only (capsules land two paragraphs down), penetration 0
so no island edges (conservative). The stiff-rewind limit is gone as
of loop63 above.

**Capsule follow-up (loop55 gate, loop57 fix).** `FastCapsuleTunnelsOnSomePhases`
ports the R3 protocol with the shot as an upright capsule (r=0.1,
half-height 0.4, identical 198 m/s travel — shape is the only variable).
Trigger met 8/12, same count as spheres. Fixed the same way, one level
up: the bound-sphere entry (shared `swept_entry_root` helper — the two
branches must not drift) triggers, then fixed-3 Newton iterations on the
true segment gap at (now interpolated — loop67) orientations refine to
the surface TOI; the
capsule solve runs at the refined centers, with the center rewind +
impulse shared with spheres. Two subtleties the gate caught: (1) the
Newton rate sign (gap shrinks as s grows — closing rate is minus the
relative velocity along the outward normal; inverted, every phase
passes silently); (2) start-inside-bound is transit, not rest — the
bound is entered on an earlier step, so shell transit climbs Newton
from s=0 while only start-inside-SURFACE keeps the historical miss.
Measured: 0/12 cross, per-phase contact telemetry, every phase matches
its sphere twin to 1e-3 (the hover guard with teeth — a bound-only
rewind lands 0.4 short and the rebound trajectories diverge). Gate
sabotage-validated (capsule TOI suppressed fails 60 assertions; sphere
gate still passes — the sabotage is surgical). Limits (loop67
retires the first): the refinement ran at fixed end orientations, and
a tumbling-pair probe (`SpinningCapsuleStillStopsAtWall`: 12 phases x
5 spin rates to 300 rad/s) found it was not an approximation but a
hole — a silent 3.3 m tunnel at 30 rad/s, and tip-first embeds at 300.
The refinement now interpolates axes across the substep (nlerp from
step-start to end orientations — trig-free, no new libm on the tick
path) with the Newton budget raised to 8 for spinning pairs only
(the nlerp gap is less smooth; still fixed-count, still
relative-velocity rate). Both-zero spin keeps the old path verbatim,
so every non-spinning scene is bit-identical. Sabotage both
directions, each surgical: no interpolation fails spin-30-phase-7
silently; no budget fails spin-150-phase-3 silently; gates,
compliance, sleep and rewind tests never move. Remaining limit:
non-converged grazes miss one step and retry (delays rather than
tunnels) — now measured to hold through 300 rad/s tumbles.

**Compliant rewind (loop63).** The pass-0 rewind was stiff even under
contact compliance — a fast body hard-stopped at the TOI surface while
slow contacts in the same scene sank to the compliant equilibrium.
It now carries the first-iteration XPBD factor
`inv_eff/(inv_eff + atilde)` (Trial 638's exact-XPBD scaling, one shared
site for spheres and capsules): a soft wall lets the body sink partway
past TOI, and the residual overlap is an ordinary discrete contact from
the next step on. The default path is untouched by construction (the
branch keeps atilde = 0 on the old code), and the suite confirms it:
the R3/capsule gates run at compliance 0 and never move. Pinned by
`CompliantRewindSinksPastToi`: a single-step zero-g shot lands at TOI
stiff vs at the predicted sunk depth compliant (1e-4 tolerance, effect
0.059 — two orders of separation), plus a 120-step no-cross leg.
Sabotage (rw forced to 1) fails the new test and moves nothing else.
Verified: CPU-filtered suite 536/536, physics/session/command/replay
106/106, asan-ubsan physics 30/30, headless live `all` 186/186,
ctest 6/6. This closes the last structural D1 solver item: compliant
contacts, speculation, sleep, and now compliant speculation.

**Default-on graduation (loop58).** The flag now defaults on: tunneling
is a correctness defect, and the flip was measured before it was made.
Trial flip first: the entire physics suite passed unchanged except the
two gates' off-mode pins — and those failed only because the test
helpers relied on the default (`if (speculative) set(true)`), not
because any simulated outcome moved. Every other scene in the suite is
slow enough that the swept test never fires, so no fingerprint moved.
The helpers now set the flag explicitly both ways (the suite must not
depend on the default), and `set(false)` restores the discrete path
for replay archaeology. blast radius beyond the suite: none measured
— full CPU suite 545/545, asan-ubsan physics+session 29/29, headless
live `all` 186/186 (including the bit-identical session replay).

**Wake-up call (loop62).** Default-on exposed a real defect the gates
could not see: per-body sleep never woke a sleeper through contact. A
300 m/s ball bounced off a sleeping 3-stack whose blocks restored
bit-identical every step — the "rock pile" (measured: pile fingerprint
unchanged through the impact). The speculative edge cannot fix it: a
settled pile is already edged through its own micro-penetrations, so
union without shared fate changes nothing (sabotage-confirmed no-op).
Shared fate (an island sleeps only when EVERY member is slow) was tried
and REVERTED: sleep is also the solver's damping (restore discards each
step's residual jiggle), and all-or-nothing sleep lets one rattling body
veto the restore-damping the other 399 need — the 400-pile settle loop
hit its 1500-step cap. What landed is a per-body contact veto: a slow
body with a contact approaching faster than `veto_thr` stays awake, so
only the bodies actually being hit wake (progressive freezing
preserved). The floor is kick-scaled, and the scale is measured, not
chosen: settled contacts approach at exactly one gravity kick per step
(0.0818 == |g|·dt, every step of a settled stack — the ground-clamped
base at v ~= 0 against the kicked snapshot), so a veto at the rest
criterion fires on every resting contact and nothing ever sleeps
(measured: asleep=0 throughout). `veto_thr = max(8·|g|·dt, threshold)`
clears settling transients with margin while a genuine impact clears it
by orders. Pinned by `FastImpactWakesSleepingStack`: isolated
speculative impact (2.5 m/step travel, no end-sample in the window, so
discrete never fires) moves the pile and halves the ball, and a 5 m/s
discrete impact wakes it too — sabotage (veto disabled) fails both pins
with the pile bit-identical. No re-sleep assert, deliberately: free
spheres have no rolling resistance (documented engine limit), so
scattered debris rolls away with or without sleep; undisturbed settling
is pinned by the pile test (green with the veto) and the veto cannot
latch (scratch reassigned every step). Verified: CPU-filtered suite
535/535, an independent physics/session/editor/control/command/replay
re-run 105/105 (Replay, PhysicsWorld incl. the new test, ControlServer,
Session, Command, EditorSession, ReplayScrubber, CommandRecorder),
asan-ubsan physics 29/29, headless live `all` 186/186. The 52 excluded
legs (timers, input, GUI-class) re-run green too; the GPU legs were not
run this loop (no render path touched).

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
island-formation input; union-find over the pair list is O(pairs·α).
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
identical across compilers/architectures; we only claim same-binary. A
**record-rewind-replay double-step check** built into their samples: before
every step, record state, step, rewind, step again, and compare; a built-in
per-step determinism validator rather than a test-suite-only property.

**Fit/cost.** The double-step validator is cheap and directly applicable: a
debug-mode flag on PhysicsWorld that replays each step once and asserts the
fingerprint; it would have caught every physics determinism regression this
roadmap records, at 2x physics cost in debug only. Adopted for D1 as
`WARPLOOM_PHYSICS_DETERMINISM_CHECK`. Cross-platform float determinism stays
out of scope until a cross-platform product requirement exists (our CI is
lavapipe + this machine's GPU; the *tests* already pin same-binary
determinism).

Fiedler's article validates what we already shipped (fixed dt + accumulator +
interpolation in D2a), with one nuance we should adopt: their interpolation
alpha is `accumulator/dt`; our `sub_frame` equivalent; and our D2a easing
extension is a superset. No change needed; the citation is the receipt.

## R6. What we deliberately do NOT adopt

- **Sequential impulse with warm starting + contact caches** (Jolt/Box2D
  core): warm starting caches impulses across steps keyed on persistent
  manifolds; that is stored state, and stored solver state is exactly what
  our replay contract forbids unless it lives in the snapshot. A warm-start
  cache derived from the previous step's contacts would have to be serialized
  into every checkpoint. Cost > benefit at our current scene sizes. Revisit if
  ragdolls/vehicles land.
- **Mesh/heightfield shapes**: our collision pipeline is analytic
  (sphere-vs-plane/sphere); mesh shapes imply GJK+EPA and a contact-manifold
  builder; a separate project. The roadmap keeps "shapes" honest: next is
  capsule (cheap, useful for characters), not mesh.
- **Blade-style tracking-free RHI** (Malyshau, arXiv:2607.26506; global
  pass-boundary barriers without per-resource tracking, measured 29-32% GPU
  span reduction removing redundant barriers): fascinating for the render
  layer, and directly relevant to the bloom-toggle rebuild problem (keeping
  images in GENERAL would have made the toggle a pure re-record). But it is a
  render-layer architecture change (wgpu/Blade lineage), and our compose graph
  already computes per-resource barriers deterministically. Recorded as an R5
  direction with the citation, not adopted now.
