# Warploom long-term roadmap: one multipurpose technical engine

Status: living plan. `docs/roadmap.md` remains the sprint-level record;
this file is the multi-year direction. It commits to scope and order, not
to dates. Every item inherits the project's verification discipline
(roadmap "Verification discipline", AGENTS.md "Verification baseline"):
four-leg matrix green, 0 validation diagnostics, 0 leaks, live hardware
proof for anything protocol- or viewport-facing, sabotage-checked guards,
`docs/rendering-status.md` updated in the same commit.

## 1. Goal, stated once

Warploom is a **multipurpose technical engine** for physics simulations,
games, movies, and CAD — no domain is second priority. The shared core
(determinism, data model, frame graph, physics solver, timeline) carries
all four; domain layers stay thin and move out of this repo when they need
a substantial consumer (AGENTS.md dogfooding rule).

Broad v1 means: **live path tracing with an in-tree denoiser, fuller
physics (rotation, joints, more shapes, sleeping), a real content
pipeline (glTF + compressed textures on GPU), and a usable editor
(script nodes, subgraph UI, asset browser, camera/light/render tracks) —
at performance budgets, not just correctness.**

Performance/scale is the binding constraint for the next 6–12 months:
frame cost, scene sizes, and GPU-driven paths gate what "broad" can mean.
Correctness work stays sabotage-checked and docs-honest; it does not pause
for perf, but perf rewrites must still prove equivalence (pixel A/B,
fingerprint stability, byte-identical replays).

## 2. Principles (non-negotiable, from experience)

- **Determinism is a product feature.** Anything reachable from a tick
  reads no wall-clock, iterates no unordered container, depends on no
  scheduling. A replay break is a regression even when every test passes.
  `-ffp-contract=off`; no `-march=native`/platform libm on tick paths.
- **Dependency budget: one package manager (CPM) + one library
  (GoogleTest).** OIDN and friends are excluded by this budget (see
  comparison-matrix §5); the denoiser is in-tree or it does not ship.
- **Delete is a feature.** Dead shims, superseded plans, draft docs go.
- **Scale the verification.** A check at one size proves one size
  (broadphase 1k-vs-4k lesson). A claim on one compiler proves one
  compiler (clang-zero vs GCC-203 lesson).
- **Never let a test pass by accident.** Sabotage both directions or the
  guard is decoration.
- **Technique needs provenance.** ACM/SIGGRAPH/IEEE > vendor docs >
  arXiv-as-preprint. Record source, claim, novelty, fit/cost, and what
  was measured here (docs/research/).
- **Mature-field rule.** Graphics, physics, and video are mature: survey
  Godot/Cycles/OCCT/Karma/Unreal/Unity (already started in
  docs/comparison-matrix.md + docs/research/) and WyattAu's own repos for
  domain patterns before inventing. Inspiration is expected; novelty
  claims must say what is standard, what combination is unusual, and what
  (if anything) is actually new.

## 3. Where we stand (measured, not claimed)

- **0.1 gate (roadmap):** clone-and-build met + CI-enforced; modules real;
  demo story (record/replay/scrub/save-load) met for render/document
  timeline; signature PT feature met as pause-to-converge (forward→pt→
  forward over socket, noise-floor convergence, flat linear accumulation,
  0.9994 capture correlation).
- **Recently landed:** shared-BLAS instancing (`mesh_index`, bit-identical
  proof), true Duff ONB + Faceforward normals with a committed
  normal-visualization orientation proof, clip strip in the viewport with
  live drag + undo/redo proof, KTX2 BC1/BC3 decode in-tree.
