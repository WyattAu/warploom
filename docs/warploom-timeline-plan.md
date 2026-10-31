# warploom timeline: G3 dependency analysis and design (gate doc)
  (`tools/headless_host.cpp`) has no sim loop; properties change only
  disarm; see interplay below).
**Recording/playback is session-side, host-ticked, deterministic**; direct
recovery path). A clip's track stores **property samples**; recording a
projection of (document clips, scrubber checkpoints); that is why G3 was
Frames, not seconds; the engine's clock is deterministic ticks; the
before the first sample); linear interpolation is a documented v1.1
| `ClipRecordStop` | `clip_record_stop` | - | yes |
| `ClipStop` | `clip_stop` | - | yes |
| `ClipsInfo` | `clips_info` |; (query → JSON array) | no (query) |
they enter the table; the protocol invariant is enforced, not remembered.
- **`tick_timeline(frame)`**; new host-ticked hook, called once per sim
  if both drive the same property; documented order):
  undo restores the pre-edit document INCLUDING the track as captured -
re-simulated by `load_replay` (a headless re-applier has no sim loop) -
**DONE**; `ClipTimelineView` landed in `warploom-editor`; the contract
   the engine (byte-determinism). Mitigation: the hash sentinel; the
   auto-disarms at the end; the clip's own length is the bound.