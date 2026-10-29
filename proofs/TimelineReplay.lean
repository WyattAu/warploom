/-
  Capture-log integrity, formalized (Loop 7 of 10).

  Models `CommandRecorder` in
  `modules/core/include/warploom/core/command_recorder.hpp` (`frame_`
  plus the stamped log; steps advance the frame, commands stamp it) and
  the replay argument in `docs/replay-format.md`: loading replays from
  the opening checkpoint plus the log, so anything in neither is
  unrepresentable.

  Proved:
  1. `recSortedStep` / `recSortedCmd` — stepping and recording preserve
     log-frame monotonicity (and the stamp bound) from the empty
     recorder. This is the property the file writer's command/checkpoint
     merge relies on for the log side.
  2. `replay_sum` — prefix replay from an opening state reaches exactly
     `opening + sum(cmds)`.
  3. `unlogged_replace_diverges` — the counter-model: an unlogged
     wholesale replace lands outside everything prefix-replay can reach.
-/

/-- Recorder state: logical frame plus the stamped log. Commands are
    abstracted to `Nat` payloads (the argument is about stamping, not
    about what commands mean). -/
structure Rec where
  frame : Nat
  log : List (Nat × Nat)
deriving DecidableEq, Repr

/-- A step advances the logical frame (the C++ `frame_ += ticks`). -/
def recStep (s : Rec) (ticks : Nat) : Rec :=
  { s with frame := s.frame + ticks }

/-- Recording stamps the CURRENT frame (the C++ `log_.push_back`). -/
def recCmd (s : Rec) (c : Nat) : Rec :=
  { s with log := s.log ++ [(s.frame, c)] }

/-- Log frames are nondecreasing (pairwise on the stamped frame). -/
def FramesSorted (l : List (Nat × Nat)) : Prop :=
  l.Pairwise (fun a b => a.1 ≤ b.1)

/-- Every stamped frame is at most the current frame. -/
def StampsBounded (s : Rec) : Prop :=
  ∀ f c, (f, c) ∈ s.log → f ≤ s.frame

/-- The joint invariant. -/
def RecOk (s : Rec) : Prop :=
  FramesSorted s.log ∧ StampsBounded s

/-- Empty recorder is fine. -/
theorem recOkEmpty : RecOk ⟨0, []⟩ := by
  constructor
  · simp [FramesSorted]
  · intro f c h
    simp at h

/-- Steps preserve the invariant (frame only grows, log untouched). -/
theorem recSortedStep (s : Rec) (h : RecOk s) (ticks : Nat) :
    RecOk (recStep s ticks) := by
  obtain ⟨hsorted, hbound⟩ := h
  constructor
  · exact hsorted
  · intro f c hmem
    exact Nat.le_trans (hbound f c hmem) (Nat.le_add_right _ _)

/-- Recording preserves the invariant (the new stamp is the current
    frame, hence ≥ every older stamp and ≤ the frame). -/
theorem recSortedCmd (s : Rec) (h : RecOk s) (c : Nat) :
    RecOk (recCmd s c) := by
  obtain ⟨hsorted, hbound⟩ := h
  constructor
  · -- pairwise over log ++ [new]: old part sorted, new part trivial,
    -- every old stamp ≤ the new stamp by the bound
    show List.Pairwise (fun a b => a.1 ≤ b.1) (s.log ++ [(s.frame, c)])
    rw [List.pairwise_append]
    refine ⟨hsorted, List.pairwise_singleton _ _, ?_⟩
    intro x hx y hy
    have hy' : y = (s.frame, c) := List.mem_singleton.mp hy
    subst hy'
    obtain ⟨a, b⟩ := x
    exact hbound a b hx
  · intro f c' hmem
    change (f, c') ∈ s.log ++ [(s.frame, c)] at hmem
    rw [List.mem_append] at hmem
    cases hmem with
    | inl h => exact hbound f c' h
    | inr h =>
      have heq : (f, c') = (s.frame, c) := List.mem_singleton.mp h
      rw [Prod.mk.injEq] at heq
      obtain ⟨rfl, rfl⟩ := heq
      exact Nat.le_refl _

/-- Prefix replay: fold command payloads over the opening state. -/
def applyAll : Nat → List (Nat × Nat) → Nat
  | s, [] => s
  | s, (_, c) :: rest => applyAll (s + c) rest

/-- Prefix replay reaches exactly opening + sum of payloads. -/
theorem replay_sum (opening : Nat) (log : List (Nat × Nat)) :
    applyAll opening log = opening + (log.map Prod.snd).sum := by
  induction log generalizing opening with
  | nil => simp [applyAll]
  | cons hd tl ih =>
    simp only [applyAll, List.map_cons, List.sum_cons]
    rw [ih]
    omega

/-- Counter-model: an unlogged wholesale replace to a state outside
    `opening + sum(cmds)` cannot be produced by prefix replay. The C++
    side rejects such replaces during capture instead of writing files
    that diverge here. -/
theorem unlogged_replace_diverges :
    ∀ (opening replaced : Nat) (log : List (Nat × Nat)),
      replaced ≠ opening + (log.map Prod.snd).sum →
      applyAll opening log ≠ replaced := by
  intro opening replaced log hne heq
  rw [replay_sum] at heq
  exact hne heq.symm