- **Known gaps:** PT is a static snapshot (app-scene geometry landed --
  mannequin/city/sponza traced, actors frozen mid-stride, skinned parts in
  bind pose placed by model*bone; no live/deforming PT); PT + bloom layers
  since 2026-11-01 (bright-pass over the accumulation sum x 1/n, additive
  post-curve like raster); no denoiser (deliberate v1 deferral, decision recorded);
  no adaptive sampling; transient planner (R6a) unwired — R6b unblocked
  by Loop 13 graph-derived lifetimes but still to wire; physics has no
  rotation/joints, capsule/sleeping/velocity-update partially open;
  scrub restores document, not physics; sub-frame fraction in the protocol
  since 2026-11-03 (T4b: parsed + validated, forwarded by both hosts,
  recorded, replayed deterministically); no hardware CI runner (RT tests
  skip on lavapipe); WASM/
  Wayland/adoption (E6) open.

## 4. Track plan (parallel tracks, shared substrate first)

Tracks run in parallel but merge in order: **T0 → T1/T2 → T3–T7 → T8.**
Nothing in T3–T7 may break T0's contracts (determinism, verification).

### T0. Substrate + performance budgets (first, unlocks everything)

- [x] **R6b: allocate/alias Vulkan images from the plan.** Loop 13
  derived lifetimes; the compose chain now derives the superset plan,
  materializes it in `VulkanTransientStore`, and adopts the images into
  the render-pass shells — no create()/bind_image() names an HDR, bloom
  or depth image anymore. Exit met: 4-of-10 elision holds on-device
  (`TransientStore.ElidesDisjointPairsOnDevice`, 10 images over 6 owner
  allocations); history never aliased (`FuzzNeverAliasesHistoryOnDevice`
  plus the Lean model: `planKind_history_dedicated`,
  `planKind_aliased_sound`, `planKind_owner_earlier` — re-elaborated
  clean under the pinned toolchain 2026-11-01, byte-identical olean);
  0 diagnostics via the tests' own validation-count assertions; pixel
  A/B pinned by the compose golden hashes in the 693-test green run.
- [x] **Scene pass as a graph node (B2 seam).** The scene pass records
  as a node in the same compile as the compose stages (T0 unified
  scene+compose frame graph): no hand-written barrier, no
  `initial_layout` seeding on the compose path. Covered by the compose
  golden-hash tests and the full 693-test green run on the unified path.
- [x] **Dynamic rendering for fullscreen passes (Blade stage 1)** so
  passes un-bake; prerequisite for GENERAL-layout PT/bloom layering work.
  Stage 0 DONE: the device negotiates `dynamicRendering` (1.3 core, no
  extension) behind `has_dynamic_rendering()`, proven by
  `DynamicRendering.ClearOnlyPassWritesAttachment` (blue pre-fill +
  CLEAR-load red read back bit-exact, sabotage-checked: DONT_CARE fails
  it; 0 validation diagnostics).  Stage 1 DONE 2026-11-01 (RTX 2060): the
  compose bloom downsample records dynamically when requested AND
  advertised (`ComposeChainConfig::enable_dynamic_rendering`, negotiated
  in `ensure()`, baked fallback otherwise). Stage 2 DONE the same day:
  the tonemap goes dynamic too (see §8.1). Proven by
  `DynamicRendering.ComposeBloomMigratesToDynamic`
  (routing + node-list gate + sabotage + mixed-graph execute, 0
  diagnostics) and `DynamicRendering.UnifiedFrameWithDynamicBloomRecordsClean`
  (production unified frame, golden-hash parity with the baked path, 0
  diagnostics). Stage 3 DONE 2026-11-03 (RTX 2060): the scene node itself
  goes dynamic (color + depth; `GraphDynamicPass.has_depth`, gate +
  derivation cover the dynamic depth, explicit `set_dynamic_scene_pipeline`
  opt-in so a baked pipeline can never land in a dynamic node). Proven by
  `DynamicRendering.UnifiedFrameWithDynamicSceneMatchesGolden` (same
  canonical hash 12508388375569283214 as the baked golden, 0 diagnostics;
  control is structural -- the sabotage direction fails with
  VUID-vkCmdDraw-renderPass-02684) and
  `DynamicRendering.DynamicSceneDepthCoveredByGate` (forged dynamic depth
  fails the R6b gate). Full suite 732/732. Remaining passes (PT blit,
  sky, UI) migrate in later stages, each with its own re-proof
  (see loop27).
