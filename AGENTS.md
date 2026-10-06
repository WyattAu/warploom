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
  Both clang and GCC are at ZERO in the default configuration, reached in two
  stages. The second stage is the one worth remembering: clang hit zero first,
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

  Still open, and this is the honest status: the sanitizer presets build a
  different tree (`WARPLOOM_USE_VULKAN=OFF`, Debug) and that configuration
  still carries 124 GCC warnings under
  `-DWARPLOOM_WARNINGS_AS_ERRORS=OFF` -- 60 `-Wfloat-equal`, 29 unused
  variables, 12 `-Wmissing-declarations`, 8 unused parameters, 11 sign or
  value conversions, 3 `-Wredundant-move`. So `asan-ubsan` and `tsan` still
  do not build and memory safety is still unverified by sanitizers. The
  clang campaign that reached zero also paid
  `-Wunused-result` (86, hid a deadlock), `-Wsign-conversion` (104),
  `-Wshorten-64-to-32` (24), `-Wdouble-promotion` (37),
  `-Wmissing-field-initializers` (21), `-Wunused-variable` (23),
  `-Wshadow` (10), `-Wold-style-cast` (9) and `-Wunused-function` (7), and it
  found real bugs on the way: an uninitialised `tlas_capacity` sum, a
  `switch` with no default and no trailing return in `bridge_control_command`,
  a 36-bit colour literal silently losing its alpha byte, `%3d` applied to
  `std::size_t` in a printf, a `[[nodiscard]]` reply discarded by a test
  fixture, and three parsers casting `double` to an integer before
  range-checking the value.
  Until that GCC debt is paid, the only verified configuration is `default` — which
  means memory safety is currently unverified by sanitizers.
- A rare race in `SystemScheduler.ParallelExecutionRunsIndependentSystemsConcurrently`,
  roughly one full-suite run in five.
