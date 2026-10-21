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
- **Floating-point determinism contract.** `-ffp-contract=off` is set on every
compiler that supports it: FMA contraction under -O2 is the most dangerous
cross-build nondeterminism source (two compiles of the same source can produce
different physics fingerprints). Do not add `-march=native`-style flags or
platform libm calls (sin/cos/tan) reachable from a tick — the replay contract
depends on the op sequence being a pure function of source + flags.

**Determinism is a product feature.** Anything reachable from a tick must not
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

- fresh `git clone` → `cmake --preset default` → build → 74 shaders compile
- `ctest` 6/6
- full unit suite under `VK_LAYER_KHRONOS_validation`: 0 diagnostics, 0 leaks
- `tools/live_proof.py all` over real sockets: 71/71 headless, plus the visual-host half of `e1` (E1 added eight: render mode and exposure wire contract; E2/R4 added three: set_bloom/get_bloom round-trip on the headless host, and R1b added the pt mode round-trip -- applied, back to forward, re-entered, left again -- on the viewport. The bloom payload-validation and read-back assertions are the visual-host half of the E1 proof -- they run when the harness detects a viewport, where the toggle was exercised live on the RTX 2060: on, reads back on, off, reads back off, missing-arg rejected)
- viewport configurations (hdr, bloom, rt+bloom, no-hdr, node editor, pt): clean
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

- **Six path-tracing bugs, found by wiring the viewport (R1b).** Three were
  mine and none of them were visible to a spec: (1) the acceleration-structure
  build barrier was emitted only BETWEEN BLAS builds, so the TLAS reused the
  scratch buffer unordered -- one mesh worked, two meshes reproducibly lost
  the device with NO validation error, because a scratch race is undefined
  behaviour rather than a spec violation; (2) one shared geometry buffer with
  per-mesh offsets, an obvious "optimisation", faults this GPU during the
  build; (3) the frame counter did not advance on the clearing frame, so every
  frame re-cleared with the same seed and the viewport showed a converged image
  that had never accumulated anything; (4) the accumulation image lacked
  `VK_IMAGE_USAGE_SAMPLED_BIT`, invisible until a real tonemap sampled it.
  The teaching point is the metric: the first convergence assertion compared
  MEAN brightness, which ACES + gamma compress into a few percent -- so a
  sabotaged build with no 1/n divide PASSED. It now compares the spatial noise
  floor, which is what actually distinguishes a converging average, and both
  sabotage directions are caught.

  Three more only the LIVE path could find, because the headless test drives
  the objects directly: (5) the orbit camera kept moving, so every round
  traced a different pose and averaged a smear -- PT now holds the camera;
  (6) the renderer had ONE frame pre-pass hook and the viewport installed the
  PT round and then, 20 lines later, the shadow pass over it, so PT's
  callback never ran again while every signal said it was. Two owners of one
  hook is an API gap, so the hook is now primary + a small chain. And the
  capture chain composed from its own (raster) HDR target, so captured frames
  showed a plausible picture of something the viewport was not displaying; the
  tell was that the captured mean matched the baseline capture to three
  decimals.