- [x] **Perf budgets as gates (T0).** Per-config frame budgets
  (default, bloom, nohdr, gd, rt, node, pt, scale10k, phys4k, phys8k) in
  docs/perf-budgets.json from real GPU timing; `analyze_telemetry.py
  --check-budgets <cfg>:docs/perf-budgets.json` is the gate (exit 0 =
  budgets + correctness checks green). Measured 2026-10-31, all ten
  analyzer runs green, 0 validation diagnostics; budgets carry ~1.5-2x
  headroom and record the measure-time load, so a canary firing on a
  loaded machine means re-run at low load before blaming the change.
- [x] **Async-compute evaluation.** MEASURED, verdict ADOPT
  (`AsyncComputeSpike.ReduceOverlapsGraphics`, RTX 2060, under
  validation): production `depth_reduce` on the COMPUTE-only queue while
  graphics renders beats sequential-on-graphics by 36-54% across 4 runs
  (delta -3.3 to -4.7 ms; the ~4 ms reduce hides almost entirely behind
  the render). Pyramid bit-identical across queues and equal to
  floatBitsToUint(0.5) everywhere; 0 diagnostics. The in-renderer
  migration is staged (loop28) -- the frame-loop surgery does not ride
  along with the spike. Step 1 DONE 2026-11-02 (RTX 2060): H-Z images
  (pyramids, compose HDR depth, swapchain depth) are CONCURRENT over
  {graphics, compute} when a dedicated COMPUTE-only family exists
  (`VulkanContext::cross_queue_sharing`, one validity rule in
  `ImageSharing::concurrent_ok`), exclusive otherwise. No behaviour
  change by construction; proven by
  `VulkanHardware.ConcurrentSharingMatchesExclusive` (concurrent pyramid
  reduces bit-identically to exclusive, absolute 0.5 check both legs,
  sabotage-checked, 0 diagnostics) with the compose+H-Z and golden-hash
  tests green. Steps 2-3 DONE 2026-11-02/03 (RTX 2060): persisted
  per-frame depth snapshots (bit-identical to live depth,
  `HiZGraph.DepthSnapshotsMatchLiveDepth`) and compute-queue reduce + pack
  (`compute_reduce_hiz_snapshot`, bit-identical to the graphics result,
  `HiZGraph.ComputeReduceMatchesGraphicsReduce`, 0 diagnostics). Remaining
  for the full migration: pipeline the compute leg a frame behind graphics
  with the timeline handoff instead of CPU-waiting (overlap, not just
  correctness) — plus Blade's later stages (sky, UI passes).

### T1. Raster core (one render path, then scale)

- [ ] Occlusion-heavy demo content (engine needs no change; content
  question) to show live savings.
- [ ] LOD mesh **generation** (selection exists; nothing generates).
- [ ] CSM evaluation only when a scene needs it; single 2048² map until
  then. SSAO/SSR/TAA stay out unless a consumer proves need.

### T2. Ray tracing / path tracing (usability → live)

- [x] **App-scene PT geometry.** DONE: setup_path_tracing snapshots the
  live scene list (same matching as the RT shadow TLAS walk: cube/ground by
  mesh, mannequin parts via model*dominant-joint bone, city/sponza identity
  over world-baked triangles) instead of the hardcoded cube pair. Startup
  builds through the control-path request on the first frame with a scene
  (the list is per-frame; an eager build snapshots emptiness). Proven by
  `app_scene_general_transforms_contribute` (extra mesh through a general
  transform contributes + deterministic) and live on the RTX 2060:
  mannequin scene 10 objects/11 meshes and city 76/60, 0 VUIDs, captures
  with 0% unwritten converging monotonically. Honest limits: static
  snapshot (animation smears an accumulator), skinned parts are rigid
  placements not vertex skinning, bloom does not layer on PT. Exposed two
  engine bugs, both fixed (see R6b note in roadmap.md): the verify gate's
  absolute-index subset check refused PT-with-bloom-off, and the per-frame
  external descriptor write was VUID-03047 with frames in flight.
