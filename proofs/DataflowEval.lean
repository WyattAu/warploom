/-
  Dataflow evaluation determinism, formalized (Loop 5 of 10).

  Models the script-node dt path in
  `modules/core/src/warploom/render/node_graph.cpp`
  (`NodeGraph::evaluate_with`) and the removed shared-state bug from
  `script_node.cpp`: per-node dt is a pure function of the node's own
  time sequence; one cell shared by a whole node type forces every node
  after the first to observe dt = 0 at a shared timestamp.

  Times are `Nat` (the argument is about sequencing, not reals).

  Proved:
  1. `dtSeqGo_spec` — the per-node delta fold is exactly the pairwise
     difference against the previous stamp (first stamp yields 0).
  2. `sharedSecond_isZero` — the shared-cell counter-model from the old
     code: on the second tick the second node observes 0 while the
     per-node rule gives t2 - t1.
  3. `runEval_deterministic` — pull evaluation (a fold of pure node
     functions over a fixed topo order) is the pure fold, so the same
     order + same seeds give the same outputs: replay needs no order log.
-/

/-- Per-node delta fold: first stamp yields 0, each later stamp yields
    the difference against the node's own previous stamp. -/
def dtSeqGo (prev : Nat) : List Nat → List Nat
  | [] => []
  | t :: ts => (t - prev) :: dtSeqGo t ts

/-- Full per-node sequence over one node's tick history. -/
def dtSeq : List Nat → List Nat
  | [] => []
  | t :: ts => 0 :: dtSeqGo t ts

/-- The fold is exactly pairwise differences. -/
theorem dtSeqGo_spec (prev : Nat) (ts : List Nat) :
    dtSeqGo prev ts = List.zipWith (· - ·) ts (prev :: ts) := by
  induction ts generalizing prev with
  | nil => rfl
  | cons t rest ih =>
    simp only [dtSeqGo, List.zipWith]
    exact congrArg _ (ih t)

/-- First element is always 0 (documented first-call behaviour). -/
theorem dtSeq_first (t : Nat) (ts : List Nat) :
    (dtSeq (t :: ts))[0]? = some 0 := by
  simp [dtSeq]

/-- The old code's shared cell: one (last, first) pair threaded through
    EVERY node of the type, in evaluation order. -/
def sharedStep (last : Nat) (first : Bool) (t : Nat) : Nat × Nat × Bool :=
  if first then (0, t, false) else (t - last, t, false)

/-- Counter-model, mirroring the sabotage test: two nodes A, B of one
    type, ticks t1 then t2, topo order A before B. The shared cell gives
    B dt = 0 on the second tick; the per-node rule gives t2 - t1. -/
theorem sharedSecond_isZero (t1 t2 : Nat) :
    let (dtA1, l1, f1) := sharedStep 0 true t1
    let (dtB1, l2, f2) := sharedStep l1 f1 t1
    let (dtA2, l3, f3) := sharedStep l2 f2 t2
    let (dtB2, _, _) := sharedStep l3 f3 t2
    dtB2 = 0 ∧ dtA2 = t2 - t1 ∧ dtA1 = 0 ∧ dtB1 = 0 := by
  simp [sharedStep]

/-- Per-node rule on the same scenario: B's second-tick dt is t2 - t1. -/
theorem perNodeSecond (t1 t2 : Nat) :
    dtSeq [t1, t2] = [0, t2 - t1] := by
  rfl

/-- Pull evaluation: stores map pin ids to values, nodes are pure
    store functions, a run is the fold over a fixed order. -/
abbrev Store := List (Nat × Nat)

def runEval : List (Store → Store) → Store → Store
  | [], s => s
  | f :: fs, s => runEval fs (f s)

/-- The recursive run is the pure fold: no hidden state can intervene,
    so the same order + same seeds give the same outputs. -/
theorem runEval_deterministic (order : List (Store → Store)) (s : Store) :
    runEval order s = order.foldl (fun s f => f s) s := by
  induction order generalizing s with
  | nil => rfl
  | cons f fs ih => exact ih (f s)