- **The viewport had no resize path at all, and a resizable window that
  silently ignores resizes is worse than a fixed one.** `STRUCTURE_NOTIFY` was
  already selected on the XCB window, so `ConfigureNotify` events had been
  arriving and being dropped on the floor; meanwhile everything downstream of
  the swapchain read the compile-time constants `kWidth`/`kHeight`, not the
  live extent. Added: the event handler, `apply_resize()` (recreate swapchain ->
  rebuild the sized depth attachment and framebuffers -> `resync_for_swapchain`
  -> rebuild the capture stack -> rebuild the PT accumulation -> relayout the
  UI), and `set_viewport_size` on the control socket (v1.11), so the path is
  driven on demand and is provable in CI rather than only by a window manager.

  **The root cause of the crash this took four attempts to find is worth the
  space.** A resize with frame capture enabled device-lost the GPU
  (`VK_ERROR_DEVICE_LOST` on the next submit) roughly half the time; the same
  resize with capture OFF was always clean, and under the validation layer it
  was always clean too. A validation layer that says nothing while the device
  dies is the signature of undefined behaviour rather than a spec violation --
  the same shape as the BLAS scratch-barrier bug above, and the tell that
  stopped me trusting the "it passes under validation" signal. Three wrong
  hypotheses died on the way, and each is a trap worth naming:
    1. "the render pass must be recreated" -- it does not have to be, and doing
       so changes nothing.
    2. "the capture compose chain caches the old render pass, so rebuild it" --
       correct, necessary, and NOT sufficient.
    3. "it is a timing race the fence wait should cover" -- `wait_idle()` is
       `vkDeviceWaitIdle`, which already drains every queue.

  The actual fault was the one object nobody resized: the capture's own
  colour target, render pass and framebuffer, which are sized at
  `FrameCapture::initialize` and were still at 1280x720 while the swapchain
  and the compose chain had moved. Rebuilding the whole capture stack --
  target AND chain -- at the new extent is the fix, and after it: five
  consecutive resizes with capture on, zero device losses, and the captured
  PPM headers follow the live extent exactly (1280x720 -> 1000x600 -> 1400x820
  -> 800x480 -> 1200x700 -> 960x540).

  Path tracing rebuilds too, and correctly RE-CONVERGES: an accumulation
  buffer is sized in pixels, so the old samples describe an image that no
  longer exists and the counter resets. Measured across a resize, the noise
  floor falls again from scratch (33.96 -> 18.2 at 1000x600; 33.70 -> 5.16 at
  1400x820 over 200 frames). Carrying samples across would composite a blurry
  low-resolution average as though it had converged.

  The viewport also now sets `_NET_WM_PID`, which is what makes a window
  attributable to a build -- two viewports share the title, so title matching
  is not an alternative, and anything that needs to know which window belongs
  to which build (a screenshot grab, a resize driver) simply could not.

  **And the first version of that fix did not work, which is the more
  interesting half.** The property was written against an atom interned
  *later*, in a different function, so it went out against atom 0 and the
  server dropped it: `xprop -id <w> _NET_WM_PID` said "not found" and
  `xdotool search --pid` found nothing. The symptom was blamed on the window
  manager, and the belief survived several rounds of measurement because
  "the WM strips it" sounds like an environmental explanation and therefore
  like a reason to stop looking. Moving the interning before the write fixed it:
  `xprop` now reports the running PID. The lesson is the one this repository
  keeps paying for: an environmental explanation is a hypothesis like any
  other, and it is worth exactly as much as a measurement.

- **The use-after-free class does not repeat, which was checked rather than
  assumed.** Having found a dispatch table cached by `VkDevice` handle value,
  the next question is whether the pattern appears elsewhere. It does not: all
  nine `vkGetDeviceProcAddr` sites audited (only the AS builder cached; the RT
  pipeline already resolves per call), no file-scope mutable state in
  `modules/render/src`, and the three function-local `static` registries were
  each read rather than pattern-matched -- `ScriptModule::load_builtin` sets
  `handle_ = nullptr`, so a builtin is never `dlclose`d and its registered
  function pointers cannot dangle. A negative result, recorded so the search is
  not repeated.

  The suite now also runs under `--gtest_shuffle`: 626/626 under a random order
  and under three fixed seeds. A process-global registry plus a test that
  asserts "already registered" is exactly the pairing that manufactures order
  dependence silently, and shuffling is the cheapest way to know whether one
  exists.

- **A configured exposure was silently discarded, and it took a 20x change
  that produced a byte-identical frame to notice.** `ensure()` preserves the
  live exposure between calls, because `set_exposure` writes it mid-run and
  `record_commands` calls `ensure()` every frame -- a plain `config_ = config`
  would reset it before each record, which is what the two-exposure probe
  caught. But the preservation was also applied on the FIRST `ensure`, where
  there is no live value to preserve: it overwrote the caller's exposure with
  the default. `bloom` was already guarded by `if (!first_ensure)`; exposure
  was not, so the two drifted apart.

  It was invisible to every brightness assertion, because the default (1.0) and
  the wrong answer produce equally "reasonable" pictures. What exposed it was
  changing `cc.exposure` from 0.05 to 1.0 during an unrelated experiment and
  getting a **byte-identical** frame: a 20x exposure change that changes
  nothing is not something a tolerance-based check will ever flag.

  The regression test then passed a sabotaged build for the same reason -- it
  configured 1.0, which is exactly the value the bug leaves in place. Neither
  value it configures may be the default now. A regression test whose chosen
  value coincides with the buggy value tests nothing, and that is worth
  remembering independently of the bug it was written for.

