# Loop 20 — synthesis: audit, converged contracts, kickoff (research loop 10 of 10)

Status: audit + one implementation (end-state verification), 2026-10-26.

This loop closes the 10-loop series (loops 11–19). It does three things:
re-checks the loops against EACH OTHER (a fix that contradicts another
loop's contract is a regression even if its own tests pass), writes down
the converged spec deltas in one place, and kicks off the highest-value
next implementation. Lean work this loop is audit-grade: full `lake
build` green plus a per-proof status table (one stated axiom, not hidden).

## 1. Decision table (what each loop adopted, rejected, deferred — and why)

| Loop | Adopted | Rejected / not adopted | Deferred (with trigger) |
|------|---------|------------------------|-------------------------|
| 11 frontier audit | history-never-alias invariant (R6); ECS crossover closed at 10k entities; NRD/DLSS deferral sharpened | neural denoisers, MGPBD/PAA deformable solvers, ReSTIR (reservoir state unsnapshottable), Vcc shaders, work-graph mesh nodes | ReSTIR re-evaluation (lights > ~64 OR replay blob section for solver state) |
| 12 physics validator | double-step `step_with_check`; XPBD compliance knob (default 0, bit-identical) | speculative contacts / TOI-TOI (needs tunneling stress test as gate) | island sleeping (needs pile-settles verification run) |
| 13 graph lifetimes | pure `derive_frame_resources` (history ⇒ Dedicated) | transient aliasing in planner; full R6b Vulkan wiring | R6b wiring (dedicated change + verification run) |
| 14 ECS scheduling | dependency-aware waves; parallel contract written down | storage migration (three orders below crossover) | structural-change deferral mechanism (`Commands`-equivalent) |
| 15 scripting dataflow | graph-owned per-node prev-time map; 1..9 pin contract; lambda-local error sink | exec-wire model (user-visible order = replay-visible state) | `noise1d` lattice cast (unproven document reachability) |
| 16 float contract | `-march=x86-64-v3` fixed baseline; integer-ns composition test | pinning FP-path equality (real drift, would pin an accident) | mpsc arrival-order hash (needs event-ABI sequence numbers + real concurrent producers) |
| 17 timeline capture | reject mid-capture loads; auto-embed warp targets | — (fix deletes a false doc claim) | — |
| 18 asset streaming | KTX2 256 MiB cap; 64-bit `valid()` | Basis/transcoding adoption (device-dependent bytes break replay); feedback-without-residency streaming | tile residency (residency must land first) |
| 19 GPU LOD | `min(count, bound)` at all three LOD sites | pyramid-reuse proof (cited Greene, not claimed) | pyramid-reuse proof; occlusion as its own loop |
| 20 (this) | load-time end-state verification (below) | session graph-clock wiring (unprompted behavior change — needs a time-source decision) | see §4 |

No loop contradicts another loop's adoption: the deferrals all share one
rationale (state the replay stream cannot snapshot is not admitted), and
every adopted fix is identity on the inputs the other loops' tests use
(verified by the full suite staying green throughout, ending 645/645 +
14/14 core).

## 2. Cross-loop consistency audit (checked, not asserted)

- **Loop 15 × Loop 17 (eval state vs replay prefix).** The hazard: replay
  re-simulates from the opening checkpoint with an EMPTY prev-time map,
  while the captured run may have had continuity — IF session graph time
  advances. Measured: it does not — `set_context` has ZERO callers, so
  session `sync_graph` evaluates at frozen default time and script dt is
  uniformly 0 live and in replay. Consistent today BY CONSTRUCTION, and
  the doc-replacement paths (`scrub_to`, `load_replay` restore) already
  reset the map by replacing the graph. Recorded trigger (not a fix):
  the day session graph time is wired to sim time, `capture_start` must
  reset eval state or first-tick dt diverges from replay. Prescribed,
  not implemented — a reset today would be a behavioural no-op.
- **Loop 16 × Loop 17 (event order vs log order).** The mpsc latent
  (arrival-order hash mixing) stands, but the capture log records in
  arrival order and replay applies in log order — single-producer (the
  engine default, spsc) is exactly reproducible. No contradiction; the
  multi-producer trigger is unchanged.
- **Loop 13 × Loop 19 (history invariant vs pyramid reads).** The H-Z
  pyramid IS the previous-frame read the never-alias invariant was
  written for; Loop 19's occlusion path samples it conservatively and
  claims no reuse proof. The invariant holds the door the deferral
  points at. Consistent.
- **Loop 14 × Loop 15 (shared-state direction).** Same-wave structural
  ban (schedule side) and lambda-local error sink + graph-owned dt map
  (eval side) both remove process-global/type-global mutable state from
  concurrent paths. Same direction, no overlap (different state).
- **Loop 18 × Loop 19 (bound untrusted input).** Same discipline at two
  layers (allocation cap before use; index clamp before use). Sizing
  cross-check: the 256 MiB KTX2 cap admits at most 8192² RGBA, inside
  the RTX 2060's 32768 maxImageDimension2D — no driver-OOM path opens
  between the cap and the hardware limit.
- **Loop 12 × Loop 16 (validator vs flags).** The validator is
  debug-only; the `-march` baseline is release codegen. Same-binary
  claim intact on both axes.
- **Loop 11 deferral stack vs everything.** No denoiser / ReSTIR / NRD
  precisely because temporal state is unsnapshottable — which is also
  why Loop 13's invariant and Loop 17's log-only replay exist. One
  rationale, three consequences. Consistent.

## 3. Spec deltas (the "rewrite", kept minimal — each loop already updated its spec inline)

- `docs/replay-format.md` §Load semantics point 4: "readers MAY verify
  and warn" → "the loader MUST verify and FAIL". The writer always
  embeds the closing checkpoint, so the old wording permitted silent
  divergence by policy. (This loop.)
- Everything else the series needed is already in-tree at its loop:
  R6 history invariant, ECS parallel contract, `reset_eval_state`
  replay contract, replay-format load/reject rules, KTX2 cap budget,
  shader capacity comments. No further rewrite — a rewrite that changes
  nothing is process theatre.

## 4. Implementation kickoff (this loop's build)

**End-state verification in `load_replay`.** The format promised
"replays from the opening checkpoint plus the log" but never checked
the landing: any live-run effect outside the log (host ticks never
issued as commands) wrote a file that loaded "successfully" onto
divergent bytes. Now, after re-apply, the loader hashes the document
and compares against the closing checkpoint, failing loudly with both
hashes and the frame. Cost: one `to_json` + hash per load. Verified:
new test crafts exactly the hole (5 unlogged host ticks under a
3-entry log) — fixed build rejects naming the closing checkpoint;
sabotaged build (check neutralised) loads silently and the test fails.
All 14 core + 645 unit tests green with the check on, so every tested
flow already satisfies the property it now enforces.

The check itself survived contact with the suite: the first version
compared against the checkpoint at the greatest FRAME, and the
pre-existing `FreshSessionLoadReproducesByteIdenticalState` round-trip
failed — a mid-file `ScrubStart` can target a frame AHEAD of the final
log position, so greatest-frame picks a mid-file record instead of the
closing one. The closing checkpoint is the LAST record in file order
(`stop()` embeds it after every command and every earlier checkpoint;
the writer's interleave flushes remaining checkpoints in vector order
once the log is exhausted). That existing test is now the check's
strongest guard: it pins that legitimate files (mid-file future-frame
checkpoints included) still load byte-identically.

**Not kicked off (decision-laden, stated with triggers):** session
graph-clock wiring (needs a time-source decision: sim frame vs host
time — changes frozen-clock behaviour every session depends on);
mpsc sequence numbers (replay-format ABI change); tile residency;
R6b Vulkan wiring; island sleeping. See the table in §1.

## 5. Lean status (audit, 2026-10-26: `lake build` clean, no warnings)

PhysicsDeterminism, FrameAllocator, EcsScheduler, DataflowEval,
FloatContract, TimelineReplay, AssetBounds, LodSelect — all build.
Known proof debt (stated, not hidden): `EcsScheduler.placeOne_preserves`
is an explicitly stated axiom (single-step preservation; the case
analysis over `findWave`/`placeOne` is routine but long). Everything
else is axiom-free (`#print axioms` clean by construction — no
`sorry`s; the build would fail on them).

## 6. Open-item register (consolidated — each with its tripwire)

1. ReSTIR/spatial-reuse (lights > ~64 or replay blob section).
2. Island sleeping (pile-settles verification run).
3. R6b Vulkan wiring (dedicated change + verification run).
4. `noise1d` lattice cast (prove document reachability or bound it).
5. mpsc arrival-order hash (real concurrent producers → ABI change).
6. Session graph clock → capture-start eval reset (wire time → add reset).
7. Tile residency before streaming feedback.
8. Pyramid-reuse proof (occlusion loop).
9. `lod_count == 0` payload underflow (a writer emits 0 — none does).
10. `clang-linux.cmake` legacy `-march=native` (dev-shell only; pointer
    left at Loop 16 — do not "fix halfway").

A step that closes any row must do what every loop here did: cite the
technique, borrow from the closest shipped prior art, prove the bound
in Lean where a bound exists, verify by sabotage, and land green.