- [ ] **RT sub-toggles (R2) + GPU-driven skinned scenes (R3).**
- [x] **PT + bloom layering.** DONE 2026-11-01 (RTX 2060): the bloom
  bright-pass reads the external accumulation in GENERAL and divides by
  the sample count BEFORE the Karis threshold (new `bloom_downsample_pt`
  shader + pipeline, same 0.8 threshold as raster), so the tonemap adds
  the same post-curve bloom term on both paths. Pinned by
  `VulkanHardware.PathTracedFrameLayersBloom` (PT+bloom frame differs from
  PT-only AND the mean rises; sabotage-forcing the raster path reproduces
  the old black-slot frame and fails it; 0 diagnostics). Raster path
  untouched (own shader, own sets, golden hashes green).
- [x] **Live PT stepping discipline** DONE 2026-11-03 (RTX 2060): a camera
  move auto-restarts the sequence (both images, through the sample-target
  hold); identical re-uploads don't. Pinned by
  `path_tracing_real.camera_move_restarts_sequence` (counts, forced-vs-
  explicit bit-identity, sabotage both directions) -- the pose-smear is
  unrepresentable through the API. Recorded finding: the replay contract
  is per tracer instance (cross-tracer bit identity does NOT hold: two
  identically-configured tracers differ on 132/4096 floats -- per-tracer
  AS builds resolve edge ties differently).
- [x] **Live PT spikes** MEASURED 2026-11-03 (RTX 2060, loop29): adaptive
  justified (top 5% of pixels hold 92% of variance -- uniform spending
  wastes ~90% of rounds; pinned by `variance_concentration_spike`) and
  variance-only a-trous buys 1.67x MSE reduction at 4spp with no G-buffer
  (corrected 2026-11-04 from the 2.68x research-spike number, which the
  committed test's constant sky could not reproduce; `denoise_variance_spike`,
  bit-identical re-dispatch pinned -- the temporal-history constraint holds
  by construction). Multi-iteration CLOSED 2026-11-05 (RTX 2060, loop29
  section 4): K=2 best 0.94x, K=3 best 0.48x -- variance-only iteration
  diverges, residual is edge-dominated, do NOT build a variance-only filter
  loop. Next, in order: adaptive build (masked re-trace + per-pixel
  counts), then the G-buffer (normals/albedo/depth outputs) or a
  per-iteration variance update -- not more iterations of the variance-only
  filter. OIDN stays excluded by the dependency budget.
- [ ] Path guiding / Light Tree / MBVH only on measured scene-size need.

### T3. Physics (depth without breaking replay)

- Order: velocity-from-position update → island sleeping (union-find over
  frozen contact graph) → capsule → **rotation** → **joints**, each a
  design arc (see docs/research/physics-solver.md + loops 2/3).
- Keep: frozen-snapshot detection (bit-identical pair sets), substeps +
  iterations as separate knobs, compliance opt-in (default 0 =
  bit-identical replays), determinism double-step validator.
- Broadphase trigger: Morton BVH only on the recorded numeric trigger;
  grid + all-pairs fallback until then.
- Perf: 4k/8k agreement + speedup pinned (today 2.10x/2.89x/2.92x at
  1k/4k/8k for the grid solver).

### T4. Data model + simulation

- ECS stays sparse-set until archetypes earn it on measurement.
- **Scrub restores physics** (checkpoints capture document only today).
- **Sub-frame fraction in the protocol: DONE 2026-11-03** (T4b: `frac`
  key on Step, validated at the boundary, forwarded by both hosts on the
  span's last tick, recorded in `n`, replayed deterministically through
  the closing-hash check). Next: concurrent record-and-play.

### T5. Movies / timeline

- Camera/light/render-property tracks; concurrent record + play arms.
- Clip-strip chrome as needed (context menu, zoom/pan UI — window is
  host-set today); block-move already live-proven.
- Capture pipeline follows PT usability (already 0.9994 correlation).

### T6. Games / editor

