# Research index

Status: research notes produced by the 2026-10-08 research loop (10 domains,
sources verified via arXiv API / vendor docs / engine architecture pages /
frame-study articles) and the 2026-10-09 second pass
(loop11-frontier-audit.md), which re-ran the survey through ACM DL /
Eurographics DL / WSCG / peer-reviewed workshops after the first pass was
correctly judged too arXiv-heavy. Per AGENTS.md each note carries source,
claim, novelty assessment, fit/cost, and what was adopted vs deliberately
not.

## Notes

- [physics-solver.md](physics-solver.md) — XPBD + substepping (Small Steps,
  SIGGRAPH 2021; MGPBD, SIGGRAPH 2025; Orientation-in-XPBD, arXiv:2608.23606),
  speculative contacts, island sleeping, Jolt's determinism contract
  (record-rewind-replay check), Fiedler's fixed-timestep canon.
- [rendering-and-graphs.md](rendering-and-graphs.md) — Blade tracking-free
  RHI + global pass barriers (arXiv:2607.26506), idTech 6 frame structure
  (previous-frame reuse, persistent feature resources), real-time path
  tracing (ReSTIR lineage, RTXDI), frame-graph transient allocation.

## Adopted into the spec

| Finding | Spec change |
|---|---|
| Substepping > iteration for stiffness (Small Steps) | D1: compliance-scaled correction + velocity-from-position behind an opt-in that preserves old replays |
| Island sleeping via per-step rebuild | D1: union-find over the frozen contact graph, island-granular rest, wake-on-contact |
| Jolt's per-step determinism validator | D1: `WARPLOOM_PHYSICS_DETERMINISM_CHECK` debug flag (step twice, compare fingerprint) |
| Persistent feature resources, no lazy creation (idTech) | E2: compose chain pre-allocates ALL resources at init; toggles change pass recording, never rebuild |
| Dynamic rendering to un-bake passes (Blade direction, stage 1) | R5: fullscreen passes move to dynamic rendering before any GENERAL-layout work |
| Transient allocation from the graph (frame-graph canon) | R6: compile_graph owns resource declarations/lifetimes; ensure() derives |
| Stepped deterministic path tracing (replay machinery as the denoiser) | R1 spec rewrite: pause-accumulate-K-frames tonemap mode, ReSTIR deferred |

## Explicitly not adopted (with reasons)

- MGPBD multigrid solver — solves high-resolution deformable stiffness; we
  have no deformables.
- ReSTIR — reservoir stream state breaks the deterministic replay contract
  unless serialized into checkpoints; revisit when scene lights exceed ~64.
- Warm-started sequential impulse + contact caches — stored solver state
  conflicts with the snapshot-only replay contract at current scene sizes.
- Cross-platform float determinism build flag — no cross-platform product
  requirement yet; tests pin same-binary determinism.

## Loop 12 — research loop 1 of the new 10-loop series (2026-10-26)

- [loop12-physics-determinism-validator.md](loop12-physics-determinism-validator.md) —
  found the spec claiming the XPBD compliance opt-in, island sleeping and
  double-step validator as adopted with zero code behind them; implemented
  the validator (`set_determinism_check`) and the compliance knob
  (`set_contact_compliance`, default 0 = bit-identical replays) with
  sabotage-checked tests both directions, plus the repo's first Lean 4
  formalization (`proofs/PhysicsDeterminism.lean`: snapshot-pure detection,
  1D grid conservativeness, compliance-0-is-stiff). Papers: XPBD MIG 2016,
  Small Steps SIGGRAPH 2021, MGPBD SIGGRAPH 2025 (not adopted — no
  deformables), Orientation-in-XPBD + PBD-R (rotation arc, staged).
  Competitors read from source/docs: Jolt (island rebuild + determinism
  checkbox), Box2D v3 (speculative contacts — still open), Godot 4.6/Jolt
  migration, Rapier-for-Godot determinism claim. Sleeping deferred to loop
  2 of 10.

## Loops 2–10 notes

- [loop2-rigid-rotation.md](loop2-rigid-rotation.md) — rotation in PBD
  (Müller tutorial 22; Tobin/Rucker Lie theory arXiv:2608.23606; PBD-R
  arXiv:2603.14634; PBRBD survey arXiv:2311.09327). Sequenced adoption arc.
- [loop3-joints-broadphase.md](loop3-joints-broadphase.md) — XPBD joints
  (Müller tutorial 25), Morton BVH / SAP / spatial-hash comparison. Distance
  joint adopted-after-rotation; Morton BVH recorded with a numeric trigger.
