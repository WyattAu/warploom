# Capability comparison: Warploom against Godot, Cycles, OpenCASCADE, Karma, Unreal, Unity

Every competitor claim below carries a file path or URL. Godot, Blender and
OpenCASCADE were checked out and read directly; Unreal, Unity and Houdini are
public-docs-only because their source is private. Claims I could not verify are
listed in [§7](#7-explicitly-unverified) rather than quietly omitted, and the
Warploom column distinguishes **measured** (run here) from **designed** (written
down, not yet running) from **absent**.

Versions pinned at read time: Godot `81ab961`, Blender `3fe0f2ab2a29`,
OCCT `3d097a03`; UE docs 5.8; Unity manual 6000.6 / SRP Core 17.0.4 /
HDRP 17.6.0; Houdini docs 22.0.

## 1. The headline: what the matrix actually shows

Three findings are worth more than the table.

**1. Transient memory aliasing is close to a two-horse field, and one of those
horses is Unreal.** Godot, the most actively developed open-source Vulkan
renderer in this set, has *no* transient allocator — a verified negative, not an
inference (`find . -iname "*transient*"` returns nothing outside `.git`; the
`TEXTURE_USAGE_TRANSIENT_BIT` it does expose is a TBDR lazy-attachment bit whose
only intended call site is a TODO at
`servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.cpp:188`).
Unity's Render Graph optimises GPU memory by "reusing allocated memory if a
texture has the same properties as an earlier texture" — *descriptor-matched
pooling*, which is exactly the approach Epic names as the thing RDG's
transient allocator exists to beat: *"The resource pools are forced to compare
and match RHI descriptors to determine their reuse. Whereas the transient
allocator can share underlying memory."* Unreal is the only project in this set
that documents lifetime-overlap aliasing.

Warploom's position is honest and mid-flight: R6a landed a pure, tested
lifetime planner (`plan_resource_allocations`), but R6b is **not wired** — no
Vulkan image is allocated from it. See [§6](#6-where-warploom-actually-stands).

**2. A denoiser is table stakes and Warploom has none.** Cycles ships OIDN 2.5.0
(optix + DLSS backends too), Karma ships Intel OIDN out of the box, HDRP offers
OIDN with OptiX deprecated. `grep -rl "denois|a-trous|svgf|oidn" modules/ assets/`
returns **nothing**. The one project that needs no denoiser is Unreal, and that
is a deliberate architectural choice with a published argument, not an omission
— Lumen filters *in the radiance cache* and is explicit that this is "different
from a screen-space denoiser, as we're still operating in the downsampled
radiance cache space". That is the shape of the fix if Warploom wants one.

**3. Warploom's determinism contract is stronger than almost everything here,
and that is not a claim — it is a verified negative on both sides.** Only Karma
makes any device-mixture reproducibility statement ("No matter what mixture of
devices it uses, the XPU engine should produce the exact same result", hedged
with "should"). Blender cycles per-pixel seeds so sampling *streams* are
schedule-independent (`kernel/sample/pattern.h:143`,
`hash_iqnt2d(x, y) ^ seed`) and exposes a `Deterministic Guiding` socket
defaulting to `true` — while explicitly admitting in the source that its packed
BVH arrays are *not* deterministic (`bvh/build.cpp:1086`: "This doesn't give
deterministic packed arrays, but it shouldn't really matter because order of
children in BVH is deterministic"). Unreal and Unity document no reproducibility
contract at all. Warploom treats determinism as a product feature
(`-ffp-contract=off`, no wall-clock reads in a tick, hash-verified replay) and
now runs the whole suite under `--gtest_shuffle` to prove no hidden order
dependence.

## 2. The matrix

| | **Warploom** | **Godot** | **Blender / Cycles** | **OpenCASCADE** | **Houdini / Karma** | **Unreal** | **Unity** |
|---|---|---|---|---|---|---|---|
| **Resource mgmt** | Persistent `VulkanMemoryAllocator` (in-tree) + a **pure lifetime planner that is not yet wired** (R6a shipped, R6b gated) | Frame-cycled deferred free + VMA pools + `is_discardable` store-op hints | Tile-based streaming; no GPU pass graph | n/a (no renderer) | Per-device memory **not pooled**; evict on OOM | **Render graph + transient lifetime-aliasing allocator** (`r.RDG.TransientAllocator`), pools as fallback | Render graph + **descriptor-matched reuse**; no aliasing claimed |
| **Render graph** | **Partial** — `compile_graph` computes barriers between declared passes; used for the 2-stage compose chain only | Yes — `rendering_device_graph.cpp` (2885 lines): topological sort, barrier synthesis, command-buffer partitioning | No — megakernel | n/a | No (Hydra delegate) | Yes — RDG, the most complete | Yes — RenderGraph |
| **Cross-frame lifetime model** | None in code; planner expresses it (Transient/History) but is unexercised | `Frame[frames_drawn]` disposal lists + VMA | Per-tile streams; frame history only for particles | n/a | Per-device, evictable | **Transient (graph-scoped) vs External** | Internal (graph-scoped) vs Imported |
| **TBDR / tile memory** | No | `TEXTURE_USAGE_TRANSIENT_BIT` → `VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT` / Metal memoryless; **effectively unused** (TODO) | No | n/a | No | Split-barriers | Merges into native render passes (standout mobile feature) |
| **BVH** | Vulkan RT: BLAS/TLAS/SBT built in-tree, one buffer per mesh | RT exposed in RD but **consumed by no renderer** (only soft-body physics) | **Binned SAH**, 32 bins, 2-child `BVH2`, **single-stack** traversal (192 entries); default CPU layout = **Embree**; OptiX/Metal/HIPRT/Multi | No BVH | OptiX (GPU) + Embree (CPU) | HW RT (triangles) / SW RT (mesh DF + Global DF, per-mesh first 2 m) | HW RT |
| **Path tracer** | **Yes**, in-tree `VulkanPathTracer`; `max_bounces` default 4; accumulation verified correct | **No** (SDFGI only) | Yes — `PathTrace` megakernel; `max_bounce` **7**, `samples` **4096** | No | Yes, both engines; viewport-capable | Realtime GI (Lumen), not a converged tracer | Yes — HDRP Volume override; **restarts on camera move** |
| **Denoiser** | **None** | None (in-engine SDFGI probes) | **OIDN 2.5.0** + OptiX + DLSS (`use_denoise` default **false**) | No | **OIDN bundled** + OptiX | **None by design** — denoises in the radiance cache | OIDN (default choice) + OptiX (**deprecated**); `None` is default |
| **Sampling** | One stratified-ish jittered sample per frame; fixed seed = frame index | n/a | `sampling_pattern` (5) × `direct_light_sampling_type` (3) × guiding (3×3); Light Tree; MNEE; **adaptive sampling on, threshold 0.01** | n/a | Auto vs Path-Traced convergence; **Pixel Oracle**; path guiding | Radiance cache + incoming-light importance sampling | `Seed Mode`/`customSeed`; accumulate until camera moves |
| **CAD / parametric geometry** | glTF + a document model; **no BRep** | No | No | **BRep_TFace holds a list of `Poly_Triangulation` + an active one**, selectable by `Poly_MeshPurpose`; per-surface-type mesher dispatch via RangeSplitter templates; anisotropic U/V tolerance | SOPs + VOPs; USD-centric | Nanite clusters (not CAD) | No |
| **Virtualized geometry** | LOD mesh chain; no cluster scheme | No | No | n/a | No | **Nanite**: hierarchical clusters, crack-free LOD, own pass bypassing draw calls; RT against Nanite unsupported (fallback mesh) | No |
| **Determinism** | **Product feature, verified**: `-ffp-contract=off`; no wall-clock reads in a tick; hash-verified replay; **628/628 under `--gtest_shuffle`** and 3 fixed seeds | **None claimed** — only shader-compile ordering | Per-pixel seeding makes streams schedule-independent; **BVH packed arrays explicitly not deterministic**; `Deterministic Guiding` default true | **[UNVERIFIED]** | **Documented**: device-mixture reproducibility, XPU only, hedged with "should" | None documented | None documented |

## 3. Where Warploom is genuinely ahead

Not much, and it should be said plainly rather than buried. Two things:

- **Determinism as a product feature.** Of six mature projects, one makes a
  hedged device-mixture claim and one explicitly concedes its BVH is not
  deterministic. Warploom makes replay reproducibility a contract and now tests
  for hidden order dependence with `--gtest_shuffle`.
- **Dependency discipline.** Cycles, Karma and HDRP all reach for OIDN —
  a third-party binary. Warploom's budget is one package manager (CPM) and one
  third-party library (GoogleTest), so "add OIDN" is not on the table without
  justifying it against that budget in the commit message. The consequence is
  visible in the matrix as a gap; the alternative — a 30 MB vendored binary —
  is a decision, not an oversight.

## 4. Where Warploom is behind, ordered by how much it hurts

1. **No denoiser** (§1.2). Every converged-tracer peer has one. Without it, PT
   is a research mode rather than a usable one. *Blocked on a design decision*,
   not on effort — see §5.
2. **No transient aliasing, and the planner is unexercised.** R6b is gated
   behind a prerequisite that does not exist yet: the renderer declares
   resources against a fixed pass list, not a graph, so "a resource live on
   passes [i, j]" does not yet describe anything real. UE solved exactly this
   and documented the setup/execute split that makes lifetimes expressible.
3. **No adaptive sampling.** Cycles (threshold 0.01) and Karma (Pixel Oracle)
   both stop sampling converged pixels. Warploom spends samples uniformly, so
   simple regions cost the same as corners — and at a fixed wall-clock budget
   that is wasted work.
4. **BVH is fixed-quality.** No path guiding, no Light Tree, no MBVH, no
   two-tier acceleration. For a handful of meshes this is irrelevant; it is the
   thing that stops mattering at scene sizes Warploom does not target yet.
5. **No CAD kernel.** OpenCASCADE's contribution here is a data-model lesson
   more than an algorithm one: *a face holds a list of triangulations and one
   active*. That decouples render-resolution from analysis-resolution, which is
   the property that makes CAD and rendering coexist at all. Warploom has
   exactly one mesh per object.
6. **Path tracing is not usable from the capture path.** The tracer is verified
   correct; the viewport's capture of it is not — see
   `docs/roadmap.md`, "The live path-traced capture is not showing the
   path-traced image".

## 5. The denoiser decision, stated before it is built

This is the one place where the matrix forces a real choice rather than more
work.

- **Vendor OIDN** (Cycles, Karma, HDRP). Rejected by the dependency budget:
  OIDN is a prebuilt third-party library and adding it must be justified against
  "CPM + GoogleTest" in the commit message. It is not free to set aside — OIDN
  is better than anything written here in the foreseeable future.
- **In-tree à-trous / bilateral (SVGF-style) filter**, using normal and
  albedo. This is what the matrix implies: a denoiser needs a G-buffer, and
  Warploom already builds one for the raster path. It is implementable at this
  dependency budget and is the honest counter to "no denoiser".
- **Denoise by construction**, à la Lumen. Radically more work: it changes the
  architecture rather than adding a pass, and the payoff depends on solving
  radiosity problems Warploom has not started.
- **Do nothing, document the gap.** Current state.

The recorded choice should be one of these *written down*, because the gap
survives an unwritten decision and not a documented one.

## 6. Where Warploom actually stands

Stated precisely, because "measured" and "designed" are different things here.

**Measured on this machine (RTX 2060, `default` preset):** 628 unit tests green
under `VK_LAYER_KHRONOS_validation=1`; ctest 6/6; ASan/UBSan 555; TSan 0 races;
74 shader sources compile; live protocol proofs 76/76 headless and 28/28 on a
visual host; viewport matrix (hdr, bloom, no-hdr, node editor, pt) all clean
with 0 validation diagnostics; clang and GCC both warning-free.

**Measured about the path tracer specifically:** the *linear* accumulation mean
is flat — 0.7336 → 0.7357 across 300–1800 samples read from inside the running
viewport, and 0.01% drift headlessly, bit-identical across runs, robust to
scene, exposure, seed schedule and accumulated depth to 1632 samples. The RNG is
unbiased and uncorrelated over 4096 frames. **The captured image is not
verified** and is currently broken.

**Designed but not running:** the frame-graph lifetime planner
(`plan_resource_allocations`) — 4 of 10 allocations elided on a compose-shaped
frame, history never aliased, 200-configuration sweep clean. No Vulkan object is
allocated from it.

**Absent:** denoiser, adaptive sampling, path guiding, BRep/CAD kernel,
virtualized geometry, MBVH, transient aliasing.

## 7. Explicitly unverified

Not omitted, not guessed:

1. OCCT's `Poly_MeshPurpose` enum members — header outside the read checkout.
2. OCCT determinism — no statement found either way; `InParallel` defaults to
   `false` but parallel mesh building is the obvious hazard.
3. Whether Unity HDRP uses the Render Graph — **Unity's own docs contradict
   each other** in the same package family (SRP Core 17.0.4 says both URP and
   HDRP use it; the 17.2 `IRenderGraphRecorder` API page says "currently this is
   only used by URP"). Do not repeat either claim without an independent check.
4. Cycles bit-exactness across device count, tile size or backend. Per-pixel
   seeding makes *streams* schedule-independent, but no documented bit-exactness
   claim was found and none was tested.
5. Godot's SDFGI denoising algorithm — the data structures in `gi.h` were read,
   not the filter implementation.
6. Houdini SOP compiled-block order stability, and Karma bit-exactness across
   Houdini or driver versions.
7. Any Unreal or Unity reproducibility contract. None in public docs; if one
   exists it is probably in a SIGGRAPH paper rather than the documentation.
8. Karma XPU's exact device-mixture claim was read but **not tested** — this
   engine has no second GPU to test it against.