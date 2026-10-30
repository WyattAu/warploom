/-
  Frame-graph alias planner, formalized (Loop 2 of 10).

  Models `plan_resource_allocations` from
  `modules/render/src/warploom/render/vulkan_frame_allocator.cpp`:
  greedy most-recent-fit over declaration order, sharing only between
  same-size/same-tag resources with disjoint live intervals, where a
  history resource spans the whole timeline [0, t] and therefore overlaps
  everything.

  Proved:
  1. `planKind_history_dedicated` — a history resource always plans
     Dedicated (the never-alias-history invariant, structurally).
  2. `planKind_aliased_sound` — an Aliased entry always names a compatible
     resource with a disjoint live interval.
  3. `planKind_owner_earlier` (R6b) — an Aliased entry's owner is declared
     strictly earlier, so the store's declaration-order allocation always
     materializes the owner's memory before any sharer binds it.
-/

structure FResource where
  first : Nat
  last : Nat
  history : Bool
  size : Nat
  tag : Nat
deriving DecidableEq, Repr

inductive PlanKind where
  | dedicated : PlanKind
  | aliased (owner : Nat) : PlanKind
deriving DecidableEq, Repr

/-- Live interval: history spans the whole timeline, transients their own. -/
def liveL (t : Nat) (r : FResource) : Nat × Nat :=
  if r.history then (0, t) else (r.first, r.last)

/-- Inclusive overlap on both ends (matches `overlaps` in the C++ file). -/
def ivOverlap (a b : Nat × Nat) : Prop := a.1 ≤ b.2 ∧ b.1 ≤ a.2

instance ivOverlapDec (a b : Nat × Nat) : Decidable (ivOverlap a b) :=
  inferInstanceAs (Decidable (_ ∧ _))

/-- Same size AND same layout tag (matches `compatible`). -/
def compat (a b : FResource) : Bool := a.size == b.size && a.tag == b.tag

/-- Most-recent-fit search over a declaration-order pfx: returns the
    highest index that is compatible and disjoint, or none. -/
def findOwner (t : Nat) (ri : FResource) : List FResource → Option Nat
  | [] => none
  | rj :: rest =>
    match findOwner t ri rest with
    | some k => some (k + 1)
    | none =>
      if _h : compat rj ri = true ∧ ¬ ivOverlap (liveL t rj) (liveL t ri) then
        some 0
      else none

/-- The plan entry for index i: search the strict pfx `take i`. -/
def planKind (rs : List FResource) (t i : Nat) : PlanKind :=
  match rs[i]? with
  | none => .dedicated
  | some ri =>
    match findOwner t ri (rs.take i) with
    | none => .dedicated
    | some j => .aliased j

/-- Soundness of the search: a found owner is really compatible, disjoint,
    and present at that index. -/
theorem findOwner_sound (t : Nat) (ri : FResource) (pfx : List FResource)
    (j : Nat) (h : findOwner t ri pfx = some j) :
    ∃ rj, pfx[j]? = some rj ∧ compat rj ri = true ∧
      ¬ ivOverlap (liveL t rj) (liveL t ri) := by
  revert j
  induction pfx with
  | nil =>
    intro j h
    simp only [findOwner] at h
    cases h
  | cons hd tl ih =>
    intro j h
    simp only [findOwner] at h
    by_cases htl : ∃ k, findOwner t ri tl = some k
    · obtain ⟨k, hk⟩ := htl
      simp only [hk] at h
      cases h
      obtain ⟨rj, hmem, hc, hdis⟩ := ih k hk
      exact ⟨rj, by simpa using hmem, hc, hdis⟩
    · have hnone : findOwner t ri tl = none := by
        match hm : findOwner t ri tl with
        | some k => exact absurd ⟨k, hm⟩ htl
        | none => rfl
      simp only [hnone] at h
      by_cases hcond :
        compat hd ri = true ∧ ¬ ivOverlap (liveL t hd) (liveL t ri)
      · rw [dite_eq_left hcond] at h
        cases h
        exact ⟨hd, by simp, hcond.1, hcond.2⟩
      · rw [dite_eq_right hcond] at h
        cases h

/-- A history resource spanning [0, t] overlaps every well-formed interval,
    so the search can never return an owner for it. -/
theorem findOwner_none_of_history (t : Nat) (ri : FResource)
    (pfx : List FResource) (hh : ri.history = true)
    (wf : ∀ r, r ∈ pfx → r.first ≤ r.last ∧ r.last ≤ t) :
    findOwner t ri pfx = none := by
  by_cases h : ∃ j, findOwner t ri pfx = some j
  · obtain ⟨j, hj⟩ := h
    obtain ⟨rj, hmem, _hc, hdis⟩ := findOwner_sound t ri pfx j hj
    have hmem' : rj ∈ pfx := List.mem_iff_getElem?.mpr ⟨j, hmem⟩
    have hwf := wf rj hmem'
    have hri : liveL t ri = (0, t) := by simp [liveL, hh]
    have hover : ivOverlap (liveL t rj) (liveL t ri) := by
      unfold ivOverlap
      rw [hri]
      constructor
      · by_cases hrjh : rj.history
        · simp [liveL, hrjh]
        · simp only [liveL, hrjh]
          exact Nat.le_trans hwf.1 hwf.2
      · exact Nat.zero_le _
    exact (hdis hover).elim
  · match hm : findOwner t ri pfx with
    | some j => exact absurd ⟨j, hm⟩ h
    | none => rfl