- **Three of the unit cube's six faces were wound backwards, and with
  back-face culling on, a backwards-wound face does not shade wrongly -- it
  disappears.** `build_unit_cube` emits six quads with a stored normal each;
  computing the right-hand-rule normal of each quad's OWN winding and
  comparing it to the stored normal showed +X, -X and +Y all inverted. The
  pipeline's `frontFace` is CCW and `enable_backface_cull` defaults to true.
  The ground is that same unit cube scaled (8, 0.1, 8), so its +Y face is its
  TOP: culled, the ground vanished, and the viewport showed a black void with
  a lit sliver of the ground's edge. The path tracer was unaffected (ray
  tracing ignores winding), which is exactly why comparing the tracer's
  picture against the raster one is what finally showed it -- the tracer
  looked RIGHT and the raster path looked wrong, and the difference was a
  winding, not a lighting bug.
  The lesson is the one this project keeps paying for: a picture that looks
  right is not evidence, and a difference between two pictures is a question,
  not a diagnosis. The face normal that is STORED is not the normal that is
  RENDERED; the winding is a second, independent statement about the same
  geometry, and nothing checks that the two agree.
  Verified by looking: the ground's top face now appears, and the suite is
  unchanged (628/628, ctest 6/6, ASan 555, clang and GCC 0 warnings).

- **A use-after-free that presented as "a flaky ray-tracing test", and was
  neither flaky nor ray-tracing-specific.** `ray_query_first_contact` segfaulted
  the host about one full-suite run in six. The core dump put frame #0 inside
  `libnvidia-gpucomp.so.615.71.09` at one fixed offset, reached from
  `vkGetAccelerationStructureBuildSizesKHR` via `query_blas_sizes`. Two things
  made it look like a test flake and it was not:
  1. **It was not one test.** Under `--gtest_repeat=3` the crash moved to
     `VulkanHardware.PathTracedFrameConvergesThroughCompose` via
     `VulkanPathTracer::create` -- same offset, same frame #1. Two crash sites
     with an identical top frame are one defect, not two flaky tests. The
     repeat harness also raised the rate from ~1/6 to ~2/3, which said the
     trigger was cumulative within the process rather than per-test.
  2. **The fault was ours, not the driver's.** `rt_for()` cached the RT entry
     points in a process-global keyed by `VkDevice` **handle value**. Vulkan
     reuses handle values after destruction, so a new device landing on a
     freed handle compared equal, the cache skipped re-resolution, and every
     later call jumped through pointers resolved from the DESTROYED device.
     That is exactly the observed asymmetry: the suite builds and tears down
     ~40 `VulkanContext`s so handle reuse is near-certain, while a single test
     creates one device and reuses no handle -- hence never in isolation
     (0/12).

  A second, independent defect surfaced on the way: `cmd_build_blas` filled in
  `vertexData.deviceAddress` and `vertexStride`, and `query_blas_sizes` left
  both at zero, so the size query described "12 triangles at address 0, stride
  0". A size query may read the vertex data, and reading address 0 is a host
  fault in the driver with no validation error to explain it. Both paths now
  share one `fill_blas_geometry`, because two copies of a struct description
  that must agree is precisely how they drifted.

  A/B on one harness, three `--gtest_repeat=3` runs each: original 1/3 crashed,
  fixed 0/3; then 6 runs at `--gtest_repeat=4` (24 suite passes) with zero
  crashes, and 6 consecutive plain full-suite runs under validation with zero
  failures where it previously failed ~1 in 6.

  The general lesson is the one worth keeping: **an intermittent crash is a
  symptom, not a diagnosis, and "it only happens in the full suite" is a
  clue rather than an excuse.** Two rounds of hypothesis-chasing here were
  wrong (missing usage bits, leaked AS objects) before the coredump plus a
  repeat harness turned it from a guess into a location.

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
