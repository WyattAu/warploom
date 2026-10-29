# Loop 14 — ECS scheduling and storage (research loop 4 of 10)

Status: notes + fix, 2026-10-26.

## Sources

- **Cox et al., "Run-time Performance Comparison of Sparse-set and Archetype
  Entity-Component Systems", CGVC 2025** (peer-reviewed workshop;
  eprints.staffs.ac.uk/9315; code at
  github.com/StaffsUniGames/cgvc25-ecs-comparison). Game-of-Life benchmark:
  sparse-set and archetype statistically indistinguishable at 100/1,000
  entities; archetype wins from ~10,000 (Mdn 1.721 vs 2.945 ms at 10k, ~2x
  at 50k); sparse-set wins add/remove by ~6.6x. Read from the PDF (via the
  eprints landing page — direct PDF fetch is unsupported in this
  environment) and the Mighty Professional summary of the same numbers.
- **Bevy ECS schedule docs** (docs.rs/bevy/latest/bevy/ecs/schedule):
  schedule as a system DAG; `MultiThreadedExecutor` runs non-conflicting
  systems in parallel; conflicts are component-access conflicts, and
  ordering edges (`before`/`after`, sets, chains) ALWAYS force separation
  regardless of access disjointness. Ambiguity detection warns when the
  conflict analysis is incomplete.
- **EnTT wiki, "Entity Component System"** (github.com/skypjack/entt/wiki):
  sparse-set model, no announced component list, per-type pools, pay-per-use
  specialization. The design authority for "sparse-set wins structural
  churn": each component lives in its own pool, so add/remove never moves
  unrelated components.
- **Flecs, "Building an ECS #3: Storage in Pictures" (Mertens, 2024)**:
  archetype as database table (SoA columns + entity-id array), archetype
  graph for O(1) add/remove transitions once edges exist, tags/ZSTs stored
  without columns, sparse components stored OUTSIDE the archetype (explicit
  callout to EnTT's design). The hybrid reference: archetype iteration +
  sparse overflow for rarely-present data.
- **Our code**: `modules/core/include/warploom/render/ecs.hpp`
  (`World` archetype tables + `SystemScheduler` mask/DAG waves);
  `tests/unit/test_ecs_foundation.cpp`.

## Claims being borrowed

1. Below ~10k entities with structural churn, sparse-set-class iteration is
   not the bottleneck; archetype migration cost dominates. (Cox et al.)
2. Parallel system execution is sound only when BOTH access-disjointness
   AND ordering edges separate the pair. (Bevy: conflicts + DAG.)
3. Sparse components (data outside the table) make add/remove O(1) without
   moving rows. (Flecs §6, crediting EnTT.)

## Novelty assessment

Nothing here is novel. Our ECS is an archetype-table store (Flecs-shaped)
with sparse-set-style per-type arrays and a Bevy-shaped schedule (DAG +
conflict waves). The combination is ordinary; the fix below is a
correctness repair, not an invention.

## What fits Warploom, and what it costs

Our scale is three orders below the archetype crossover (scenes are
hundreds of entities, and the gd draw-loop profile shows query iteration
is not hot — compose dominates). So: NO storage migration. The measurable
trigger stands: revisit only above 10k entities with hot structural churn
(loop 11 closed this the same way; this loop re-verified rather than
re-decided).

What DID change is scheduling soundness. `partition_into_waves` considered
only component-mask overlap, ignoring `depends_on`. Two systems with
disjoint masks but a dependency edge shared a wave and ran concurrently —
the exact class Bevy separates by construction (ordering edges always win
over access analysis). The fix makes waves dependency-aware: a system
joins a wave only if all its known dependencies are already scheduled in
EARLIER waves and no wave member depends on it. Cost: one name→index map
per partition (same shape `build_schedule` already builds); no per-tick
allocation beyond the waves vector, which `run_parallel` already made.

The parallel contract is now written down: same-wave systems must not do
structural `World` changes (add/remove entity or component). Disjointness
covers component data, not the archetype table itself — same limitation as
Bevy's `Commands` deferral, minus the deferral mechanism (out of scope;
`run()` stays the structural path).

## What was measured

- New tests, both green:
  `DependentSystemsNeverShareAWaveDespiteDisjointMasks` (wave count 2, plus
  50 `run_parallel` rounds where the consumer observes the producer's
  write) and `IndependentDisjointMasksShareOneWave` (no over-serialization).
- Sabotage: the wave-count assertion fails under the old mask-only rule by
  construction (disjoint dependent pair collapses to one wave).
- Full `SystemScheduler.*` suite: 6/6 pass.
- Lean `proofs/EcsScheduler.lean`: wave model with `AllWavesOk` invariant
  (pairwise mask-disjoint + no intra-wave dep edge + deps in earlier
  waves). Schedule-level soundness (`buildWaves_sound` + the three
  corollaries) is machine-checked; the single-step preservation
  (`placeOne_preserves`) is an explicitly stated axiom, not a proof —
  the case analysis over `findWave`/`placeOne` is routine but long and
  is not claimed as verified. The C++ side carries the sabotage-checked
  tests instead (wave-count assertions fail under the old mask-only rule
  by construction: the disjoint dependent pair collapses to one wave).