- G4 script node DONE (engine + UI rows + viewport wiring); G5 asset
  browser DONE (engine scan + viewport panel + click-load); G2 subgraph
  copy/paste DONE (engine + protocol + Ctrl+C/V wiring, rollback-tested);
  scene management (no spec yet).
- Input already in-tree; no new backends without a consumer.

### T7. CAD / modelling

- OCCT lesson first: **face holds a list of triangulations + one
  active** (render vs analysis resolution). Apply to our one-mesh-per-
  object model before any kernel talk.
- Parametric bridge + LOD/decimation generation (shared with T1).
- Full BRep kernel: spike only, out-of-tree on first need (dogfooding
  rule). No silent "CAD support" claims from a mesher.

### T8. Productization (proves the engine from outside)

- [ ] **E3 hardware CI runner** — RT tests currently skip on lavapipe;
  the best subsystem has the least protection.
- [ ] E6: WASM leg, native Wayland, FetchContent/vcpkg adoption, docs
  rebrand, repo rename.
- [ ] Dogfooding splits per AGENTS.md: viewport split as the natural
  first; then a game consumer (WyattAu/warploom-game pattern exists),
  and a movie/CAD consumer only when the capability needs one. Each:
  separate repo + CI, public API only, its existence is the evidence.

## 5. v1 definition (broad, testable)

v1 ships when **all** hold, each with its machine-checked proof:

1. Pause-to-converge **and** live PT with in-tree denoiser + adaptive
   sampling, on app scenes (not cubes-only), at published frame budgets.
2. Deterministic tick + replay + scrub **including physics**, sub-frame
   playback reproducible from the protocol.
3. Timeline: interpolated/eased tracks incl. camera/light/render,
   concurrent arms where claimed, clip strip fully usable.
4. Physics: spheres + capsules, rotation + joints, sleeping, broadphase
   at scale, validator clean.
5. Content pipeline: glTF + KTX2 (GPU-native upload on measured VRAM
   trigger) + asset browser.
6. One render path: scene-through-compose on the graph, transient
   aliasing on-device, 0 diagnostics/leaks, full preset matrix green.
7. Hardware CI runner so RT is regression-protected; packaging/adoption
   (E6) done enough for an external consumer to build on it.

Explicitly **out** for v1: multiplayer/rollback, audio backends,
mobile, ABI stability, MBVH/path-guiding/Light Tree (until scene sizes
demand), OIDN (budget-excluded), BRep kernel (spike only).

## 6. Inspiration queue (survey before inventing)

- Rendering/graphs: Unreal RDG transient allocator, Unity RenderGraph
  descriptor-reuse (know what we beat), Blade tracking-free RHI, idTech
  persistent resources, Cycles/Karma sampling + Pixel Oracle, Lumen
  radiance-cache (why no-denoiser is a defensible architecture).
- Physics: Jolt island rebuild + determinism checkbox, Box2D v3
  speculative contacts, Rapier/Godot determinism claims, XPBD/Small
  Steps/MGPBD lineage in docs/research/physics-solver.md.
- CAD: OCCT triangulation lists + `Poly_MeshPurpose`, Nanite cluster
  LOD (know the shape before claiming any of it).
- WyattAu repos: mine domain patterns (gameplay, tooling, asset
  workflows) for T5–T7 before designing new UI/API. Proposals that skip
  this survey carry the burden of proof.

## 7. Distance to v1 (measured 2026-11-01, 697-test green run)

Against the §5 definition, criterion by criterion:

1. **PT live + denoiser + adaptive, on app scenes, at budgets: FAR.**
   Pause-to-converge on app scenes is real; PT + bloom layers. Missing:
   per-frame live stepping discipline, adaptive sampling, and the whole
   in-tree denoiser (à-trous/SVGF spike → implementation is the single
   biggest item left). Skinned/deforming PT is rigid-placement only.
2. **Deterministic tick + replay + scrub incl. physics: NEAR.** T4a DONE
   2026-11-03: checkpoints (ring + file) carry the full solver state;
   scrub restores physics bit-identically; re-apply re-simulates with the
   header dt (pause-tracked) and the closing check covers physics. T4b DONE
   2026-11-03: the sub-frame fraction travels the protocol end to end
   (parse + validate, host forward, record, deterministic replay).
