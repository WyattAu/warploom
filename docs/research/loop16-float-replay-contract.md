# Loop 16 — determinism, float and replay contract (research loop 6 of 10)

Status: notes + fix, 2026-10-26.

## Sources

- **Goldberg, "What every computer scientist should know about
  floating-point arithmetic", ACM Computing Surveys 23(1):5-48, 1991,
  DOI 10.1145/103162.103163** (peer-reviewed, archival). IEEE 754
  semantics: rounding, exactness of the basic ops, why summation order
  changes results. Claim borrowed: FP addition is not associative, so
  anything that changes reduction order (vector width included) changes
  fingerprints.
- **Jolt Physics architecture docs** (jrouwe.github.io/JoltPhysics, vendor
  documentation) + discussion #617: `CROSS_PLATFORM_DETERMINISTIC` exists
  as an explicit ~8%-cost build option — determinism is a build
  configuration, not a code property alone. Same lesson as this loop's
  fix, from an engine a decade older.
- **Godot Rapier determinism docs** (godot.rapier.rs, vendor
  documentation): Rapier is deterministic AND cross-platform deterministic
  (the latter at a perf cost); the sharp line is "same initial
  conditions" must include HIDDEN state (their serialization functions
  exist for exactly this), and their warning that scene-tree/rendering/
  input/scripting sit outside the physics state maps directly onto our
  contract: the replay stream (ticks + events + step + mode) IS the
  initial-conditions list, and anything not in it (eval-local dt maps —
  loop 15; thread arrival order on the mpsc transport) is not replayed.
- **Fiedler, "Fix Your Timestep"** (gafferongames.com, practitioner canon,
  labelled as such not as evidence): fixed-step accumulator with capped
  catch-up. Our runtime already follows it; the loop pins the exactness
  property of the integer-ns path.
- **Our code**: `DeterministicRuntime` (both time modes),
  `Replay<C>`/`verify_replay`, `cmake/CompilerFlags.cmake`,
  `tests/unit/test_deterministic_runtime.cpp`.

## Claims being borrowed

1. Same source + same flags must imply same codegen, or fingerprints are
   host-dependent. (Goldberg for the mechanism; Jolt for the
   configuration-shaped solution.)
2. Replay determinism = fixed step + complete initial conditions + exact
   accumulation. (Fiedler; Rapier's hidden-state warning.)

## Novelty assessment

None. The fix deletes a host-dependent flag the repo's own comment
already questioned, and the proof writes down the accumulator algebra
the code already implements.

## What fits Warploom, and what it costs

**The defect: `-march=native` in Release codegen.** AGENTS.md forbids
host-dependent flags ("fingerprints must be a pure function of source +
flags"); `cmake/CompilerFlags.cmake` appended `-march=native` to every
non-cross Release build — including the `headless-release` preset whose
description is literally "Optimized deterministic runtime build". Native
bakes the host CPU into the binary: AVX-512 machines vectorize reductions
8-wide where AVX2 machines go 4-wide, changing FP summation order and
hence physics fingerprints from identical sources. Measured on this host
(i5-9400F, skylake): native resolves to skylake tuning with AVX2+FMA —
i.e. x86-64-v3 plus a name — so the single-machine A/B shows only label
and scheduling diffs (expected: one machine cannot exhibit
cross-machine divergence; that impossibility is the argument FOR a fixed
baseline, which is host-independent by construction). Fix: fixed
`-march=x86-64-v3` on x86_64 (both compilers here accept it); other
architectures keep the compiler default (already a portable baseline);
cross builds unchanged. Cost: pre-AVX2 x86 hosts (pre-2013) lose Release
support — acceptable and now explicit, instead of silently producing
divergent fingerprints there. The unused-by-presets `clang-linux.cmake`
toolchain still says native; it is dev-shell legacy, out of scope, left
with a pointer to this note rather than silently "fixed" halfway.

**Pinned alongside: integer-mode composition.** The property that makes
`integer_nanoseconds` the replay mode — split advances compose exactly —
had no test. Added `IntegerModeSplitsComposeExactly` (1.0 vs 0.3+0.7 vs
10x0.1: identical ticks/accumulator/hash, 60 ticks). The floating-point
path deliberately gets NO such test: its documented drift is real, and
pinning equality would be pinning an accident.

**Audited, no change:** the float->int casts on the replay path
(`fixed_step_ns_`, `header(step)`, `advance_integer` elapsed). NaN is
safe (the constructor's `> 0` check rejects it; `elapsed < 0` likewise —
NaN comparisons are false, so NaN takes the default/clamp arm, never the
cast); huge values need a ~292-year clock jump to overflow. Same-binary
replay is unaffected. Recorded so the audit is not repeated.

**Latent, not fixed:** the mpsc transport mixes concurrently-posted
events into the state hash in arrival order — two producers racing give
different hashes run to run. Single-producer posts are ordered and
deterministic (tested); making multi-producer arrival deterministic
needs sequence numbers in the event type, which is a replay-format ABI
change. Recorded with the trigger (real concurrent producers in
production); the engine default stays spsc.

## What was measured

- `IntegerModeSplitsComposeExactly` green; full unit suite + ctest below.
- `headless-release` (the preset that takes the changed RELEASE path)
  configures, builds warning-free, and its tests pass — the flag change
  compiles and runs on the affected configuration, not just Debug.
- Default (Debug) configure + build: 0 errors/warnings; the change is a
  no-op there by construction (only `CMAKE_CXX_FLAGS_RELEASE` edited).
- Lean `proofs/FloatContract.lean`: `adv_compose` (split = whole, ticks
  AND residue), `adv_residue_bound`, from an explicit single-step axiom
  nowhere — all machine-checked, no axioms, no sorries.