- [loop4-6-transient-determinism-pt.md](loop4-6-transient-determinism-pt.md)
  — transient allocation from the graph, the per-step determinism validator
  (Jolt's record-rewind-replay check), PT accumulation as a product feature.
  The cross-loop conclusion: pre-allocation beats create-when-enabled, and
  the determinism contract is a FILTER that picks techniques whose state is
  derivable from the snapshot.
- [loop7-10-scripting-ecs-determinism-pt.md](loop7-10-scripting-ecs-determinism-pt.md)
  — dataflow vs exec-wire scripting (our pull model is the replay-correct
  one), ECS scheduling (sparse-set fits; archetype only on measured need),
  -ffp-contract=off (ADOPTED — the most dangerous cross-build
  nondeterminism source), SVGF denoising (v1 PT ships pause-to-converge).

## Loop 11 — the non-arXiv second pass

- [loop11-frontier-audit.md](loop11-frontier-audit.md) — the first pass was
  arXiv-heavy and abstract-level; this one re-ran the survey through ACM
  (PACMCGIT/HPG 2026, DOIs), Eurographics DL (HPG 2025), WSCG journal, a
  peer-reviewed ECS workshop study, and vendor documentation (NRD, FSR 4 /
  Redstone, RTXPT — labelled as vendor docs throughout). Outcomes: R6 gains
  the history-resources-never-alias invariant (F3), before any
  the history-resources-never-alias invariant (Sandu & Shcherbakov, WSCG
  2023, measured ~10% memory at Dagor/Enlisted scale); the ECS storage
  question is CLOSED with a measured ~10k-entity crossover (Cox et al., CGVC
  2025); the ReSTIR/denoiser deferrals survive 2026 literature with sharper
  reasons; R3's eventual RT-skinned design has a named technique
  (tetrahedral cage, HPG 2026). Also records a DOI collision in the HPG 2026
  diglib export, worked around by title+authors citation.

## Loop 13 — research loop 2 of the new 10-loop series (2026-10-26)

- [loop13-graph-lifetimes.md](loop13-graph-lifetimes.md) — closed the
  prerequisite R6b was gated behind: `derive_frame_resources` derives
  lifetimes from the recorded node sequence (RPS external/persistent
  taxonomy for the caller-supplied half). Papers: WSCG 2023 re-read,
  Blade tracking-free RHI (R5 direction, not adopted), Stołecki frame-graph
  survey 2026. Competitors: Unreal RDG Transient vs External (Epic docs),
  RPS persistent/transient/temporal/external (GPUOpen), Godot graph without
  allocator (verified negative), Unity descriptor-matched pooling. Lean
  `proofs/FrameAllocator.lean`: most-recent-fit model, history-Dedicated
  and alias-soundness theorems. R6b Vulkan wiring stays deferred, now
  unblocked rather than gated.

## Loop 14 — research loop 4 of the new 10-loop series (2026-10-26)

- [loop14-ecs-scheduling.md](loop14-ecs-scheduling.md) — re-verified the
  loop-11 ECS storage verdict (Cox et al., CGVC 2025, read via the eprints
  landing page; Bevy schedule docs; EnTT wiki; Flecs storage-in-pictures)
  and found a real soundness bug instead: `partition_into_waves` ignored
  `depends_on`, so dependent systems with disjoint masks shared a wave and
  ran concurrently. Fix: waves are dependency-aware (deps in earlier
  waves, no reverse edge) plus a documented no-structural-changes-in-a-wave
  contract. Tests: dependent-disjoint pair splits into 2 waves with ordered
  `run_parallel` over 50 rounds; independent-disjoint pair still shares one
  wave. Lean `proofs/EcsScheduler.lean`: `AllWavesOk` invariant,
  machine-checked schedule soundness from an explicit single-step axiom.
  No storage migration: scale is three orders below the ~10k crossover.

## Loop 15 — research loop 5 of the new 10-loop series (2026-10-26)

- [loop15-scripting-dataflow.md](loop15-scripting-dataflow.md) — verified
  the pull-model dataflow discipline (Lustre POPL 1987, DOI
  10.1145/41625.41641; Unreal pure/impure exec-wire docs; Godot visual
  shaders — vendor pages labelled as such) and found the script-node path
  violating it twice: a per-TYPE shared TickState (every node after the
  first saw dt=0; replay in the same object diverged — sabotage A/B by
  real rebuild), and unbounded double->size pin-count casts plus a
  process-global error string. Fix: graph-owned per-node prev-time map,
  dt in the per-node context copy, stateless lambda; range-checked 1..9
  counts degrading to zeros; lambda-local error sink. Lean
  `proofs/DataflowEval.lean`: `dtSeqGo_spec`, `sharedSecond_isZero`
  counter-model, pull-fold purity.  `noise1d`'s time->uint64 lattice cast recorded as open (host-driven,
  unproven document reachability).

## Loop 16 — research loop 6 of the new 10-loop series (2026-10-26)

