# Loop 15 — scripting and dataflow determinism (research loop 5 of 10)

Status: notes + fix, 2026-10-26.

## Sources

- **Halbwachs et al., "LUSTRE: a declarative language for real-time
  programming", POPL 1987, DOI 10.1145/41625.41641** (peer-reviewed,
  archival; also Proc. IEEE 1991). Synchronous dataflow: programs are
  systems of equations over streams; evaluation is a pull in topological
  order and is deterministic BY CONSTRUCTION — no execution order is
  user-visible, so there is nothing for replay to record beyond the input
  streams. Claim borrowed: purity + topo-order pull = replay-free
  determinism.
- **Epic Games, "Functions in Unreal Engine"** (vendor documentation, read
  from dev.epicgames.com): pure vs impure functions. Impure functions MUST
  be explicitly executed by connecting exec wires; pure nodes have no exec
  pins and are evaluated on demand as parameters. The exec model exists
  precisely because side effects need a visible order — which is also why
  it is the wrong model for anything recorder-visible.
- **Godot docs, "Using VisualShaders"** (vendor documentation,
  docs.godotengine.org): visual-shader nodes have input/output ports and no
  execution model — pure dataflow, matching our graph.
- **Our code**: `NodeGraph::evaluate_with` (pull phase + topo order),
  `register_script_node_type` bound/unbound variants, `ScriptModule` C ABI
  host; tests `test_script_node.cpp`, `test_node_persistence.cpp`
  (NodeScriptNodes).

## Claims being borrowed

1. Deterministic replay requires the PULL model for recorder-visible
   scripting: nodes are pure functions of (params, inputs, context) and
   evaluation order is an implementation detail. (Lustre; our graph
   already had this shape — the loop verifies rather than adopts.)
2. Ordering state must live in the SCHEDULE, not in the node type.
   (Unreal's exec wires are the counterexample: user-visible order is
   replay-visible state.)

## Novelty assessment

Nothing novel. The fix restores the pull-model invariant the graph
already claimed; Lustre published the principle in 1987.

## What fits Warploom, and what it costs

Two real defects, both on the script-node path (the math/builtin nodes
were already pure):

1. **Shared per-type TickState (bound `script:<name>` nodes).** dt lived
   in the TYPE lambda's captured state, shared by every node instance of
   that type. The second node evaluated at a timestamp always saw dt=0
   (its sibling had just stamped the shared time), and re-evaluating the
   same time gave a different dt — the header's "deterministic under
   replay" was false, and the existing `DtFollowsTheTickSequence` test
   could not see it (single node; replay in a FRESH graph). Fix: dt is
   computed by `NodeGraph::evaluate_with` from a graph-owned ordered
   per-node previous-time map and arrives in the per-node context copy
   (`GraphContext::dt`, new field); the lambda is stateless. Cost: one
   ordered map, 8 bytes per evaluated node; no signature change for the
   5 existing builtin `context_evaluate` lambdas (they ignore the field).
   Replay contract documented on the new `reset_eval_state()`: eval-local
   prev-times are run state, not snapshot state — re-simulation starts
   from a snapshot (empty) or after reset.
2. **Unbounded double->size casts + process-global error sink (unbound
   `script` node).** `inputs`/`outputs` params went through a blind
   `static_cast<size_t>`: 1e18 = bad_alloc, -1 = wrap — the same
   cast-before-range-check class as the 12 fixed parsers. Now through
   `checked_double_to_uint64` with a 1..9 pin-contract bound; violations
   degrade to zeros on out0 (the node's established total-failure shape,
   cf. unknown-module). The file-scope `g_error_scratch` string on the
   eval path is now a lambda-local (it was mutable process-global state:
   a race under any concurrent evaluate and unsnapshotted replay state).

Explicitly NOT done: the `noise1d` lattice hash casts a host-driven time
double to uint64 without a range check (UB for |t| ~ 1e19). Same class,
unproven reachability from documents (time comes from the host/clock
path, not from a param). Recorded as open, not fixed.

## What was measured

- New tests, all green: `TwoNodesOfOneTypeGetIndependentDt` (both nodes
  see dt=0.5 on the second tick), `EvalStateResetsForANewHistory`
  (same-object re-eval gives dt=0; reset + replay reproduces the
  sequence), `OutOfContractPinCountsDegradeToZeroWithoutCrashing`
  (10 / -1 / 1e18 / 1.5 / "one" / 0 all evaluate true with out0=0).
- Sabotage A/B (real rebuild, not reasoning): the old shared-state
  `script_node.cpp` FAILS `TwoNodesOfOneTypeGetIndependentDt`; the fixed
  file passes. The 1e18 case crashed the old path by construction
  (22 000 000 000 000 000-element vector request).
- Targeted suites 11/11; full unit suite + ctest below.
- Lean `proofs/DataflowEval.lean`: per-node dt as a pure function of the
  time sequence (`dtSeq` nth lemma), shared-cell counter-model
  (`sharedSecond_isZero`), pull-fold purity corollary.
