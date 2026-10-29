/-
  LOD threshold-count clamp, formalized (Loop 9 of 10).

  Models the selection loop in `assets/shaders/lod_select.comp`
  (`thresholds[8]`, host-provided `threshold_count`) and the identical
  shape in `cull_and_draw_lod.comp` / `cull_and_draw_lod_occlude.comp`
  (capacity 4). Pixel sizes are mapped to `Nat` (floor); the claim is
  about indices, where the float/Nat distinction is immaterial.

  Proved:
  1. `select_le` — a count clamped to the array bound can only return an
     index within `0 .. bound` (sentinel included), so every array read
     the shader performs is in-bounds. This is the argument for placing
     the `min()` before the loop rather than trusting the host count.
  2. `unclamped_selects_bogus_lod` — the live-fire counter-model: with
     the real thresholds, a 6 px sphere and the camera words aliased past
     the array, the unclamped count 10 selects index 8 (a visible LOD 8
     that names no variant), while the clamped count selects the sentinel
     8 outside `< 8`, i.e. culled. Mirrors the GPU test
     `LodSelectClampsThresholdCountToArrayBound` exactly.
-/

/-- Selection over a threshold list: first satisfactory index from `i`,
    else the end index (the shader's sentinel). -/
def selectGo : List Nat → Nat → Nat → Nat
  | [], _, i => i
  | t :: ts, s, i => if s >= t then i else selectGo ts s (i + 1)

/-- The shader's selection: first index below `cnt` meeting the size. -/
def select (th : List Nat) (screen cnt : Nat) : Nat :=
  selectGo (th.take cnt) screen 0

/-- The shader's visibility decision: selected index below the count. -/
def visible (sel cnt : Nat) : Bool := sel < cnt

/-- `selectGo` never returns past the end of the list it scans. -/
theorem selectGo_bound (l : List Nat) (s i : Nat) :
    selectGo l s i ≤ i + l.length := by
  induction l generalizing i with
  | nil => simp [selectGo]
  | cons t ts ih =>
    simp only [selectGo]
    split
    · exact Nat.le_add_right _ _
    · have h := ih (i + 1)
      simp only [List.length_cons] at h ⊢
      omega

/-- A clamped count only yields indices within `0 .. bound` (sentinel
    included), so every `thresholds[i]` read is in-bounds. -/
theorem select_le (th : List Nat) (screen cnt bound : Nat) :
    select th screen (min cnt bound) ≤ bound := by
  unfold select
  have h := selectGo_bound (th.take (min cnt bound)) screen 0
  simp only [Nat.zero_add] at h
  have hlen : (th.take (min cnt bound)).length ≤ bound := by
    rw [List.length_take]
    exact Nat.le_trans (Nat.min_le_left _ _) (Nat.min_le_right _ _)
  omega

/-- Counter-model, mirroring the GPU test: 8 descending thresholds, a
    6 px sphere, camera words `[0, 0]` aliased at indices 8-9. Unclamped
    count 10 selects index 8 — and `8 < 10` reports it VISIBLE with a
    LOD 8 that names no variant. -/
theorem unclamped_selects_bogus_lod :
    select [200, 150, 100, 80, 60, 40, 20, 8] 6 10 = 8 ∧
    visible (select [200, 150, 100, 80, 60, 40, 20, 8] 6 10) 10 = true := by
  native_decide

/-- The same inputs under the clamp select the sentinel 8, and `8 < 8`
    is false: culled, exactly what the fixed shader writes. -/
theorem clamped_select_culls :
    select [200, 150, 100, 80, 60, 40, 20, 8] 6 (min 10 8) = 8 ∧
    visible (select [200, 150, 100, 80, 60, 40, 20, 8] 6 (min 10 8)) 8 = false := by
  native_decide
