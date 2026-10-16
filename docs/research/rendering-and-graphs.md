# Rendering research

Recorded borrowings, in the order they were adopted. Each entry states the
source, the claim being borrowed, a novelty assessment, why it fits here and
what it costs, and what was *measured in this codebase* — never the paper's
numbers.

## Frame-graph resource allocation (R6)

**Source.** Sandu & Shcherbakov, "A Resource Allocation Algorithm for a
History-Aware Frame Graph", *Journal of WSCG* 31(1-2):63–70, 2023. ISSN
2305-6062. Peer-reviewed workshop proceedings, archival, no DOI; cited by venue,
volume and pages. Read alongside the survey in `loop11-frontier-audit.md`
§"WSCG / frame-graph state".

**The claim being borrowed.** A frame-graph resource allocator must decide
which resources may share memory, and the decision depends on *lifetime across
the frame boundary*, not just within a frame. A resource read by a later frame
is history: its previous-frame contents must survive every instant of this
frame's execution. The paper's cyclic-time allocator recovers the ~10% of GPU
memory that the safe rule (never alias history) gives up, measured in their
engine at 81 nodes / 27 resources / 14 with history reads.

**Novelty assessment.** Nothing here is novel. Declaring resource lifetimes in
a frame graph is standard; the history-aware distinction is the paper's
contribution; interval overlap testing is textbook. What is *not* claimed: the
cyclic-time allocator, the memory recovery, and any speedup. This engine takes
the conservative branch on purpose.

**Why it fits here.** R1b made the rule load-bearing rather than theoretical.
The path tracer's accumulation image is now genuinely history: it is written
during the live frame and sampled by the *capture chain after the frame
boundary*, so "previous frame" is not a metaphor. The H-Z packed pyramid is the
pre-existing case. Aliasing a transient over either would produce a wrong
picture with **no validation layer diagnostic** — the class of bug this
repository has been repeatedly bitten by (the BLAS scratch barrier, the shared
geometry buffer, the accumulation image missing `VK_IMAGE_USAGE_SAMPLED_BIT`).
Each was invisible to a spec check and only visible by measuring.

**What it costs.** Nothing in memory: the safe rule reserves history
allocations permanently, and at 1280×720 with a handful of compose targets the
transients being reclaimed are the small ones. What it costs is that callers
must *declare* lifetimes, which is new API surface and new opportunities to
declare them wrong. That cost is real and is why R6a ships as a pure, tested
planner with no Vulkan behind it: the derivation is verifiable without a device,
so the Vulkan side can be added once the rule is proven rather than after.

**What was measured.** A pure planner over declared lifetimes, on the compose
frame's own shape (10 resources, 5 passes, H-Z pyramid and PT accumulation both
history): **4 of 10 allocations elided**, with both history resources keeping
dedicated allocations. Randomized deterministically (LCG, no `<random>`) over
200 configurations of 3–10 resources: 0 violations of the never-alias-history
rule, and 0 overlapping shares.

### The part worth remembering: a sabotage that passed

The first validation attempt **passed**. Three independent guards enforced the
same invariant — the interval widening, a `history → HistoryReserved` enum
label, and a `cand.history → continue` skip in the reuse loop — so removing any
one of them changed nothing. The suite was green and proved nothing.

Collapsing to one mechanism (the interval) exposed a second problem: the
randomized sweep also passed the sabotage. Not because the sweep's input space
was too narrow, but because its *helper* was a tautology — it compared
`plan[i].shares_with` against resources declared **later** in the list, while
the owner is always declared earlier, so the comparison could never match.
A standalone probe measured what the planner actually did under sabotage:
history aliased in **84 of 200** random configurations. 84 opportunities, zero
detected.

So two fixes, both of which are the real lesson: **defence in depth can hide a
missing test**, and a property test is only as strong as the property it
actually checks. `invariant_violated()` now resolves the owner by name and
checks the rule as specified — history is never aliased, *in either
direction*, which is not implied by interval disjointness at all, since a
history resource and a disjoint transient genuinely have disjoint intervals.
With that, the sabotage fails at trial 5 by name.

`ResourceAllocation` has no `HistoryReserved` member, deliberately. An enum
value asserting the invariant would be a second, unprovable guard. The
invariant is a property of the interval arithmetic; the caller that wants to
assert it checks `kind == Dedicated` for every resource declared as history.

## Dispatch-table caching by handle (R1b follow-up)

**Technique.** None. This entry records a bug fix, not a borrowing, and is
here because the tempting "optimisation" is worth naming as a trap rather than
silently reverting.

**What happened.** Ray-tracing entry points were resolved once and cached in a
process-global keyed by `VkDevice` handle value. Vulkan guarantees no
uniqueness for dispatchable handles: an implementation may return a previously
destroyed handle value for a new object. Keying any cache by handle value is
therefore unsound in exactly the environment a renderer targets, where devices
are created and torn down constantly (this suite builds ~40 of them).

**Why caching by value still "works" in a test that runs alone.** A single
test creates one device, destroys it, and exits. No second device ever lands on
the recycled handle, so the stale-pointer path is unreachable. That is the
danger: the bug is invisible in isolation and only appears under churn, which
is the condition a full test suite and any real application create.

**Cost of the fix.** Five `vkGetDeviceProcAddr` calls per acceleration-structure
operation. AS builds happen a handful of times per tracer build -- not per
frame -- so the effect on frame time is below the noise floor. This is stated
as reasoning about where the calls sit, not as a measured number: no benchmark
was run, so none is claimed.

**Generalisation.** Vulkan's own "dispatchable object" model is the source:
handles are opaque values, not identities. Caches keyed by them -- dispatch
tables, function pointers, per-object lookup tables -- must be invalidated on
destruction or keyed by something with a lifetime.

### The audit that followed, and its measured (negative) result

Having found one, the obvious question is whether the class repeats. It does
not, and that is worth recording so nobody re-runs the search:

- **Every `vkGetDeviceProcAddr` site in `modules/`** (five in the
  acceleration-structure builder, three in `vulkan_rt_pipeline.cpp`, one in the
  renderer's sync2 path). Only the acceleration-structure builder cached; the
  RT pipeline helpers already resolve per call, and the renderer's sync2 lookup
  is local to one submit.
- **File-scope mutable state in `modules/render/src`**: the audit script found
  exactly one hit, a namespace alias. No globals.
- **Function-local `static` across `modules/render`, `modules/core`,
  `modules/asset`**: three hits. Two are name-keyed registries holding
  function pointers, which is the same *shape* as the bug and worth reading
  rather than pattern-matching. `ScriptModule::load_builtin` sets
  `handle_ = nullptr`, so a builtin is never `dlclose`d and the registry's
  function pointers cannot dangle -- verified by reading the call, not assumed
  from the name.
- **Test order dependence**: the suite passes 626/626 under `--gtest_shuffle`
  and under three fixed seeds (12345, 99991, 424242), so there is no hidden
  ordering contract. This is now part of the verification baseline, because a
  global registry plus an "already registered" test is exactly the pairing that
  manufactures order dependence silently.