3. **Timeline tracks + concurrent arms + clip strip: MEDIUM.** Strip
   chrome + block-move live-proven; camera/light/render tracks and
   concurrent record + play are specless.
4. **Physics spheres + capsules + rotation + joints + sleeping: MEDIUM-FAR.**
   Grid broadphase pinned with speedups; rotation and joints are each a
   design arc (loops 2/3 researched, not built); capsule/sleeping partial.
5. **Content pipeline: NEAR.** glTF + KTX2 decode + asset browser done;
   only GPU-native upload on the VRAM trigger remains.
6. **One render path, aliasing on-device, matrix green: NEAR-DONE.** R6b
   + B2 + Blade stage 1 landed; remaining are Blade later stages and the
   async-compute decision.
7. **Hardware CI + E6: NEAR in code, FAR in access.** Nothing to invent;
   needs a machine with an NVIDIA GPU on the CI path, then packaging.

## 8. Execution order (next stages, smallest unblocking step first)

1. **Blade stage 2 (T0).** DONE 2026-11-01 (RTX 2060): the tonemap goes
   dynamic whenever the framing is negotiated AND the caller supplies the
   present image/view -- threaded per call (the renderer resolves the
   swapchain image internally; headless callers pass their stand-in
   explicitly; null keeps the baked path), not a setter, so no per-frame
   descriptor writes and no state to go stale across resizes. The present
   image is modeled as caller-owned history in the skeleton/plan/gate
   (kKindPresent), exactly like the external source. Pinned by golden
   parity in `DynamicRendering.UnifiedFrameWithDynamicBloomRecordsClean`
   (same canonical hash as the baked golden, 12508388375569283214) plus
   a wrong-final-layout sabotage (VUIDs + hash failure, reverted). Baked
   path untouched: all golden hashes green.
2. **Async-compute spike (T0).** DONE -- measured win, ADOPT recorded
   (spike + loop28): 36-54% on the reduce leg, determinism intact.
   Migration steps 1-3 DONE 2026-11-02/03: CONCURRENT images, persisted
   depth snapshots, compute-queue reduce + pack (bit-identical, 0
   diagnostics). Remaining: pipeline the compute leg (overlap, not just
   correctness) + Blade later stages (sky, UI).
3. **Scrub restores physics (T4a) + sub-frame fraction (T4b).** BOTH DONE
   2026-11-03: checkpoint scrub reproduces physics bit-identically (ring
   + file, re-simulation with header dt, closing check covers physics);
   the fraction replays deterministically end to end (wire key, host
   forward, record, closing-hash-verified re-apply).
4. **Rotation, then joints, then capsule + sleeping (T3).** One design
   arc each, validator clean before the next starts.
5. **Live PT (T2).** Stepping discipline DONE; adaptive + denoiser SPIKES
   measured (loop29) -- verdicts: adaptive justified (top5% = 92%),
   variance-only a-trous = 1.67x at 4spp (corrected 2026-11-04),
   temporal-history constraint holds by construction; multi-iteration
   CLOSED 2026-11-05 (K=2 0.94x, K=3 0.48x -- diverges, do not build).
   Remaining, in order: adaptive build, then the G-buffer (or a
   per-iteration variance update) on the edge-dominated residual -- never
   more variance-only iterations. Exit: live PT on app scenes at the
   published budgets.
6. **Camera/light/render tracks + concurrent arms (T5).** Spec first
   (WyattAu repo survey per §6), then chrome.
7. **E3 hardware runner, then E6 packaging (T8).** RT is the best-tested
   and least-protected subsystem; that inversion ends here.
8. **T1/T6/T7 remainder on consumer need.** LOD generation, CSM-on-need,
   scene management, parametric bridge: specced when a consumer proves
   the need, not before.

Then re-evaluate per the loop: what unblocked what, what it revealed.
