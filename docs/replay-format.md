# warploom-replay-v1 — protocol record/replay file format (W2)

Status: **spec for W2** (docs/roadmap.md, track W). This document is the
contract for replay files before any code lands. The schema name embeds the
target identity (`warploom`) so the S-decision identifier migration never
breaks this format's namespace. G3 (protocol v1.8) extends the *recorded
command set* with the timeline clip kinds; the file format itself is
unchanged — still `warploom-replay-v1`, `schema_version` 1.

## Goals

1. **Portable session records**: everything needed to reproduce an editing
   session — the command log plus hash-verified state snapshots — in one
   file a second Warploom instance (or a future tool) can consume.
2. **Compose with existing proofs**: checkpoints reuse the W1 scrubber's
   `{frame, hash, json}` record shape (already byte-deterministic); the
   embedded document bytes reuse the G1 document serialization. No new
   serialization machinery.
3. **Scrubber-loadable**: loading a replay hydrates the W1 `ReplayScrubber`
   so any embedded checkpoint is immediately warpable.

## Non-goals (v1)

- Recording render/camera state, telemetry, or GPU timing.
- Compression, binary encoding, chunking.
- Cross-machine guarantee beyond what the document format already promises
  (byte-deterministic JSON; see `replay_scrubber.hpp`).

## File layout