/-- Wrapper 1: history plans Dedicated. -/
theorem planKind_history_dedicated (rs : List FResource) (t i : Nat)
    (ri : FResource) (hr : rs[i]? = some ri) (hh : ri.history = true)
    (wf : ∀ r, r ∈ rs → r.first ≤ r.last ∧ r.last ≤ t) :
    planKind rs t i = .dedicated := by
  have hfind :=
    findOwner_none_of_history t ri (rs.take i) hh
      (fun r hm => wf r (List.mem_of_mem_take hm))
  simp only [planKind, hr, hfind]

/-- A found owner index is within the searched prefix (by the search's own
    structure: the tail case returns a shifted tail hit, the head case 0). -/
theorem findOwner_some_lt_length (t : Nat) (ri : FResource)
    (pfx : List FResource) (j : Nat)
    (h : findOwner t ri pfx = some j) : j < pfx.length := by
  revert j
  induction pfx with
  | nil =>
    intro j h
    simp only [findOwner] at h
    cases h
  | cons hd tl ih =>
    intro j h
    simp only [findOwner] at h
    by_cases htl : ∃ k, findOwner t ri tl = some k
    · obtain ⟨k, hk⟩ := htl
      simp only [hk] at h
      cases h
      have hlt := ih k hk
      simp only [List.length_cons]
      omega
    · have hnone : findOwner t ri tl = none := by
        match hm : findOwner t ri tl with
        | some k => exact absurd ⟨k, hm⟩ htl
        | none => rfl
      simp only [hnone] at h
      by_cases hcond :
        compat hd ri = true ∧ ¬ ivOverlap (liveL t hd) (liveL t ri)
      · rw [dite_eq_left hcond] at h
        cases h
        simp only [List.length_cons]
        exact Nat.zero_lt_succ _
      · rw [dite_eq_right hcond] at h
        cases h

/-- Wrapper 3 (R6b): an Aliased entry names a STRICTLY EARLIER owner.
    The store allocates in declaration order, so this is what guarantees the
    owner's memory exists before any sharer binds it -- no separate guard. -/
theorem planKind_owner_earlier (rs : List FResource) (t i j : Nat)
    (ri : FResource) (hr : rs[i]? = some ri)
    (h : planKind rs t i = .aliased j) : j < i := by
  have hfind : findOwner t ri (rs.take i) = some j := by
    have h2 :
      (match findOwner t ri (rs.take i) with
        | none => PlanKind.dedicated
        | some k => PlanKind.aliased k) = PlanKind.aliased j := by
      simpa [planKind, hr] using h
    match hm : findOwner t ri (rs.take i) with
    | none =>
      have hded : planKind rs t i = .dedicated := by
        simp only [planKind, hr, hm]
      rw [hded] at h
      cases h
    | some k =>
      have halk : planKind rs t i = .aliased k := by
        simp only [planKind, hr, hm]
      rw [halk] at h
      -- The goal here is `some k = some j` (the match abstracted the
      -- discriminant), so `cases h` substitutes k := j and leaves `rfl`.
      -- This differs from planKind_aliased_sound, whose goal never mentions
      -- the discriminant, so it can still use hm afterwards.
      cases h
      rfl
  have hlt := findOwner_some_lt_length t ri (rs.take i) j hfind
  have hlen : (rs.take i).length = min i rs.length := List.length_take
  omega

/-- Wrapper 2: an Aliased entry names a compatible, disjoint owner. -/
theorem planKind_aliased_sound (rs : List FResource) (t i j : Nat)
    (ri : FResource) (hr : rs[i]? = some ri)
    (h : planKind rs t i = .aliased j) :
    ∃ rj, (rs.take i)[j]? = some rj ∧ compat rj ri = true ∧
      ¬ ivOverlap (liveL t rj) (liveL t ri) := by
  have h2 :
    (match findOwner t ri (rs.take i) with
      | none => PlanKind.dedicated
      | some k => PlanKind.aliased k) = PlanKind.aliased j := by
    simpa [planKind, hr] using h
  match hm : findOwner t ri (rs.take i) with
  | none =>
    have hded : planKind rs t i = .dedicated := by
      simp only [planKind, hr, hm]
    rw [hded] at h
    cases h
  | some k =>
    have halk : planKind rs t i = .aliased k := by
      simp only [planKind, hr, hm]
    rw [halk] at h
    cases h
    exact findOwner_sound t ri (rs.take i) j hm
