# Second-pass research audit — non-arXiv sources, 2026-10-09

Status: research notes. The first pass (2026-10-08) was correctly criticized as
too narrow: most of its papers were located through arXiv queries, and several
were adopted from abstracts rather than full reads. This pass re-ran the survey
through the venues AGENTS.md actually asks for — ACM Digital Library (PACMCGIT /
TOG), Eurographics Digital Library (HPG European years), SIGGRAPH and I3D
programs, WSCG, peer-reviewed workshop proceedings, and vendor/engine
documentation (which is labelled as vendor documentation, never as evidence).
Every finding below was read from the source page or full text, not from a
search snippet.

## F1. HPG 2026 (PACMCGIT V9N4) — the whole program, with abstracts

**Source.** High-Performance Graphics 2026, Proceedings of the ACM on Computer
Graphics and Interactive Techniques Vol. 9 No. 4 (ACM, July 2026),
DOI 10.1145/3828970; individual papers DOI 10.1145/38200xx, all read from
diglib.eg.org collection e73f8f86-2c62-4288-b543-d8b07453eda5. Peer-reviewed,
archival.

Source-integrity note, recorded because AGENTS.md requires stable identifiers:
the diglib BibTeX export assigns DOI 10.1145/3820024 to BOTH
"Memory-Efficient Bounding Volume Hierarchies with Merged Nodes" (Haydel,
Kensler, Brunvand, Yuksel) and "Compatibility-Guided Neighbor Selection for
ReSTIR" (Junkins, Kettunen, Lin, Ramamoorthi, Wyman). A DOI collision means
neither citation can be resolved by DOI alone until ACM fixes the export;
both are cited here by title + authors + proceedings instead. This is a defect
in the metadata export, not in the papers.

Findings that touch the roadmap:

- **"Optimizing Vulkan Dispatch Schedules for Real-Time U-Net Denoising"
  (Sassie, Hanika, Alber, Dolp, Dachsbacher; 10.1145/3820016).** A portable
  ONNX-to-Vulkan deployment path for U-Net denoisers that is fully GPU-resident
  — no host sync, no external inference framework — using graph-based and
  profile-guided dispatch scheduling (block sizes, layouts, kernel fusion),
  beating TensorRT on the OIDN network. Claim: neural denoising can be a
  lightweight in-engine dependency rather than a framework.
  **Relevance:** R1's eventual denoiser. Our current R1 spec deliberately
  ships pause-to-converge (no denoiser, no temporal state), so nothing is
  adopted. But the "external framework is impractical for tightly integrated
  pipelines" conclusion independently supports the decision to defer NRD/DLSS
  integrations rather than pull one in. Cost if ever adopted: a network is a
  multi-megabyte asset plus per-driver dispatch tuning — far past the one-
  package-manager budget unless written in-tree. Recorded, not adopted.

- **"Compatibility-Guided Neighbor Selection for ReSTIR" (Junkins et al.).**
  Improves ReSTIR's SPATIAL reuse (which pixels may share samples) by
  path-compatibility heuristics: SMAPE −6–29%, temporal covariance −22–49%,
  at 2–5% incremental cost, drop-in for any pixel-space ReSTIR variant.
  **Relevance to the ReSTIR deferral:** it sharpens but does not overturn it.
  The deferral exists because reservoir stream state breaks snapshot replay;
  this paper improves reuse QUALITY and does not change WHAT STATE exists.
  The deferral stands, with the trigger unchanged (lights > ~64 or a replay
  blob section for solver state). What this paper adds: when ReSTIR is
  eventually evaluated, spatial neighbor selection is a knob measured in
  image error, not a second research project.

- **"Ray Tracing Massive Amounts of Animated Geometry" (Gruen, Benthin, Kern,
  McAllister; 10.1145/3820014).** Split animated geometry into a tetrahedral
  cage; build per-tetrahedron BLAS once at load, rebuild only the cage's
  structure per frame, transform RAYS through the animated cage instead of
  transforming vertices. Per-frame animation cost decouples from triangle
  density.
  **Relevance:** R3 (GPU-driven skinned scenes with RT). If skinned meshes
  ever need RT, this is the technique that keeps per-frame cost independent
  of mesh density — the same "animation cost is a function of the cage, not
  the triangles" shape as our H-Z work being a function of the pyramid, not
  the scene. Not adopted now: R3 has no RT requirement yet. Recorded with the
  falsifiable claim (per-frame BLAS work ∝ cage resolution).

- **"Common Subtree Merging Compressed Opacity Micromaps" (Chernaik, Modisett,
  Billeter; 10.1145/3820017).** Lossless OMM compression to ~2 orders of
  magnitude smaller, real-time random access without decompression, plus
  hierarchical aggregate opacity usable as LOD. **Relevance:** R2 (RT
  reflections/AO) on alpha-tested geometry; ours has almost none today.
  Deferred with R2.

