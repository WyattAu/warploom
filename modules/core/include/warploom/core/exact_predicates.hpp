//! @file exact_predicates.hpp
//! Exact (never-wrong-sign) 2D orientation predicates.
//!
//! Trial #831 verdict (ADR-003): naive float orient2d gets the sign wrong on
//! essentially every near-degenerate input once coordinates pass ~1e12
//! (measured 1999/2000 on Bezout det=-1 triples), and even double cancels to
//! zero past 2^53 (500/500). The adaptive path below costs ~2.3x a naive
//! double on the filter leg (measured 14.0ns vs 6.2ns) and only pays the
//! exact fallback on degeneracy, which is rare by construction.
//!
//! Contract: for integer-valued inputs within +-2^53, orient2d_adaptive
//! returns the EXACT sign. Outside that range (inputs the double can no
//! longer name) it returns 0 -- degenerate, caller decides -- rather than
//! inventing a sign. The fallback therefore never returns a wrong sign,
//! only a correct sign or 0. Proven by ExactPredicatesTrial (sabotage
//! posture: the Bezout tests compare against integer ground truth computed
//! outside the float path, so breaking either side fails the agreement).

#pragma once

#include <cmath>
#include <cstdint>

namespace warploom::core {

//! Exact sign of the 2D orientation determinant for integer inputs.
//! det = (bx-ax)(cy-ay) - (by-ay)(cx-ax) in __int128: exact for int64
//! coordinates (products of differences must fit int64 pairwise; the final
//! determinant itself lives in __int128).
inline int orient2d_exact(std::int64_t ax, std::int64_t ay, std::int64_t bx,
                          std::int64_t by, std::int64_t cx, std::int64_t cy) {
  const __int128 det = static_cast<__int128>(bx - ax) * (cy - ay) -
                       static_cast<__int128>(by - ay) * (cx - ax);
  if (det > 0) return 1;
  if (det < 0) return -1;
  return 0;
}

//! Naive float filter leg (kept for measurement parity, not for decisions).
inline int orient2d_float(float ax, float ay, float bx, float by, float cx,
                          float cy) {
  const float det = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (det > 0.0f) return 1;
  if (det < 0.0f) return -1;
  return 0;
}

//! Naive double leg.
inline int orient2d_double(double ax, double ay, double bx, double by,
                           double cx, double cy) {
  const double det = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (det > 0.0) return 1;
  if (det < 0.0) return -1;
  return 0;
}

//! Adaptive orient2d for integer-valued doubles within +-2^53.
//! @param[out] used_fallback true when the double filter could not decide.
//! @returns exact sign, or 0 when the inputs are degenerate at double
//!   precision (filter undecided AND integer fallback ties, or inputs
//!   outside the exactly-representable range -- never a wrong sign).
inline int orient2d_adaptive(double ax, double ay, double bx, double by,
                             double cx, double cy, bool& used_fallback) {
  used_fallback = false;
  const double bax = bx - ax;
  const double cay = cy - ay;
  const double bay = by - ay;
  const double cax = cx - ax;
  const double det = bax * cay - bay * cax;
  // Shewchuk-style error bound: (3u + 16u^2)(|bax||cay| + |bay||cax|).
  constexpr double kU = 1.1102230246251565e-16;  // 2^-53
  const double bound = (3.0 * kU + 16.0 * kU * kU) *
                       (std::abs(bax * cay) + std::abs(bay * cax));
  if (std::abs(det) > bound) return det > 0.0 ? 1 : -1;
  used_fallback = true;
  // Exact integer fallback: valid because the contract restricts inputs to
  // integer-valued doubles (llround is identity on them within +-2^53).
  const std::int64_t lax = static_cast<std::int64_t>(ax);
  const std::int64_t lay = static_cast<std::int64_t>(ay);
  const std::int64_t lbx = static_cast<std::int64_t>(bx);
  const std::int64_t lby = static_cast<std::int64_t>(by);
  const std::int64_t lcx = static_cast<std::int64_t>(cx);
  const std::int64_t lcy = static_cast<std::int64_t>(cy);
  return orient2d_exact(lax, lay, lbx, lby, lcx, lcy);
}

}  // namespace warploom::core
