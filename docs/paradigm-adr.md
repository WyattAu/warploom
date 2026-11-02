# Paradigm trial ADRs (551-850)

Decision records for the funnel in `docs/paradigm-trials.md`. One ADR per
verdict, numbered in order. Rejections require a **measured** reason (spike
numbers, sabotage, or a cited constraint) -- never taste. Adoptions point
at the landed commit + proof.

## ADR-000: trial process (accepted)

Trials run smallest-spike-first: a unit test, probe, or capture A/B
against the current implementation. Verdict criteria per trial:

- **Adopt** only on: correctness parity (or proven superiority) + measured
  win or new capability + 0 validation diagnostics + full suite green.
- **Reject** on: measured loss/parity-with-cost, failed parity, or a
  demonstrated no-op (sabotage that does not fail the check fails the
  trial). Record the numbers that killed it.
- A step that cannot be verified does not land; revert and record why.

Status words live in the trials table; the reasoning lives here.

## ADR-001: out-of-scope classes (accepted)

The following never enter the trial queue, because no consuming subsystem
exists in this repo (engine + core sim + movie tool + future modeler),
or a hard constraint forbids them. Review trigger: a subsystem appears,
or the constraint lifts -- then the item re-tiers to P2, not straight to
trial.

- **Fluids/CFD/all multiphase continua** (LIX 641-650, LXII 671-680,
  LXXV 801-810): no fluid subsystem; nothing to measure against.
- **Silicon primitives** (LXI 661-670, incl. 561 fixed-function Z-buffer
  removal, 567 rasterizer removal): we target Vulkan on vendor hardware;
  we do not design silicon.
- **Robotics** (LXIII 681-690): no robot subsystem.
- **Geometric deep learning** (LXIV 691-700): the dependency budget is
  CPM + GoogleTest; no ML framework enters the tree.
- **Audio/acoustics/DSP** (LXV 701-710, LXXVII 821-830, 812 phononic):
  no audio engine; the movie tool is silent by design.
- **Geomechanics/terrain physics** (LXVI 711-720): terrain is static
  geometry; no soil/erosion subsystem.
- **Tissue/biomechanics** (LXVIII 731-740), **molecular dynamics**
  (LXIX 741-750): no bio/molecular subsystems.
- **Relativistic/non-Euclidean rendering** (LXX 751-760): the renderer is
  Euclidean; straight-line rays are a product requirement (ray tracing
  pipeline), not a limitation to remove.
- **Multiplayer netcode** (610), **NeRF training** (571): no netcode, no
  training infra (dependency budget + determinism contract).
- **Manufacturing/CAM** (763-766, 769, 770, most of LXXVI 811-820):
  the modeler models; it does not emit toolpaths.
- **Sensors/SLAM reconstruction** (771-774, 776-780, 843, 844, 847):
  no camera/sensor input subsystem; 775 stays P2 for tool calibration.
- **Display hardware** (726, 728, 730): panel/optics manufacturing.

## ADR-002: trial #554 Plucker intersectors (rejected as CPU replacement)

Spike `PlueckerTrial` in `tests/unit/test_pluecker_intersector.cpp`
 pits a Klein-quadric Plucker ray-triangle test against the incumbent
 Moller-Trumbore on this machine (scalar C++, -O2, deterministic seed):

- Parity: 20000 aimed rays, 20000/20000 hit agreement, 0 disagreements,
  max |dt| 1.165e-12, max barycentric error 2.087e-13. Full parity.
- Miss parity: 1000 random rays (500 aimed, 500 wild), mt == pl == 850
  hits, 0 disagreements.
- Timing: MT 258.10 ns/ray vs Plucker 347.69 ns/ray (10M-ray loop,
  anti-DCE sinks equal at 10000000.000000) -- Plucker is **1.35x slower**.

Verdict: **rejected** as a CPU-side Moller-Trumbore replacement. The
 Plucker setup (ray line coordinates + three edge inner products) costs
 more than it saves on a scalar path where MT's early-outs already win;
 parity-with-cost is a reject per ADR-000. Sabotage sensitivity holds by
 construction: the parity test compares two independent implementations,
 so breaking either side fails agreement (no single shared code path to
 silently no-op).

Explicitly untested and not claimed: a branchless-Plucker variant inside
 a GPU shader (closest-hit or compute cull), where divergence rather
 than scalar latency dominates. The engine's ray-triangle work currently
 rides hardware BLAS, not shader MT, so that variant has no incumbent to
 beat yet -- it may re-enter as a P2 if a shader-side intersector ever
 exists. Zero validation diagnostics; suite green at verdict time.

## ADR-003: trial #831 exact geometric predicates (adopted as utility)

Spike `ExactPredicatesTrial` in `tests/unit/test_exact_predicates.cpp`
 pits naive float/double orient2d against the adaptive
 (double filter + Shewchuk-style bound + exact-int64 fallback) landed in
 `modules/core/include/warploom/core/exact_predicates.hpp`, with ground
 truth from integer arithmetic on intended coordinates (no float in the
 reference path). Bezout det=-1 triples, deterministic seeds:

