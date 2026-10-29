/-
  Decoded-size cap soundness, formalized (Loop 8 of 10).

  Models `decode_ktx2` in
  `modules/asset/src/warploom/asset/ktx2_decoder.cpp` (`level_bytes` plus
  the `kMaxDecodedKtx2Bytes` early reject) and `DecodedImage::valid()` in
  `modules/asset/include/warploom/asset/image_decode.hpp`.

  Proved:
  1. `level_le_succ` — each mip level is no larger than the previous
     (ceil-halving per axis), so level 0 dominates: capping level 0 caps
     the allocation, since only level 0 is returned.
  2. `cap_sound` — anything the cap check accepts allocates at most
     `cap` bytes.
  3. `valid32_wraps` — the counter-model: 32-bit-truncated comparison can
     validate a short buffer (65536x65536x4 wraps to 0 mod 2^32), which
     is why `valid()` uses the 64-bit product.
-/

/-- Halved dimension with the 1-pixel floor (`shift` in both the C++ and
    the test helper: `dim >> lvl`, floored at 1). -/
def halfDim : Nat → Nat → Nat
  | dim, 0 => dim
  | dim, n + 1 =>
    let h := halfDim dim n / 2
    if h = 0 then 1 else h

/-- Mip payload size: ceil-halved w × ceil-halved h × 4 (`level_bytes`). -/
def levelBytes (w h lvl : Nat) : Nat :=
  halfDim w lvl * halfDim h lvl * 4

/-- 256 MiB shared budget (`kMaxDecodedPngBytes` /
    `kMaxDecodedJpegBytes` / `kMaxDecodedKtx2Bytes`). -/
def cap : Nat := 256 * 1024 * 1024

/-- Halving a positive dimension stays positive (the 1-pixel floor).
    Needs `0 < dim`: `halfDim 0` climbs 0 → 1, which is why the
    monotonicity lemma below also takes positivity. The C++ side
    establishes it with the nonzero reject before any size math. -/
theorem halfDim_pos (dim lvl : Nat) (h : 0 < dim) :
    0 < halfDim dim lvl := by
  induction lvl with
  | zero => simpa [halfDim] using h
  | succ n ih =>
    simp only [halfDim]
    by_cases hc : halfDim dim n / 2 = 0
    · have e : (if halfDim dim n / 2 = 0 then (1 : Nat) else halfDim dim n / 2) = 1 := by simp [hc]
      rw [e]
      exact Nat.zero_lt_one
    · have e : (if halfDim dim n / 2 = 0 then (1 : Nat) else halfDim dim n / 2) = halfDim dim n / 2 := by simp [hc]
      rw [e]
      exact Nat.pos_of_ne_zero hc

/-- Halving never grows a positive dimension. -/
theorem halfDim_le_succ (dim lvl : Nat) (h : 0 < dim) :
    halfDim dim (lvl + 1) ≤ halfDim dim lvl := by
  have hpos := halfDim_pos dim lvl h
  simp only [halfDim]
  by_cases hc : halfDim dim lvl / 2 = 0
  · have e : (if halfDim dim lvl / 2 = 0 then (1 : Nat) else halfDim dim lvl / 2) = 1 := by simp [hc]
    rw [e]
    exact hpos
  · have e : (if halfDim dim lvl / 2 = 0 then (1 : Nat) else halfDim dim lvl / 2) = halfDim dim lvl / 2 := by simp [hc]
    rw [e]
    exact Nat.div_le_self _ _

/-- Each mip level is no larger than the previous (positive dimensions).
    Corollary: level 0 dominates every level, so capping level 0 caps the
    allocation — the argument for checking the cap before the level walk. -/
theorem level_le_succ (w h lvl : Nat) (hw : 0 < w) (hh : 0 < h) :
    levelBytes w h (lvl + 1) ≤ levelBytes w h lvl := by
  unfold levelBytes
  have hw' := halfDim_le_succ w lvl hw
  have hh' := halfDim_le_succ h lvl hh
  have h1 : halfDim w (lvl + 1) * halfDim h (lvl + 1)
      ≤ halfDim w lvl * halfDim h lvl :=
    Nat.mul_le_mul hw' hh'
  exact Nat.mul_le_mul h1 (Nat.le_refl 4)

/-- Level 0 is exactly the capped product `w * h * 4`. -/
theorem level0_eq (w h : Nat) : levelBytes w h 0 = w * h * 4 := by
  simp [levelBytes, halfDim]

/-- Cap soundness: if the level-0 product passes the early reject, the
    allocation is within budget. This is the property the C++ check
    enforces before walking the level index or resizing. -/
theorem cap_sound (w h : Nat) (ok : w * h * 4 ≤ cap) :
    levelBytes w h 0 ≤ cap := by
  rw [level0_eq]; exact ok

/-- 32-bit wrap counter-model: 65536×65536×4 ≡ 0 (mod 2^32), so a
    32-bit-truncated comparison validates an EMPTY buffer against
    gigapixel dimensions (nonzero dims, 0 = 0) — the bug the 64-bit
    `valid()` fixes. On wasm32 (`size_t` 32-bit) the old code took this
    path; on LP64 the 64-bit promotion already saved it. -/
theorem valid32_wraps :
    (65536 * 65536 * 4) % 2 ^ 32 = 0 := by
  native_decide