- **"Real-Time Dissectible Deformable Models…" and "Periodic Anderson
  Acceleration for GPU-based Elastic Body Simulation" (10.1145/3820013,
  10.1145/3819834).** Both PBD-family with a solver-acceleration component.
  **Not adopted:** our physics has no deformables; PAA accelerates implicit
  elastic solves, and a least-squares mixing step over recent iterates is
  exactly the kind of cross-step state our replay contract forbids without
  snapshot support. Same conclusion as the MGPBD rejection, now with 2026
  evidence.

- The rest of the program (radiance fields, Gaussian splatting, hair, vector
  graphics via mesh amplification, light-field displays) has no bearing on the
  four open roadmap items; surveyed for coverage and not recorded further.

## F2. HPG 2025 (Eurographics year)

**Source.** High-Performance Graphics 2025, Eurographics Digital Library,
DOI 10.2312/hpg.20251xxx, Copenhagen, June 2025.

- **"No More Shading Languages: Compiling C++ to Vulkan SPIR-V" (Devillers et
  al., 10.2312/hpg.20251167).** Vcc: conventional C/C++ compiled to SPIR-V,
  competitive with native CUDA on a real-time path tracer benchmark.
  **Not adopted:** it is a compiler (a dependency order of magnitude past the
  budget) and our 74-shader GLSL tree builds everywhere already. Recorded
  because it is the standing answer to "should shaders be written in C++" —
  possible, published, peer-reviewed, and still not worth the dependency.

- **"Real-Time GPU Tree Generation" (Kuth et al., 10.2312/hpg.20251168).**
  GPU work graphs with mesh nodes rendering gigabytes of generated geometry
  from kilobytes of procedural code. **Not adopted:** no vegetation; recorded
  as the reference for work-graph mesh nodes if procedural geometry ever
  enters scope (Vulkan mesh-node support is vendor-gated today).

## F3. History-aware frame-graph allocation (directly sharpens R6)

**Source.** Sandu & Shcherbakov, "A Resource Allocation Algorithm for a
History-Aware Frame Graph", Journal of WSCG 31(1-2):63-70, 2023, ISSN
1213-6972 (also dspace.zcu.cz item 3fe80f2d). Implemented in Gaijin's Dagor
Engine; Enlisted's frame graph (81 nodes, 27 tracked resources, 14 with
history reads) is the measured case.

**Claim.** Frame-graph transient allocation is dynamic storage allocation
(DSA) only when no resource is read across the frame boundary. Roughly half
of a shipped engine's graph resources ARE read across the boundary (TAA
history, screen-space AO/reflections, occlusion-culling pyramids). The paper
generalizes DSA to cyclic time and gives a best-fit greedy algorithm:
O(n log n), ~20 µs on their graph, ~10% memory saved (6 MB) versus the naive
rule of never aliasing history resources, competitive ratio ~1.1, and a proof
that the natural interval-graph-coloring class cannot be optimal.

**Novelty.** The generalization is the paper's contribution; the machinery is
standard best-fit. Plainly: this is a published engineering refinement, not
an invention of ours to borrow wholesale.

**Fit/cost, and the spec change it drives.** Our own graph ALREADY has the
paper's motivating resource: the H-Z pyramid is a previous-frame read
(occlusion culling), and exposure/compose history will join it. The R6 spec
(transient allocation from compile_graph) must therefore state, from day one:
resources with cross-frame reads are NEVER aliased; only pure-transient
lifetimes participate in reuse. That is a one-line invariant in the allocator,
cheap to enforce, and it forecloses the exact bug class (a transient aliased
over TAA/pyramid history) the paper exists to prevent. Adopted into R6's spec
as a stated invariant with the paper's numbers as the expected magnitude of
what skipping it costs (~10% at their scale — ours is far smaller, so the
invariant costs almost nothing).

## F4. ECS storage: the open follow-up question, answered

**Source.** Cox, Williams, Vickers, Ward, Headleand, "Run-time Performance
Comparison of Sparse-set and Archetype Entity-Component Systems", CGVC 2025
(University of Staffordshire eprints 9315), peer-reviewed UKCG/EG workshop.
Cross-checked against Bevy's own notes (Bevy 0.5 release notes; Bevy PR
#14049) — engine source/vendor material, labelled as such.

