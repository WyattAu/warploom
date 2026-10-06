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

Known-broken, and must not be reported as working:

- `headless-debug`, `asan-ubsan` and `tsan` configure but do not **compile**:
  they inherit `WARPLOOM_WARNINGS_AS_ERRORS=ON` and the tree carries warning
  debt. `-Wunused-result` is paid (86 -> 0, and it hid a deadlock); the
  `-Wsign-conversion` is paid too (104 -> 0). GoogleTest is built from source
  here, so its own TUs are compiled with our flags; warnings from it are
  relaxed per-target (`-w`) because they are not actionable and they mask ours.
  33 remain: `-Wfloat-equal` (16), `-Wunused-parameter` (10),
  `-Wmissing-include-dirs` (5) and `-Wmissing-braces` (2).
  `-Wold-style-cast` is paid too (9 -> 0): every one was a C-style narrowing
  cast of an index or queue family id in a test, now an explicit
  `static_cast<std::uint32_t>`. `-Wunused-function` is paid too (7 -> 0): five
  were genuinely dead test helpers, one was a dead free-function shim in
  vulkan_renderer.cpp whose replacement is the member
  `record_fullscreen_pass`, and one -- `record_hiz_contract` -- was dead for a
  reason worth knowing: `VK_USE_PLATFORM_XCB_KHR` is only defined for the
  viewport and render targets, never for the unit-test target, so the HIZ
  contract test body is preprocessed out entirely and the test always skips
  with "Vulkan XCB support was not enabled for this build". That helper is now
  behind the same condition, and the dead test is recorded here rather than
  left looking like coverage. `-Wshadow` is paid too
  (10 -> 0): the city scene's camera framing shadowed the outer orbit
  variables, and a telemetry `objects` manifest shadowed the function's
  `ScenePbrObject` list. Renamed, and the city rename was A/B'd byte-identical
  over 15 protocol commands against a pre-rename host. `-Wunused-variable` is paid too
  (23 -> 0) by deletion rather than `(void)` casts: dead code is the finding,
  and marking it used would only hide it. Two were checked before deleting --
  `grep -c port` on node_editor.cpp is 0, so the unused in_count/out_count
  cannot be needed; and the joystick's unused kTypeInit is already explained by
  the comment beneath it.
  `-Wmissing-field-initializers` is paid too (21 -> 0): 17 were Vulkan structs
  written `VkFoo info{VK_STRUCTURE_TYPE_FOO};`, which leaves every other field
  value-initialized -- correct, but it reads as "only sType is set". They are
  now `{}` followed by an explicit `.sType =`, which is the Vulkan idiom and
  says what it means. The rest were `Accepted` and ECS `System`, both of which
  now name every member at construction. `-Wdouble-promotion` is paid too (37 -> 0): every
  one of the 37 was in a test, none in engine code -- printf `%f` varargs
  require the double, and the Monte-Carlo reference is deliberately double
  against a float GPU result. Both are explicit casts now.
  `-Wshorten-64-to-32` is paid too (24 -> 0), and it found a missing cast that
  promoted a whole `tlas_capacity` sum to `size_t` before narrowing it.
  `-Wfloat-equal` is down to 16 and found a real one: three parsers read
  numbers as `double` and hand-rolled the same "is this an exact integer?"
  predicate as `v != static_cast<double>(static_cast<std::uint64_t>(v))`, which
  casts BEFORE range-checking, so `"id": 1e300` was an undefined float->uint64
  conversion. Verified, not assumed: a standalone `-fsanitize=float-cast-overflow
  -fno-sanitize-recover=all` build aborts on the old form and exits 0 on the
  replacement. One helper (`numeric_cast.hpp`) now owns it, with tests. Worth
  noting the accidental-pass trap there -- on x86-64 the bad cast yields
  0x8000000000000000, whose round-trip comparison then *fails*, so a naive
  functional test passes against the broken code. Only UBSan catches it.
  `-Wswitch-enum` was tried and deliberately dropped: it fires even when a
  switch has a `default:`, and every site here partitions an enum on purpose.
  `-Wswitch` is enabled and still catches an enum switch with no default at
  all -- it found one, in `bridge_control_command`, whose switch had no
  `default` and no trailing return, so the 35 kinds that bridge does not own
  fell off the end of a non-void function. Unreachable from its only caller,
  and still UB.

  Until that debt is paid, the only verified configuration is `default` — which
  means memory safety is currently unverified by sanitizers.
- A rare race in `SystemScheduler.ParallelExecutionRunsIndependentSystemsConcurrently`,
  roughly one full-suite run in five.