- Random small-int (20000 triples, 40000 comparisons): 0 disagreements --
  generic data needs nothing; the baseline, not the argument.
- Bezout ~1e12 (coordinates the engine will actually see): float wrong
  1999/2000, double wrong 0/2000, adaptive wrong 0/2000. Float is
  unusable on near-degenerate data; the fallback earns its keep.
- Bezout ~2^54: double wrong 500/500 -- even double has a cliff. The
  helper contract is therefore integer-valued doubles within +-2^53 ->
  exact sign; outside it the filter refuses (500/500 fallbacks, 0 wrong
  signs out -- never invents a sign, returns 0 degenerate instead).
- Timing: float 8.9ns, double 8.7ns, adaptive 15.7ns per predicate
  (~1.8x double on the filter leg; the exact fallback fires only on
  degeneracy, rare by construction).

Verdict: **adopted** as a header-only utility + 4 guarding tests. No live
 consumer exists yet (software rasterizer uses edge functions, not sign
 predicates), so this is capability staging, not a migration: the first
 consumer is expected to be triangulation (837) or curvature-preserving
 decimation (800), both of which need orientation tests that cannot flip
 under near-degeneracy. Sabotage posture: the Bezout tests compare the
 header against integer ground truth outside the float path, so breaking
 either the filter bound or the fallback fails agreement. Full suite +
 validation re-run at landing; numbers above are from this codebase
 (RTX 2060 host for GPU legs, scalar C++ -O2 for the predicate legs),
 not from the proposal text.

## ADR-004: trial #608 golden-ratio spatial hashing (adopted, live)

