#pragma once

//! @file numeric_cast.hpp
//! @brief Checked float-to-integer conversion for protocol payloads.
//!
//! Three parsers -- the control protocol, the document schema reader and the
//! input-script reader -- all receive their numbers as JSON `double`s and all
//! need the same predicate: "this number is an exact non-negative integer that
//! fits in an unsigned type". Each wrote it by hand as
//!
//!     value < 0.0 || value != static_cast<double>(static_cast<std::uint64_t>(value))
//!
//! which is wrong in two ways:
//!
//!  1. The cast happens BEFORE the range is known to be valid.
//!     `static_cast<std::uint64_t>(1e300)` is undefined: the value is outside
//!     the destination type's range, so the result is unspecified (x86-64
//!     yields 0x8000000000000000 via cvttsd2si's integer-indefinite). A
//!     hostile or merely malformed protocol line carrying `"id": 1e300`
//!     reaches that cast.
//!  2. For any `value >= 2^53` the double has no fractional part to begin
//!     with, so "round-trips exactly" stops meaning "is an integer" -- it
//!     silently accepts 2^53 + 1 as an integer, because that double *is* an
//!     integer, just not the one the writer typed.
//!
//! Both failures are in the range check, not the comparison, so this lives in
//! one place and gets one set of tests.
//!
//! The exactness test itself is a float equality, which -Wfloat-equal flags
//! and correctly so in general. Here it is the only sound formulation: the
//! alternative (a tolerance) would accept values that are not integers, which
//! is a correctness bug rather than a style question. The suppression is
//! local to this one function and the reasoning is here.

#include <cmath>
#include <cstdint>
#include <limits>

namespace warploom::core {

//! Largest integer a `double` represents exactly. Beyond 2^53, consecutive
//! integers are not distinguishable in the floating-point domain, so
//! "the double is an integer" and "the double is the integer the caller meant"
//! stop being the same question.
inline constexpr double kMaxExactInteger = 9007199254740992.0;  // 2^53

//! Converts `value` to a `std::uint32_t` when it is an exact non-negative
//! integer in [0, 2^53). Returns false and leaves `out` untouched otherwise --
//! including for NaN and +-infinity, which the comparisons below reject
//! because every NaN comparison is false and every infinity exceeds the bound.
[[nodiscard]] inline bool checked_double_to_uint32(double value,
                                                  std::uint32_t& out) noexcept {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
  // Range first: this is what makes the casts below well-defined. The NaN case
  // falls out of `!(value >= 0.0)`, and +-inf out of `!(value < 2^53)`.
  // Bounded by the DESTINATION type, not just by what a double represents:
  // 2^32..2^53 are exact doubles but wrap when narrowed to uint32.
  constexpr double kMaxUint32 = 4294967295.0;  // UINT32_MAX, exactly representable
  if (!(value >= 0.0) || !(value <= kMaxUint32)) {
    return false;
  }
  // Now in [0, 2^32]: the cast cannot overflow, and it truncates toward zero.
  const auto narrowed = static_cast<std::uint64_t>(value);
  // Exact, not approximate: narrowing back to double is lossless across the
  // whole range above, so this is true iff `value` had no fractional part.
  if (static_cast<double>(narrowed) != value) {
    return false;
  }
#pragma GCC diagnostic pop
  out = static_cast<std::uint32_t>(narrowed);
  return true;
}

//! Converts `value` to a `std::uint64_t` under the same rule. Values in
//! [2^53, 2^64) are integral doubles but are NOT representable exactly, so
//! they are rejected rather than silently rounded -- a replay tick of
//! 2^53 + 1 must not decode as 2^53.
[[nodiscard]] inline bool checked_double_to_uint64(double value,
                                                  std::uint64_t& out) noexcept {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
  if (!(value >= 0.0) || !(value <= kMaxExactInteger)) {
    return false;
  }
  const auto narrowed = static_cast<std::uint64_t>(value);
  if (static_cast<double>(narrowed) != value) {
    return false;
  }
#pragma GCC diagnostic pop
  out = narrowed;
  return true;
}

//! Converts `value` to a `double` from an integer payload field, for the
//! converse direction in the two writers. Widening int -> double is exact up
//! to 2^53; past that it is not, so the caller is told rather than left with a
//! silently rounded number. Named explicitly because it is not implicit.
[[nodiscard]] inline bool checked_uint64_to_double(std::uint64_t value,
                                                   double& out) noexcept {
  if (value > static_cast<std::uint64_t>(kMaxExactInteger)) {
    return false;
  }
  out = static_cast<double>(value);
  return true;
}

}  // namespace warploom::core