JSON Lines (UTF-8, LF endings, matching the control protocol's wire style).
Line 1 is the header; every later line is one record. Readers must ignore
unrecognized fields (forward compatibility, same rule as protocol parsing)
and reject unknown `schema_version` values.

### Header (exactly one, first line)

```json
{"record":"header","schema_version":1,"format":"warploom-replay-v1","scene":"city","created_unix":0}
```

- `record`: literal `"header"`.
- `schema_version`: `1` for this spec.
- `format`: literal `"warploom-replay-v1"`.
- `scene`: free-text scene name captured at `start_capture` time.
- `created_unix`: writer's wall clock, informational only — never used for
  determinism.

### Command records

One per protocol command observed by the recorder while capture is active,
in arrival order, stamped with the logical frame at arrival:

```json
{"record":"cmd","frame":42,"seq":7,"cmd":"spawn_cube","n":[1,2,3,0.5]}
{"record":"cmd","frame":43,"seq":8,"cmd":"set_property","n":[9,8,7],"t":"cube_2","t2":"position"}
```

- `frame`: sim frame when the command arrived (from the recorder's
  frame source; see "Frame source").
- `seq`: 0-based monotonic arrival index within the file.
- `cmd`: the protocol command name (same strings as the v1.8 mapping table).
- `args`: the command's parsed payload in **positional form** —
  `"n":[numbers[0..number_count)} when any numbers are set, plus
  `"text"`/`"text2"`/`"text3"` when non-empty. Writers echo the parsed
  command generically (no per-kind field map), so recording can never
  drift from the parser; readers rebuild the `ControlCommand` directly
  from these fields — replay does NOT re-parse wire syntax. Ids are
  inside the positional payload; replies are not recorded.

**Recorded kinds** — every *mutating* kind (the document-edit set:
spawn/destroy/set_property/undo/redo/select/node-*/bind-*, plus the G3 clip
document edits `clip_add`/`clip_remove`/`clip_move`) plus the session-arm
commands `clip_record`/`clip_record_stop`/`clip_play`/`clip_stop` (arming
shapes what the following step ticks capture and apply, so they are part of
session intent), the scrub commands and `pause`/`resume`/`step` (they shape
the sim-frame timeline and are part of session intent). NOT recorded: pure
queries (`list_objects`, `get_object`, `schema`, `get_graph`,
`list_bindings`, `scrub_info`, `clips_info`, `capture_status`, `ping`),
host-mirrored visual state (`set_camera`, `set_sun`, `capture` — no
document effect), and the v1.7 capture commands themselves
(`start_capture`/`stop_capture`/`load_replay` never nest; v1 records one
session per file).

**Frame source.** The recorder stamps commands with a *logical* frame: the
`start_capture` frame plus the accumulated `ticks` of recorded `step`
commands (the headless session has no free-running sim clock). A
`scrub_to` time warp does NOT rewind the logical frame in v1 — it is an
arrival stamp, not sim time; the restored state is carried by the command
itself, and replay reproduces both identically. Viewport hosts may pass a
richer frame source later.

**Tick contract (G3).** Since v1.8 one full session tick is
`sync_graph()` followed by `tick_timeline(frame)`: playback first (apply
step-hold values for every playing clip track at the current frame), then
record (sample the current document value into each armed clip track).
Hosts run this per `step` tick *before* the step command is recorded, so
the command log alone does NOT carry tick side effects — a replay loader
must re-execute the same contract: per logged `step` command, run its
`ticks` count of full ticks with the logical frame from the frame source
(incrementing per tick) before applying the next command. The W1
non-timeline sessions are unaffected (empty clip set makes `tick_timeline`
a no-op). Sample offsets are clip-relative, so `clip_move` never rewrites
recorded samples and replay stays byte-faithful.

**Only commands whose reply was `ok` are recorded.** A failed command had
no state effect; recording it would abort re-apply on load for no fidelity
gain. The `end` record's `commands` count is therefore the count of
recorded (successful) commands.

### Checkpoint records (two lines)

A checkpoint is a marker line followed by the raw document JSON on the next
line. Embedding the document as a nested OBJECT (not an escaped string)
keeps every line unescaped and the reader trivial: after a `ckpt` marker,
the next line IS the document bytes.

```
{"record":"ckpt","frame":42,"hash":"18446744073709551615"}
{"schema_version":2,...}
```

- `frame`: logical frame at capture.
- `hash`: decimal string of the capture-time `fnv1a64` over the document
  line's bytes (decimal string, not number, to stay lossless through any
  JSON round-trip).
- The document line: the byte-deterministic document serialization captured
  at that frame (identical bytes to what W1 stores in the ring).

### Capture end

```json
{"record":"end","frame":57,"commands":12,"checkpoints":2}
```

Counts are advisory (readers may cross-check); `end` is required — a file
without it is truncated and readers must REJECT it entirely (clean protocol
error, state untouched). No partial loads.

## Checkpoint density (settled here, per the roadmap)

**Capture start + explicit snapshots; no periodic auto-capture in v1.**

- `start_capture` itself embeds one checkpoint at the start frame.
- Each `scrub_start` command recorded during capture embeds another
  checkpoint (that is exactly what a scrub_start does — it captures).
- Everything else is the command log.

Rationale: checkpoints are the expensive, high-value records (multi-KiB
document bytes); the command log is the cheap, dense one. Deterministic
re-apply of the command log reconstructs intermediate states exactly, so
periodic snapshots would buy scrubbing granularity the re-applied log
already provides, at N× file size. A future `warploom-replay-v2` may add
`"ckpt_every": K` in the header if real sessions show re-apply is too slow
to be the default scrub path.

## Load semantics

Loading a replay file (`load_replay` protocol command, v1.7) into a session:

1. Parse + validate header (`schema_version == 1`, `format` matches). Reject
   otherwise (clean protocol error, state untouched).
2. Every checkpoint record is hash-verified (bytes → hash) and inserted into
   the session's scrubber ring.
3. **Re-apply the command log in `seq` order** through the session's normal
   `on_control` path (same mutation authority as live editing, so undo
   history, bindings, and validation behave identically), executing the
   tick contract above for every recorded `step` between commands
   (`sync_graph` + `tick_timeline` per tick, logical frame incrementing
   from the opening checkpoint's frame). Re-applied
   commands are NOT re-recorded when a capture is active — the loaded log
   IS their record. A command that fails to apply aborts the load with the
   seq, command, and error; the session state is then whatever the prefix
   produced — documented, not hidden: the failure is reported, the
   scrubber keeps the hydrated checkpoints, and `scrub_to` to the last
   good checkpoint recovers a known state.
4. The document ends at the state after the final command — which, for a
   deterministic engine, must equal the state hashed in the final checkpoint
   if the writer embedded one at capture end. (Writers SHOULD embed a
   final `scrub_start`-style checkpoint; readers MAY verify this and warn.)

Scrub-to-a-checkpoint after load is plain W1 time warp (history clears at
the warp, same as live).

## Writer semantics (v1.8 protocol)

- `start_capture {frame}` (optional `scene` string, default ""): begins
  recording. Requires capture not already active. Embeds the opening
  checkpoint at `frame`. Reply detail carries the recording state.
- `stop_capture {path}`: writes the file (atomic tmp+rename, chmod 0600 —
  same discipline as G1 saves), embeds the closing checkpoint at the current
  frame, ends recording. Requires capture active and a non-empty path.
- `capture_status`: query; detail JSON `{recording, frame, commands,
  checkpoints, path}`.
- Capture state is session-local: `load_document`/`load_replay`/`scrub_to`
  do NOT stop an active capture (the recorder logs them like any other
  command), but `stop_capture` after a load records the loaded state's
  checkpoint — self-consistent by construction.

## Threading & determinism notes

Same threading contract as the scrubber: frame-thread-only, no locks,
callers serialize. Recording adds no nondeterminism — it observes commands
post-parse and never mutates the document itself.
