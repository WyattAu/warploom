# Loop 17 — timeline and capture integrity (research loop 7 of 10)

Status: notes + fix, 2026-10-26.

## Sources

- **Lasseter, "Principles of traditional animation applied to 3D computer
  animation", SIGGRAPH 1987, DOI 10.1145/37401.37407** (peer-reviewed,
  archival): slow-in/slow-out — the authored justification for easing
  modes. Our `evaluate_at` Step/Linear/SmoothStep/EaseIn/EaseOut is this
  paper's vocabulary restricted to pure functions of (samples, offset).
- **Witkin & Kass, "Spacetime constraints", SIGGRAPH 1988, DOI
  10.1145/54852.54870** (peer-reviewed, archival): trajectory
  optimization subject to constraints. Related, NOT adopted: our clips
  are authored keyframes with step-hold/eased interpolation, not
  optimized trajectories; adopting it would trade the pull-model purity
  (Loop 15) for solver state the replay contract cannot snapshot.
- **Unreal Sequencer docs** (vendor documentation, dev.epicgames.com):
  sequence start frame inclusive, end frame exclusive — the same
  half-open `[start, start+length)` span our `evaluate`/`evaluate_at`
  range-check, independently converged on.
- **Godot AnimationPlayer docs** (vendor documentation,
  docs.godotengine.org): Nearest/Linear/Cubic (+Bezier) interpolation
  modes per track. Ours is per-EVALUATION mode instead (Step default
  preserves old replays bit-for-bit); the mode lives in the call, not
  the data, so old files never change meaning.
- **Our code**: `TimelineClip::evaluate`/`evaluate_at`, `tick_timeline*`,
  `CommandRecorder::is_recorded`, `EditorSession::{scrub_to,load_replay}`,
  `docs/replay-format.md`.

## Claims being borrowed

1. Eased keyframe interpolation is authored easing, not simulation
   (Lasseter). No hidden state; evaluation stays a pure function.
2. Replay from a log replays exactly what is IN the log; a state change
   with no log entry and no snapshot is, by definition, unreplayable.
   (This loop's own finding — the format doc claimed otherwise.)

## Novelty assessment

None. The fix deletes a false doc claim and enforces the format's own
load semantics (replay from opening checkpoint + log).

## What fits Warploom, and what it costs

**The defect: two wholesale-replace ops were invisible to capture.**
`LoadDocument` and `LoadReplay` replace the entire document (and reset
the command stack) but `is_recorded` excluded them — while
`docs/replay-format.md` claimed the recorder "logs them like any other
command" and the result was "self-consistent by construction". Both
halves false: nothing was logged, and the loader replays from the
single opening checkpoint plus the log, so a mid-capture load wrote a
file that silently diverged on replay. Fix: reject both during an
active capture (loud error, document untouched — the codebase already
rejects overlapping captures, so this is the established discipline,
not a new one). Cost: sessions that want to load mid-narration stop the
capture first; no format change.

**The companion hole, same file: warp targets must travel.**
`scrub_to` to a pre-capture checkpoint restored live (bytes in the
session ring) but wrote a file whose replayed `scrub_to` found no
checkpoint — a capture that cannot replay itself (loud at load, still
a defect). Fix, mirroring the existing `scrub_start`-during-capture
embed: after a successful warp with an active recorder, embed the
restored bytes at the target frame when the recorder has no checkpoint
there yet. `CommandRecorder::has_checkpoint` added for the check, so
`scrub_start`-prepared warps store no byte twice.

**Audited, no change:** the tick path iterates only ordered maps
(`tracks`, `physics_bodies_` are `std::map`); `evaluate_at` endpoint
hold, `span > 0` guard, and Bool/String hold are all present;
`tick_timeline_interpolated` is NaN-safe (`!(alpha > 0)` catches NaN)
and clamps `alpha >= 1`. The stale scrubber header NOTE (claiming
restore leaves history, when `scrub_to` itself resets the stack) now
says what the code does.

## What was measured

- New tests, all green: `LoadDocumentDuringCaptureIsRejected`,
  `LoadReplayDuringCaptureIsRejected` (loud error naming capture, doc
  byte-identical, capture still stoppable),
  `ScrubToDuringCaptureCarriesTargetBytes` (warp-to-pre-capture-frame
  file round-trips end to end and lands on the warped bytes).
- Sabotage A/B by real rebuild (guards neutralized + embed dropped):
  all 3 new tests FAIL; fixed file passes.
- Full core/unit/ctest below.
- Lean `proofs/TimelineReplay.lean`: log-frame monotonicity preserved
  by step/record from empty (`recSortedStep`, `recSortedCmd` —
  the property the file writer's merge relies on for the log side),
  plus the unrepresentability counter-model (an unlogged replace lands
  outside `opening + sum(cmds)`, which is all prefix-replay can reach).
