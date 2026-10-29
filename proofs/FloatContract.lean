/-
  Fixed-step accumulator contract, formalized (Loop 6 of 10).

  Models the integer-nanosecond advance path in
  `modules/core/include/warploom/core/deterministic_runtime.hpp`
  (`advance_integer`): ticks and residue are exact `Nat` division and
  remainder of the pooled nanoseconds. All quantities are `Nat` — the
  point of the integer path is that no float ever appears.

  Proved:
  1. `adv_ticks_add` / `adv_residue` — one advance yields quotient and
     remainder of the pool.
  2. `adv_compose` — splitting one advance into pieces gives exactly the
     same total ticks and final residue (the property that makes the
     integer path the replay mode; the C++ side pins it in
     `IntegerModeSplitsComposeExactly`).
  3. `adv_residue_bound` — the residue is always below one step.
-/

/-- One advance: pool the elapsed ns onto the accumulator, take whole
    steps as ticks, keep the remainder. `step` is fixed_step_ns. -/
def adv (step acc elapsed : Nat) : Nat × Nat :=
  let total := acc + elapsed
  (total / step, total % step)

/-- Ticks taken are the quotient of the pool. -/
theorem adv_ticks_add (step acc elapsed : Nat) :
    (adv step acc elapsed).1 = (acc + elapsed) / step := by
  simp [adv]

/-- Residue kept is the remainder of the pool. -/
theorem adv_residue (step acc elapsed : Nat) :
    (adv step acc elapsed).2 = (acc + elapsed) % step := by
  simp [adv]

/-- Composition: advancing by e1 then e2 from acc equals advancing by
    e1 + e2 at once, both in ticks and in residue. Needs step > 0
    (a zero step would divide by zero; the C++ constructor clamps the
    step away from non-positive). -/
theorem adv_compose (step acc e1 e2 : Nat) (hstep : 0 < step) :
    let (t1, r1) := adv step acc e1
    let (t2, r2) := adv step r1 e2
    (t1 + t2, r2) = adv step acc (e1 + e2) := by
  have hdm : step * ((acc + e1) / step) + (acc + e1) % step = acc + e1 :=
    Nat.div_add_mod _ _
  -- Reassociation with the quotient/remainder abstracted: no div/mod in
  -- sight, so plain rewrites suffice (keeps `step * q` away from omega).
  have key : ∀ (a q r : Nat), step * q + r = a →
      a + e2 = (r + e2) + step * q := by
    intro a q r h
    rw [←h, Nat.add_assoc, Nat.add_comm (step * q)]
  have harg : acc + (e1 + e2) = ((acc + e1) % step + e2) +
      step * ((acc + e1) / step) := by
    rw [←Nat.add_assoc acc e1 e2]
    exact key _ _ _ hdm
  have hticks : (acc + e1) / step + ((acc + e1) % step + e2) / step
      = (acc + (e1 + e2)) / step := by
    rw [harg, Nat.add_mul_div_left _ _ hstep]
    exact Nat.add_comm _ _
  have hres : (((acc + e1) % step + e2) % step)
      = (acc + (e1 + e2)) % step := by
    rw [harg, Nat.add_mul_mod_self_left]
  show (((acc + e1) / step + (((acc + e1) % step + e2) / step)),
         ((((acc + e1) % step + e2) % step)))
       = ((((acc + (e1 + e2))) / step), (((acc + (e1 + e2))) % step))
  rw [Prod.mk.injEq]
  exact ⟨hticks, hres⟩

/-- The residue never reaches a full step (it is always a remainder). -/
theorem adv_residue_bound (step acc elapsed : Nat) (hstep : 0 < step) :
    (adv step acc elapsed).2 < step := by
  simp only [adv]
  exact Nat.mod_lt _ hstep