- [loop16-float-replay-contract.md](loop16-float-replay-contract.md) —
  float determinism sources (Goldberg ACM Comput. Surv. 1991, DOI
  10.1145/103162.103163; Jolt `CROSS_PLATFORM_DETERMINISTIC` as the
  configuration-shaped precedent; Rapier hidden-state warning; Fiedler
  fixed-timestep as practitioner canon) and the defect they point at:
  `-march=native` in Release codegen, contradicting the repo's own
  contract — fixed to `-march=x86-64-v3` on x86_64. Pinned
  integer-ns composition (  `IntegerModeSplitsComposeExactly`); audited the replay-path float->int
  casts (NaN-safe by the comparison arms) and the mpsc arrival-order hash
  mixing (latent, needs an ABI change — recorded with trigger). Lean
  `proofs/FloatContract.lean`: `adv_compose` + residue bound, no axioms.

## Loop 17 — research loop 7 of the new 10-loop series (2026-10-26)

- [loop17-timeline-capture.md](loop17-timeline-capture.md) — clip
  evaluation sources (Lasseter SIGGRAPH 1987, DOI 10.1145/37401.37407
  for easing; Witkin-Kass SIGGRAPH 1988, DOI 10.1145/54852.54870,
  related but not adopted; Unreal Sequencer half-open spans; Godot
  interpolation modes — vendor pages labelled as such) and the defect
  they frame: mid-capture `LoadDocument`/`LoadReplay` replaced the whole
  document with no trace in the file while the format doc claimed
  self-consistency — silent replay divergence. Fix: reject both during
  active capture; auto-embed `scrub_to` warp targets missing from the
  recorder. Tests incl. a warp-to-pre-capture-frame round-trip;
  sabotage A/B by real rebuild (all 3 fail without the fix). Lean
  `proofs/TimelineReplay.lean`: log-frame monotonicity preserved by
  step/record, plus the unlogged-replace counter-model.

## Loop 18 — research loop 8 of the new 10-loop series (2026-10-26)

- [loop18-asset-streaming.md](loop18-asset-streaming.md) — asset
  streaming/compression sources (Khronos KTX 2.0 and glTF 2.0 specs as
  vendor standards; Basis Universal vendor docs — transcoding rejected to
  protect transport-independent bytes; idTech-6 virtual-texture feedback
  as practitioner canon, NOT adopted: a demand counter without tile
  residency drives nothing) and the defect they frame: PNG/JPEG enforce a
  256 MiB decoded-size cap but KTX2 `resize`d `w*h*4` unchecked (a
  30000x30000 header attempts 3.6 GiB from untrusted bytes), and
  `DecodedImage::valid()` compared in 32-bit product (wraps to 0 at
  65536x65536 — live on wasm32, latent on LP64). Fix: same 256 MiB cap
  for KTX2 checked pre-walk/pre-alloc; 64-bit `valid()`. Tests incl. the
  hostile-header reject and the wrap premise; sabotage A/B by real rebuild.
  Lean `proofs/AssetBounds.lean`: mip monotonicity (level 0 dominates, so
  the cap placement is sound), cap soundness, the 32-bit wrap
  counter-model. No axioms.

## Loop 19 — research loop 9 of the new 10-loop series (2026-10-26)

- [loop19-gpu-lod-culling.md](loop19-gpu-lod-culling.md) — GPU LOD
  sources (Clark CACM 1976, DOI 10.1145/360349.360354; Greene/Kass/Miller
  SIGGRAPH '93, DOI 10.1145/166117.166147, related NOT adopted — no
  pyramid-reuse proof here; Unreal HLOD screen-size docs as vendor docs;
  the GLSL/Vulkan OOB-UB contract, with `robustBufferAccess` measured
  absent) and the defect they frame: three LOD shaders trust a
  host-provided threshold count against fixed arrays (capacities 8/4/4,
  unenforced) — past the end of `lod_select`'s array sit the camera words,
  comparing as 0.0 px and selecting a phantom LOD 8. Fix: `min(count,
  bound)` at each site (identity on all in-tree counts). Live-fire GPU
  test (6.5 px sphere, hostile count 10): fixed culls, sabotaged shader
  reports visible LOD 8. Cull-shader clamps are same-shape hardening:
  verified by recompile + unchanged GPU suites + Lean, stated as such
  (their OOB words are past the push range — no deterministic live-fire
  exists). `lod_count == 0` underflow audited unreachable, recorded open.
  Lean `proofs/LodSelect.lean`: `select_le` bound plus both exhibits.
  No axioms.

## Loop 20 — research loop 10 of the new 10-loop series (2026-10-26)