Spike `GoldenHashGridTrial` in `tests/unit/test_golden_hash_grid.cpp`
 mirrors the engine broadphase (same cell size, same i-ascending
 27-neighbourhood walk) with two cell stores: the incumbent
 unordered_map on int64 triples vs a flat open-addressing table keyed by
 a golden-ratio (2^64/phi MMIX) multiplicative mix with linear probing.
 Pair emission is textually identical, so parity is checked twice: exact
 emission ORDER first (production's historical contract), then the sorted
 set. Deterministic scenes (fixed-seed xorshift), three distributions:

- Uniform 1k/4k/16k: identical order + set at all scales (the N=4000
  scale that caught the first broadphase attempt); challenger 1.48-1.53x
  faster (run-to-run range 1.2-1.9x across scenes, always challenger
  ahead: cluster 4k 1.87x, lattice 4k 1.68x).
- Probes stay flat: max 13 at 16k bodies, mean 1.0-1.3 -- no pile-up
  degradation, which is what the quasi-random projection promises and
  what the numbers confirm.

Verdict: **adopted into production** (`PhysicsWorld::BroadphaseGrid`
 in `modules/physics/include/warploom/physics/physics_world.hpp`):
 unordered_map + CellHash removed, same occupancy, same walk, trailing
 sort untouched -- so contact output is provably unchanged while the
 build+query leg runs ~1.5x faster. Scratch stays member-resident
 (slots + member vectors + used-list persist; clear_frame() only resets
 flags), preserving the steady-state zero-allocation contract. All 28
 physics tests green (they include the broadphase-vs-all-pairs
 comparison), spike order-parity holds against the removed incumbent.
 Sabotage posture: the spike compares two independent stores, so a
 broken mix or probe fails set agreement; the production swap is
 covered by the existing broadphase-equivalence tests, which failed in
 the past for exactly this class of change.

## ADR-005: trial #611 dual-quaternion SE(3) blend skinning (adopted as utility)

Spike `Se3SkinningTrial` in `tests/unit/test_se3_skinning.cpp` pits
 the incumbent LBS shader math (linear matrix blend, as in
 `skinned_scene.vert` / `pbr_gpu_driven_skinned.vert` /
 `shadow_skinned.vert`) against a dual-quaternion SE(3) blend landed in
 `modules/core/include/warploom/core/dual_quat_skinning.hpp`
 (rigid bone -> unit dual quat, weighted sum with antipodal correction,
 full normalize + orthogonalize, rigid extract, transform). Two probes,
 deterministic, this machine (scalar C++, -O2):

- Twist 180 deg about Y, 0.5/0.5 weights, radius-0.2 vertex:
  LBS radius 0.000000 (collapses to the axis -- the candy-wrapper),
  DLB radius 0.200000 (exact preservation, rotated 90 deg as expected).
- Bend 90 deg, same weights: LBS radius 0.141421 (= cos45 * 0.2, a 29%
  shrink the linear average always pays), DLB radius 0.200000.
- Timing (200k-iter loop, anti-DCE sink): LBS ~77-79ns vs DLB ~89-90ns
  per 2-bone blend+transform -- DLB is **~1.15x LBS**, inside the 3x
  guardrail.

Verdict: **adopted as a header-only utility + 3 guarding tests**.
 No live shader consumer yet: all three skinning shaders still do LBS,
 and the bone SSBO stays mat4. A GPU migration (8-float palette +
 in-shader normalize) is staged, not landed -- same staging pattern as
 ADR-003. The first consumer is expected to be the mannequin/animation
 path when a 180-degree twist pose exists to prove the pixels against.
 Sabotage posture by construction: the twist test pins BOTH sides
 (LBS == collapse at 0.0 AND DLB == 0.2), so breaking either the LBS
 baseline or the DLB normalize/antipodal leg fails; the bend test pins
 the LBS 0.141421 shrink, so a "simplification" that equates the two
 paths fails too. Spike files warning-clean; pre-existing
 double-promotion warnings in test_path_tracing_real.cpp untouched.
 Full suite at landing: 713/713 green under validation. Two log lines
 pre-exist on this machine across runs and are untouched by this change
 (CPU-only header + test, zero Vulkan touch): HiZGraph's sabotage leg
 logs `image index out of range` by design, and
 `ComposeDepthReductionIsValidationClean` prints a 4-object
 vkDestroyDevice note while still passing -- both were already present
 in the pre-change runs.

## ADR-006: trial #800 curvature-preserving decimation (adopted as utility)

Spike `QuadricDecimationTrial` in `tests/unit/test_quadric_decimation.cpp`
 pits midpoint-placement QEM (Garland & Heckbert) landed in
 `modules/core/include/warploom/core/quadric_decimation.hpp` against a
 curvature-blind baseline sharing the same collapse machinery (boundary
 vertices locked, midpoint placement, one interior edge per step so both
 meet the budget at identical triangle counts). Roof-ridge heightfield
 probe (z = 1-|x|, 9x9 grid, 128 tris -> 64), deterministic, this
 machine (scalar C++, -O2):

- QEM volume loss **0.0000** (all 32 collapses zero-cost, ridge
  untouched) vs blind volume loss **0.1339** -- same 64 tris, so the
  comparison is exact, not budget-confused.
- Timing (10-iter loop, anti-DCE sink): blind ~3.3ms vs QEM ~5.0ms per
  128->64 decimation -- QEM is **~1.5x the baseline**, inside the 3x
  guardrail (the per-iteration quadric re-accumulation is the cost).
- Sabotage-validated, not assumed: negating the QEM cost (anti-QEM,
  picks the WORST edge each step) drives the loss to 0.2188 and fails
  both guardrails; removing the boundary lock (the first-version bug,
  which ate the patch outline in index order) measured 0.239 and fails
  too. A test that passes a broken QEM tests nothing; this one fails it.

Two earlier probes were tried and REJECTED (recorded so nobody repeats
 them): a paraboloid mesh (constant second derivative, so QEM costs are
 near-uniform -- the test measured noise, blind won 0.21 vs 0.31) and
 shortest-edge as the baseline (edge length implicitly encodes
 curvature, so it protects features for the wrong reason -- it won 0.07
 vs 0.24). The blind fixed-seed-hash baseline plays no such favorites.

Verdict: **adopted as a header-only utility + 3 guarding tests**.
 No live consumer yet: LOD selection exists (`lod_select.comp`) but no
 pipeline generates lower-LOD meshes -- that generator is the staged
 consumer (asset-conditioning path), same staging pattern as ADR-003
 and ADR-005. Scope is stated in the header: midpoint placement (not
 optimal-position QEM), boundary locked, no attributes, no manifold
 repair. Spike files warning-clean.

## ADR-007: trial #602 homological multi-scale slicing (adopted as guard)

Spike `MultiscaleTopologyTrial` in
 `tests/unit/test_multiscale_topology.cpp` tests the homological
 prerequisite of the 602 claim: a multi-scale chain must preserve
 topology across scales. The full claim (no LOD pop-in via continuous
 slicing) needs render integration and stays staged; what is adopted is
 the acceptance guard the staged generator will use --
 `euler_chi` in `modules/core/include/warploom/core/mesh_topology.hpp`
 (chi = V-E+F on the live index soup, dead verts/degenerate tris
 excluded, ordered sets so deterministic). Closed-torus probe
 (24x16, 768 tris -> 192, no boundary so every edge is eligible),
 deterministic, this machine (scalar C++, -O2):

- QEM chi **0** (handle preserved) vs blind chi **34** (handle pinched
  and fragmented) -- identical 192 tris, so the outcome differs only
  by cost function, not budget.
- Sabotage-validated: negating the QEM cost drives chi to 1 (torus
  pinched sphere-like) and fails the guard; the blind baseline IS the
  broken version, so the test fails a broken QEM by construction.
- Timing: ~1.8s for the 768->192 pair (288 single-edge steps with
  per-iteration quadric re-accumulation); acceptable for an offline
  asset-conditioning guard, not for runtime -- stated, not hidden.

Consumer investigation (recorded, same session): there is NO
 asset-conditioning path in-tree (gltf_importer emits meshes directly;
 `lod_meshes` is hand-populated in tests), and the QEM header carries
 positions only -- production wiring needs attribute-aware collapse
 (uv/normal/skin streams) first. So the generator itself stays staged
 behind that extension; what lands now is its acceptance guard plus
 the measured proof that the guard discriminates. Forcing a
 positions-only wiring into the importer would silently corrupt
 attributes -- rejected on that ground, not on effort.

Verdict: **adopted as a header-only guard + 2 guarding tests**.
 Spike files warning-clean.
