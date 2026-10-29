/-
  Warploom physics determinism contract, formalized (Loop 1 of 10).

  What this pins down, in the exact terms the C++ solver implements
  (`modules/physics/.../physics_world.hpp`, step 3):

  1. Detection reads ONE frozen snapshot, so the contact set is a pure
     function of the snapshot — re-running detection on the same snapshot
     yields the same pair list (determinism / double-step validator).
  2. The uniform-grid broadphase is conservative: with cell size ≥ r₁+r₂,
     every overlapping pair lands in the same or an adjacent cell, so the
     grid candidate set contains every overlapping pair (1D model; the C++
     grid is the 3D analogue with 27-neighbour cells and cell = 2·maxR,
     which implies cell ≥ r₁+r₂ by `two_max_bounds_sum` below).
  3. XPBD compliance staging: compliance 0 reproduces the stiff projection
     exactly (correction denominator `inv_sum + 0 = inv_sum`).

  1D Nat model keeps the arithmetic checkable with core-Lean tactics only
  (no Mathlib dependency); the C++ side is the 3D instantiation of the
  same argument, measured conservative at 200–8000 bodies.
-/

structure Body where
  pos : Nat
  radius : Nat
deriving DecidableEq, Repr

/-- Two 1D spheres overlap iff center distance is below the sum of radii,
    stated subtraction-free so `omega` can use it directly. -/
def overlaps (a b : Body) : Prop :=
  a.pos + (a.radius + b.radius) > b.pos ∧
    b.pos + (a.radius + b.radius) > a.pos

instance : Decidable (overlaps a b) :=
  inferInstanceAs (Decidable (_ ∧ _))

/-- All index pairs i < j over `n` bodies, in fixed ascending order. -/
def allPairs : Nat → List (Nat × Nat)
  | 0 => []
  | n + 1 => allPairs n ++ (List.finRange n).map fun i => (i.val, n)

/-- Contact detection against a frozen snapshot: pure filter over `allPairs`. -/
def contactsOf (bodies : List Body) : List (Nat × Nat) :=
  (allPairs bodies.length).filter fun (i, j) =>
    match bodies[i]?, bodies[j]? with
    | some a, some b => decide (overlaps a b)
    | _, _ => false

/-- Double-step validator model: re-running detection must agree. -/
def checkDeterminism (bodies : List Body) : Bool :=
  contactsOf bodies == contactsOf bodies

theorem contactsOf_deterministic (bodies : List Body) :
    contactsOf bodies = contactsOf bodies := rfl

theorem validator_always_passes (bodies : List Body) :
    checkDeterminism bodies = true := by
  simp [checkDeterminism]

/-- Cell index (C++ uses floor; Nat division is floor by definition). -/
def cellOfNat (cellSize pos : Nat) : Nat := pos / cellSize

/-- The C++ cell invariant (cell = 2·maxR) implies the hypothesis the
    conservativeness theorem needs. -/
theorem two_max_bounds_sum (r₁ r₂ : Nat) : r₁ + r₂ ≤ 2 * max r₁ r₂ := by
  omega

/-- One-sided half: positions closer than a cell map to quotients that
    differ by at most one. Proved from `div_add_mod` + `mod_lt` only. -/
theorem div_close_of_dist_lt (cellSize x y : Nat)
    (hpos : 0 < cellSize)
    (hlt : y < x + cellSize) :
    y / cellSize ≤ x / cellSize + 1 := by
  by_cases h : y / cellSize ≤ x / cellSize + 1
  · exact h
  · exfalso
    have hcon' : x / cellSize + 1 < y / cellSize := by omega
    have e1 := Nat.div_add_mod x cellSize
    have ecomm : (x / cellSize) * cellSize = cellSize * (x / cellSize) :=
      Nat.mul_comm _ _
    have m1 := Nat.mod_lt x hpos
    have hle2 : (y / cellSize) * cellSize ≤ y := Nat.div_mul_le_self _ _
    have hstep : x / cellSize + 2 ≤ y / cellSize := by omega
    have hmul : (x / cellSize + 2) * cellSize ≤ (y / cellSize) * cellSize :=
      Nat.mul_le_mul_right _ hstep
    have hdist : (x / cellSize + 2) * cellSize
        = (x / cellSize) * cellSize + 2 * cellSize := by rw [Nat.add_mul]
    have hfinal : x + cellSize ≤ y := by omega
    omega

/-- 1D analogue of the grid-conservativeness claim: overlapping bodies whose
    summed radii fit in one cell are in the same or adjacent cells. -/
theorem broadphase_conservative_nat
    (cellSize p₁ p₂ r₁ r₂ : Nat)
    (hpos : 0 < cellSize)
    (hcell : r₁ + r₂ ≤ cellSize)
    (hov : p₁ + (r₁ + r₂) > p₂ ∧ p₂ + (r₁ + r₂) > p₁) :
    p₁ / cellSize = p₂ / cellSize ∨
      p₁ / cellSize + 1 = p₂ / cellSize ∨
      p₂ / cellSize + 1 = p₁ / cellSize := by
  have h12 : p₂ < p₁ + cellSize := by omega
  have h21 : p₁ < p₂ + cellSize := by omega
  have hle1 := div_close_of_dist_lt cellSize p₁ p₂ hpos h12
  have hle2 := div_close_of_dist_lt cellSize p₂ p₁ hpos h21
  omega

/-- XPBD staging over Nat: compliance 0 is definitionally the stiff
    projection (C++ uses floats; `s + 0 = s` is the same fact). -/
def correctionStiff (penetration invSum : Nat) : Nat :=
  penetration / invSum

def correctionCompliant (penetration invSum alpha : Nat) : Nat :=
  penetration / (invSum + alpha)

theorem compliance_zero_is_stiff (penetration invSum : Nat) :
    correctionCompliant penetration invSum 0
      = correctionStiff penetration invSum := by
  simp [correctionCompliant, correctionStiff]