**Claim, with numbers.** At 100 and 1,000 entities, frame latency is
statistically indistinguishable (sparse-set Mdn 0.799 ms vs archetype 0.797 /
0.799 ms, p=0.49 / 0.376). From 10,000 entities archetype wins (Mdn 1.721 vs
2.945 ms; at 50k, 7.410 vs 13.819 ms — ~2x), while sparse-set entity
instantiation/modification is ~6.6x faster. Bevy's own history agrees on the
tradeoff (tables iterate fast, add/remove costs; sparse sets are opt-in for
churn-heavy components).

**Decision.** Our bitmap-mask ECS is sparse-set family. The README's open
question ("do component-mask waves lose to archetype iteration at our entity
counts?") now has a numeric answer: below ~10k entities there is nothing to
lose — the difference is inside noise. D3's current scene scale is ~10
objects; even a 100x growth stays 10x under the crossover. The trigger is
therefore WRITTEN DOWN instead of guessed: revisit storage only when a
measured scene sustains >10k simultaneous entities with hot structural churn.
The README follow-up is closed with that number.

## F5. Vendor documentation (labelled as such — API behaviour, not evidence)

- **NVIDIA NRD (README, v4.18.0, github.com/NVIDIA-RTX/NRD).** The denoiser
  makes NO GAPI calls: it returns resource descriptions, the app allocates and
  dispatches. Permanent vs transient texture pools, recreate-on-resize (no
  dynamic resize), inputs are motion vectors + normal/roughness + viewZ +
  optional confidence, radiance must be material-demodulated, blue noise
  preferred with sequence length ≈ history length. Performance table (RTX
  4080, 1440p): REBLUR 2.55 ms, RELAX 3.25 ms, SIGMA 0.40 ms.
  **Effect on R1:** confirms the denoiser integration shape (dispatch-driven,
  app-owned resources — same philosophy as our renderer) but ALSO confirms
  the deferral: NRD is temporal state, exactly the class our replay contract
  cannot serialize yet. Pause-to-converge remains v1 PT. When R1 grows a
  denoiser, NRD's no-GAPI-calls contract is the one that fits our
  app-owns-resources renderer.

- **AMD FSR 4 / "Redstone" (GPUOpen manuals + product pages).** Vendor
  documentation. Relevant API facts for any future upscaler: screen-space
  pixel-unit motion vectors (internally truncated to 16-bit — extra precision
  is wasted), render-resolution inputs must be JITTERED except the motion
  vectors, an optional 1x1 R32_FLOAT EXPOSURE texture (we already have a
  scalar exposure and a chain that owns it — the texture form is a
  descriptor, not a rewrite), linear color space + HDR flag, and FSR 4 ML
  modes are RDNA-4-only with signed-DLL distribution (DX12-only for ML
  modes). **Not adopted:** hardware gate + binary distribution violate the
  dependency budget; recorded so that if a Vulkan upscaler path appears, the
  motion-vector/exposure/jitter contract is already known and testable.

- **NVIDIA RTXPT (github.com/NVIDIA-RTX/RTXPT, v1.8.1 README).** Vendor
  reference path tracer (Falcor-derived). Its stack — ReSTIR DI/GI via RTXDI,
  NRD denoise, OMM, SER, DLSS-RR — is the industry composition R1 is
  deliberately NOT taking wholesale, because every temporal component in it
  is replay-hostile. Useful as the checklist of what a "full" PT integration
  contains, and its known-issues list is a warning shot: enabling the Vulkan
  debug layer shows "a number of warnings and errors - fixes are work in
  progress" IN NVIDIA'S OWN REFERENCE. Our zero-diagnostics bar is above the
  reference implementation's; that is worth knowing when judging the cost of
  keeping it.

## What this pass changes

1. R6 gains the history-resources-never-alias invariant (F3), before any
   allocator code exists — the cheap time to write it.
2. The ECS follow-up question is closed with a measured crossover (F4).
3. R1's deferral stack (no denoiser, no ReSTIR) survives contact with the
   2026 literature, each with a sharper reason (F1, F5).
4. R3's eventual RT-skinned-geometry design has a named technique and a
   falsifiable claim (F1, tetrahedral cage).
5. Source discipline demonstrated on this repo's own terms: one metadata
   defect found in a major venue's export and worked around (F1), and the
   vendor/engine material is labelled wherever it is the only source.

## Not read, stated plainly

SIGGRAPH 2026's TOG issue (327 accepted papers) was surveyed at
program/TOC level only — none of its simulation papers were pulled because
HPG 2026's physics session and the WSCG/ECS material covered the open
physics/data-model questions. IEEE TODS/TOG, USENIX, and dissertations were
not reached for this pass; the frame-graph question (the one that needed
them most) was instead answered by the WSCG journal paper, which is
peer-reviewed and implementation-backed. If a future step needs
multiprocessor scheduling or storage-engine depth, USENIX ATC/OSDI is the
gap to close first.