- [loop20-synthesis.md](loop20-synthesis.md) — synthesis: per-loop
  decision table (adopted/rejected/deferred with triggers), cross-loop
  consistency audit with measurements (`set_context` 0 callers, so the
  Loop-15xLoop-17 eval-state hazard is conditional — trigger recorded,
  not "fixed"; KTX2 cap vs `maxImageDimension2D` sizing cross-check),
  spec delta (`replay-format.md` load point 4: MAY-warn → MUST-fail),
  Lean audit (`lake build` clean; `EcsScheduler.placeOne_preserves`
  recorded as the one stated axiom), and the kickoff: `load_replay`
  end-state verification against the closing checkpoint (one `to_json` +
  hash per load; new unlogged-ticks test + sabotage A/B). Open-item
  register with tripwires (ReSTIR, island sleeping, R6b wiring,
  `noise1d`, mpsc ordering, graph-clock wiring, tile residency,
  pyramid-reuse proof, `lod_count == 0`, legacy toolchain flag).

## Loop 21 — shared C++ gates (2026-10-29, first of the all-directions series)

- [loop21-shared-cpp-ci.md](loop21-shared-cpp-ci.md) — the estate runs one
  reusable workflow per language and C++ had none. Warploom's 10-job CI is
  extracted into `.github/workflows/cpp-gates.yml` (`on: workflow_call`),
  developed and CI-executed here first so the handoff to
  `engineering-standards` is a verbatim copy; `test.yml` is a thin caller.
  Sources: GitHub reusable-workflow vendor docs plus the estate's own
  gate-matrix page and `go-kit.yml` raw. Adopted: the kit shape
  (sequence shared, parameters per repo), `persist-credentials: false` on
  checkouts. Deferred to handoff: SHA-pinning. Verified: caller/callee
  contract script, sabotage-checked both directions, moved jobs
  byte-identical modulo the checkout hardening. Next in E7: the first
  dogfooding consumer repo.

## Loop 22 — estate handoff (2026-10-29)

- [loop22-estate-handoff.md](loop22-estate-handoff.md) — plumbing, no new
  technique. `cpp-kit.yml` is live in `engineering-standards` (tiered
  `fast|full`, 22 SHA-pinned action refs reusing sibling-kit SHAs);
  warploom's `test.yml` calls it, local files deleted. The inline-heredoc
  contract design was un-runnable (YAML block scalars vs column-0 Python)
  and replaced with an estate checkout + `scripts/check_cpp_contract.py`.
  Sabotage A/B both directions. Next: the dogfooding consumer repo.

## Loop 23 — first dogfooding consumer (2026-10-29, second of the all series)

- [loop23-dogfooding-consumer.md](loop23-dogfooding-consumer.md) —
  `WyattAu/warploom-game`: 20-cube tower drop, three claims (replay
  reproduces, external determinism, the pile fell). Second `cpp-kit.yml`
  caller (warploom legs disabled via the new inputs). Three defects no
  in-tree test could find: two install (`EXPORT_NAME`, missing
  `find_dependency`) and one replay (same-frame open/close ring
  collision → `restore_bytes` + regression test, both sabotage-checked).
  Session-level Step is ack-only by design — the game is the host. Next:
  the content pipeline (glTF/compressed textures).

## Follow-up searches for the next loop

- Visual-scripting execution models (dataflow pull vs imperative push, and
  Unreal BluePrint's graph re-instance costs) for the G4 script node's
  editor wiring.
- Asset streaming/browser patterns (G5) — idTech 6's mega-texture feedback
  buffer (render pass reports which tiles are needed) is the closest shipped
  analogue to a browser that reflects runtime state.
- ECS scheduling — archetype (Bevy/Unity DOTS) vs sparse-set (EnTT) vs our
  bitmap masks, against the SystemScheduler's wave partitioning. CLOSED by
  loop11 (Cox et al., CGVC 2025): the implementations are statistically
  indistinguishable at 100/1,000 entities and archetype only wins from
  ~10,000 (2x at 50k), while sparse-set wins add/remove by ~6.6x. Our scale
  is three orders below the crossover; revisit only above a measured 10k
  entities with hot structural churn.
- ReSTIR DI/GI with serialized reservoirs — revisit if the replay format
  gains a generic blob-payload section for solver state. (Loop 11
  sharpened this: the HPG 2026 spatial-reuse improvement changes reuse
  quality, not the reservoir STATE that breaks replay, so the deferral is
  about state, and stays.)

## Comparison research

- [../comparison-matrix.md](../comparison-matrix.md) - Warploom against Godot,
  Blender/Cycles, OpenCASCADE, Houdini/Karma, Unreal and Unity. Godot, Blender
  and OCCT were read from source; Unreal, Unity and Houdini are public-docs-only.
  Every competitor claim carries a file path or URL, and what could not be
  verified is listed rather than omitted.
