# Working agreement

This file is the operating contract for autonomous work on Warploom. It exists
because the project is far larger than any single session: a rendering engine,
a deterministic simulation engine, a movie/sequencing tool, 3D modelling and
CAD, with determinism and performance as product features rather than
afterthoughts. "No steps forward" is not a reachable state, so the process has
to be a loop rather than a plan with an end.

## The loop

Repeat until stopped:

1. **Evaluate.** Read `docs/roadmap.md`, `docs/rendering-status.md` and the
   build/test output. Identify the single highest-value next step. Prefer a
   step that (a) unblocks others, (b) fixes a correctness or honesty defect, or
   (c) removes a constraint that is currently silently limiting something.
   Write the step down before starting it.
2. **Research**, when the step involves a technique rather than plumbing. See
   the research discipline below.
3. **Execute.** Smallest change that fully resolves the step. Land it verified,
   or revert it and record why.
4. **Verify.** Fresh clone, build, full suite, validation layer, live proofs.
   Claims in docs must be true of the code as it stands.
5. **Evaluate again.** Record what changed, what it made possible, and what it
   revealed. That last part matters most: a step that reveals a deeper problem
   has succeeded even if the step itself was small.

### Rules that survive a failed step

- **A step that cannot be verified does not land.** If equivalence cannot be
  demonstrated, either find a way to demonstrate it or do not ship it.
- **Revert rather than ship something measured to be wrong**, then write up what
  the attempt established. A reverted experiment with a precise finding is
  worth more than a landed change nobody can justify.
- **Scale the verification.** A check that passes at one size proves one size.
  This has bitten twice: a broadphase that was bit-identical at 1000 bodies and
  diverged at 4000, and a docs claim that was true of the default preset and
  false of three others.
- **Never let a test pass by accident.** If a sabotage does not fail the test,
  either the test is weak or the sabotage is a no-op. Find out which before
  trusting it. Two tests here have now been "validated" by sabotages that
  turned out not to touch anything.
- **Do not report a number you did not measure.** The fabricated `gpu_ns: 0`
  in this repo was a reporting bug, not a rendering bug: the pipeline said "0
  milliseconds" because nobody checked whether the query had ever resolved.

## Research discipline

Research is expected to go past arXiv. Prefer, in order: ACM Digital Library
and SIGGRAPH proceedings, IEEE journals and conferences, ACM TOMS / TOG, USENIX
 ATC/OSDI, NVIDIA and AMD developer documentation, and peer-reviewed
 dissertations. arXiv is a legitimate source but is neither peer-reviewed nor
 archival, so a preprint is cited as a preprint and never as the settled
 position on a question.

For each technique adopted, record in `docs/research/`:

- **Source.** Full citation with venue, year, and a stable identifier (DOI,
  ACM DL, IEEE Xplore). A blog post is acceptable for vendor API behaviour and
  must be labelled as vendor documentation rather than as evidence.
- **The claim being borrowed**, stated so it can be checked.
- **Novelty assessment.** Is this novel? Almost never. Say plainly what is
  standard, what is a combination that appears unusual, and what would actually
  be new. A combination of two published techniques is a combination, not an
  invention.
- **Why it fits here**, and what it costs. Dependencies, determinism
  consequences, platform assumptions.
- **What was measured.** Numbers from this codebase, not from the paper. The
  paper's speedup is not this engine's speedup.

If a step is plumbing, a config change, or a bug fix with no technique
involved, say so and skip the research section rather than padding it.

## Architecture and dependencies

- **The dependency budget is one package manager (CPM) and one third-party
  library (GoogleTest).** Adding a dependency requires justifying it against
  that, in the commit message. "It is common" is not a justification; Phase 0
  exists precisely because of what dependency sprawl cost this codebase.
  Diagnostics, allocation, maths and threading are all in-tree for this reason.
- **core does not depend on render, and neither depends on the application's
  headers.** A type in the wrong layer is a design error, not a convenience.
- **Public headers must compile without Vulkan present.** Use `#ifdef`, never
  `#if OMNICPP_HAS_VULKAN` — that macro is defined with no value, so `#if`
  silently evaluates false and the Vulkan include vanishes. This has cost real
  build cycles.
- **Determinism is a product feature.** Anything reachable from a tick must not
  read wall-clock time, iterate an unordered container, or depend on thread
  scheduling. If a change makes a recorded run unreproducible, that is a
  regression even when every test passes.
- **Deleting is a feature.** Dead compatibility shims, superseded plans and
  draft documents are removed, not maintained. Check before assuming something
  is dead: `omni_scripts` looked like scaffolding and turned out to be the live
  implementation behind 17 imports.

## Dogfooding repositories

When a capability needs a substantial consumer, that consumer does **not** grow
in this repository. The engine stays a library; consumers live in their own
repos that depend on it — the pattern used for rust-in-cpp.

