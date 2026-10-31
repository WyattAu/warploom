# warploom-replay-v1: protocol record/replay file format (W2)
unchanged; still `warploom-replay-v1`, `schema_version` 1.
   session; the command log plus hash-verified state snapshots; in one
- `created_unix`: writer's wall clock, informational only; never used for
- `args`: the command's parsed payload in **positional form** -
  from these fields; replay does NOT re-parse wire syntax. Ids are
**Recorded kinds**; every *mutating* kind (the document-edit set:
host-mirrored visual state (`set_camera`, `set_sun`, `capture`; no
`scrub_to` time warp does NOT rewind the logical frame in v1; it is an
the command log alone does NOT carry tick side effects; a replay loader
Counts are advisory (readers may cross-check); `end` is required; a file
  checkpoint (that is exactly what a scrub_start does; it captures).
   commands are NOT re-recorded when a capture is active; the loaded log
   produced; documented, not hidden: the failure is reported, the
4. The document ends at the state after the final command; which, for a
- `stop_capture {path}`: writes the file (atomic tmp+rename, chmod 0600 -
  checkpoint; self-consistent by construction.
callers serialize. Recording adds no nondeterminism; it observes commands