Put something in `examples/` only when it is small enough to read in one sitting
*and* is needed to prove an engine feature. A full application, editor front
end, asset pipeline or modeler is its own repo.

When creating one:

- Separate repository, own CI, depends on this one as a package or submodule.
- It exercises the public API only. If it needs an `engine/` internal, that is
  an API gap and the fix belongs here, not there.
- Its existence is evidence: an engine claim that only its own tests support is
  weaker than one an external consumer depends on.

Current dogfooding repos: none yet. The viewport (`examples/viewport`) is still
small enough to live here, and moving it is a natural early split.

## Verification baseline

Nothing is claimed without these, on the `default` preset:

- fresh `git clone` → `cmake --preset default` → build → 73 shaders compile
- `ctest` 6/6
- full unit suite under `VK_LAYER_KHRONOS_validation`: 0 diagnostics, 0 leaks
- `tools/live_proof.py all` over real sockets: 64/64
- viewport configurations (hdr, bloom, rt+bloom, no-hdr, node editor): clean
- pixel A/B against the previous commit where behaviour should be unchanged

All four presets build warning-free on both compilers. `-Wunused-result` is
  paid (86 -> 0, and it hid a deadlock) and so is `-Wsign-conversion`
  (104 -> 0). GoogleTest is built from source here, so its own TUs are compiled
  with our flags; warnings from it are relaxed per-target (`-w`) because they
  are not actionable and they mask ours. The second stage is the one worth remembering: clang hit zero first,
  and a separate `-DCMAKE_CXX_COMPILER=g++` probe then reported 203 warnings
  clang never emitted. A "warnings are clean" claim measured on one compiler
  is the same mistake as a correctness claim measured at one size.

  That GCC gap contained the only memory-safety findings in the whole
  campaign: 14 `-Wdangling-pointer` and 10 `-Wnull-dereference`. Paying them
  also found a logic bug no compiler had reported -- `default_registry()`
  assigned `all_ok = registry.register_type(...)` once per type, so each
  registration OVERWROTE the previous result and only the last one counted,
  directly under a comment claiming "every registration is checked". The four
  `all_ok` reads GCC could not see through are now `&& all_ok`.

  The sanitizer presets were the last thing standing, and they build a
  DIFFERENT tree -- Debug, `WARPLOOM_USE_VULKAN=OFF` -- which compiles code
  the default configuration never reaches. That configuration started at 124
  GCC warnings. Paying them was mostly mechanical (`[[maybe_unused]]` on
  `#ifdef OMNICPP_HAS_VULKAN` parameters and on Vulkan-only test helpers, int
  loop counters that fed an int accumulator) and surfaced one more real bug:
  12 document parsers reused the cast-before-range-check with the bound
  `v > static_cast<double>(UINT64_MAX)`. `(double)UINT64_MAX` rounds UP to
  2^64, so exactly 2^64 passed the guard and `static_cast<std::uint64_t>(2^64)`
  is undefined. Reachable from a `.warploom` document, so from untrusted
  input. All 12 now go through `checked_double_to_uint64`.

  All four presets now build warning-free on GCC, and memory safety is
  verified rather than assumed:

  - `asan-ubsan`: 540 tests, 0 AddressSanitizer and 0 UndefinedBehaviorSanitizer
    diagnostics, 0 leaks; plus 39 editor, 10 core, 19 ui and 10 runtime tests,
    all clean. 6/6 ctest under sanitizers.
  - `tsan`: 540 tests, 0 ThreadSanitizer data races. The first real
    verification of the job system's concurrency claims.
  - `headless-debug`: builds clean, and is what CI's live-proof job uses.

  Along the way both compilers also paid `-Wunused-result` (86, hid a
  deadlock), `-Wsign-conversion` (104), `-Wshorten-64-to-32` (24),
  `-Wdouble-promotion` (37), `-Wmissing-field-initializers` (21),
  `-Wunused-variable` (23), `-Wshadow` (10), `-Wold-style-cast` (9),
  `-Wunused-function` (7), and the bugs listed above: an uninitialised
  `tlas_capacity` sum, a `switch` with no default and no trailing return in
  `bridge_control_command`, a 36-bit colour literal silently losing its alpha
  byte, `%3d` applied to `std::size_t` in a printf, a `[[nodiscard]]` reply
  discarded by a test fixture, a literal `\n` inside an `#include` that meant
  `<cstdint>` was never included, and 15 parsers casting a `double` to an
  integer before range-checking it.
- The H-Z graph barrier contract had no executing test in ANY configuration:
  it lived inside an XCB-gated swapchain test, behind
  `defined(VK_USE_PLATFORM_XCB_KHR)` -- a macro the unit-test target is never
  given -- so its body was preprocessed out everywhere while the suite counted
  it as coverage. Deeper: `begin_frame` calls `vkAcquireNextImageKHR`, so the
  renderer's whole frame loop was unexercisable without a window system;
  `record_commands` appeared in exactly one test file, the gated one. Fixed
  with `initialize_headless`, which shares ONE body with `initialize()` (a
  first draft duplicated it and missed the fence-signaled flag and the
  timeline decision within thirty lines) and
  `HiZGraph.ContractHoldsOnARealFrame`, which now runs everywhere and was
  validated by sabotage: corrupting the depth use's initial_layout fails it;
  corrupting an unrelated layout constant does not, which is how the first
  sabotage attempt was found to be a no-op.

  The first headless run also caught a real VUID the swapchain path masks --
  submit_frame signaled the binary render_finished semaphore every frame and
  only the present ever waits on it -- and, via the leak check that had been
  counting nothing, a production bug: `VulkanRenderPass::~VulkanRenderPass`
  called `cleanup(nullptr)`, which frees nothing, so every render pass leaked
  its framebuffer, depth image, view and memory. The class now captures its
  device at create().

  The compose-chain extraction (B3b) claimed to be behaviour-preserving, and
  that claim is now verified rather than assumed, on both paths:

  - LDR live path: the no-HDR capture is BYTE-IDENTICAL across the extraction
    (commit 515c6e6 vs the chain-class commit): same 2,764,816-byte PPM, same
    md5, from deterministic stepped frames. The screenshot route was tried
    first and abandoned -- this machine's window manager lists three stale
    viewports whose windows carry no _NET_WM_PID, so a grabbed frame could not
    be attributed to a build. The capture path is the honest instrument: it
    reads back the exact framebuffer.
  - HDR live path: pinned by a new golden hash,
    `VulkanHardware.HeadlessComposeFrameGoldenHash`, which renders a triangle
    through headless-compose and reads the tonemapped result back
    (canonical_hash 16601212335261142594 on the RTX 2060). The same test was
    transplanted onto the pre-extraction commit -- its
    `initialize_headless` gained the same present-format parameter the
    extraction added, since the old signature could not express a headless
    compose target at all -- and produced the identical hash. Old and new
    agree bit-for-bit on the tonemapped frame.

  Process note worth keeping: the first "leak" the new golden test reported was
  PHANTOM -- the test binary had silently failed to rebuild (an
  allocator.cleanup(device) signature error) and the stale binary, which
  predated the cleanup calls, was what validation was reporting on. A build
  that fails must be treated as evidence of nothing, not as a passing suite
  with an odd leak. The real leak class it uncovered secondhand -- a test-local
  VulkanPipeline needs explicit cleanup, the destructor is deliberately inert --
  is handled by following the file's existing cleanup convention.

  Viewport configuration matrix, re-run after the frame-loop and compose-chain
  changes (hdr, bloom, no-hdr, rt, node editor): all five run on the RTX 2060
  with 0 validation diagnostics, 0 errors, until killed by timeout -- which is
  the pass condition, since a render loop exits only on ESC.

  The two swapchain tests were dead for the same reason, and are now alive:
  `VK_USE_PLATFORM_XCB_KHR` is defined for the unit-test target and the target
  links xcb (test-only; no engine target touches a window system). On a runner
  with a display both execute -- 8 rendered frames, readback and golden hash on
  the surface path, and a recreation stress loop -- which also gives the H-Z
  contract a second, independent execution through the real swapchain instead
  of only the headless framebuffer. This machine has `DISPLAY=:0`, so they run
  here; on a headless CI runner they skip at runtime for a stated reason
  rather than not existing.

- ~~A rare race in `SystemScheduler.ParallelExecutionRunsIndependentSystems-
  Concurrently`, roughly one full-suite run in five.~~ FIXED. Now that `tsan`
  builds, ThreadSanitizer reproduced it with full stacks: `run_parallel`
  incremented its completion counter *outside* the mutex and locked only to
  `notify_one`, so a worker could finish the increment and still be inside
  `notify_one` when the main thread's predicate turned true, woke, and
  destroyed the mutex and condvar. Destroying a `condition_variable` while
  another thread is notifying it is undefined. Replaced with C++20 atomic
  `wait`/`notify_one`, where the notify is part of the atomic operation so the
  window does not exist and there is no separate object whose lifetime can end
  early.

  Honest note on the evidence: the race was *observed* by TSan on the old code
  with complete stacks, and 57 hammer runs of the new code are clean -- but
  this machine runs at load 37 on 6 cores, so the old code also went 20 runs
  clean in a later batch. The A/B is therefore not statistically conclusive.
  What makes the fix certain is structural, not statistical: the old ordering
  was undefined by the standard, and the new one has no window to hit.
