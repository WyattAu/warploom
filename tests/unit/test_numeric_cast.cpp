//! @file test_numeric_cast.cpp
//! @brief Checked float->integer conversion used by the protocol parsers.
//!
//! These exist because the predicate replaced three hand-rolled copies that
//! cast a `double` to an integer type before checking the range, which is
//! undefined for out-of-range values. The `1e300` cases are the point: under
//! UBSan a float-cast-overflow is a hard diagnostic, so a regression here is
//! caught by the sanitizer presets rather than by inspection.

#include <warploom/core/numeric_cast.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace warploom::core {
namespace {

TEST(NumericCast, AcceptsExactNonNegativeIntegers) {
  std::uint64_t out = 999;
  EXPECT_TRUE(checked_double_to_uint64(0.0, out));
  EXPECT_EQ(out, 0U);
  EXPECT_TRUE(checked_double_to_uint64(1.0, out));
  EXPECT_EQ(out, 1U);
  EXPECT_TRUE(checked_double_to_uint64(1234567.0, out));
  EXPECT_EQ(out, 1234567U);
}

TEST(NumericCast, AcceptsLargestExactlyRepresentableInteger) {
  std::uint64_t out = 0;
  // 2^53 is exactly representable, so it is in range and exact.
  EXPECT_TRUE(checked_double_to_uint64(9007199254740992.0, out));
  EXPECT_EQ(out, std::uint64_t{1} << 53U);
}

TEST(NumericCast, RejectsFractions) {
  std::uint64_t out = 42;
  EXPECT_FALSE(checked_double_to_uint64(0.5, out));
  EXPECT_FALSE(checked_double_to_uint64(1.0000000001, out));
  EXPECT_FALSE(checked_double_to_uint64(-0.5, out));
  // A tiny value that rounds to zero in the float domain must still be
  // rejected: it is not an integer the caller could have meant.
  EXPECT_FALSE(checked_double_to_uint64(1e-12, out));
  EXPECT_EQ(out, 42U) << "out must be untouched on failure";
}

TEST(NumericCast, RejectsNegativesAndZeroForNonNegativeOnly) {
  std::uint64_t out = 7;
  EXPECT_FALSE(checked_double_to_uint64(-1.0, out));
  EXPECT_FALSE(checked_double_to_uint64(-1e300, out));
  EXPECT_EQ(out, 7U);
}

TEST(NumericCast, RejectsOutOfRangeBeforeCasting) {
  // This is the undefined behaviour the old code had: 1e300 and UINT64_MAX+1
  // are both outside uint64's range, so the cast was unspecified.
  std::uint64_t out = 5;
  EXPECT_FALSE(checked_double_to_uint64(1e300, out));
  EXPECT_FALSE(checked_double_to_uint64(
      static_cast<double>(std::numeric_limits<std::uint64_t>::max()) * 2.0, out));
  EXPECT_FALSE(checked_double_to_uint64(std::numeric_limits<double>::infinity(),
                                        out));
  EXPECT_FALSE(
      checked_double_to_uint64(-std::numeric_limits<double>::infinity(), out));
  EXPECT_EQ(out, 5U);
}

TEST(NumericCast, RejectsNan) {
  // Every NaN comparison is false, so the negated form is what rejects it.
  std::uint64_t out = 11;
  EXPECT_FALSE(checked_double_to_uint64(std::numeric_limits<double>::quiet_NaN(),
                                        out));
  EXPECT_EQ(out, 11U);
}

TEST(NumericCast, RejectsIntegersPastTwoPow53) {
  // 2^53 + 1 is not representable, so this double is the even number 2^53.
  // Accepting it would decode a tick the writer never wrote.
  std::uint64_t out = 3;
  const double just_past = 9007199254740992.0 + 2.0;
  // Exact: establishing that the double is integral is the premise of the
  // test, so an approximate comparison would make the assertion meaningless.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
  EXPECT_TRUE(std::floor(just_past) == just_past) << "double really is integral";
#pragma GCC diagnostic pop
  EXPECT_FALSE(checked_double_to_uint64(just_past, out));
  EXPECT_EQ(out, 3U);
}

TEST(NumericCast, Uint32VariantRejectsAboveItsOwnRange) {
  std::uint32_t out = 1;
  EXPECT_TRUE(checked_double_to_uint32(4294967295.0, out));
  EXPECT_EQ(out, 4294967295U);
  // The next integer is out of range for uint32 and must not wrap.
  EXPECT_FALSE(checked_double_to_uint32(4294967296.0, out));
  EXPECT_EQ(out, 4294967295U) << "out must be untouched on failure";
}

TEST(NumericCast, Uint32VariantRejectsLargeFractions) {
  std::uint32_t out = 2;
  EXPECT_FALSE(checked_double_to_uint32(4294967295.5, out));
  EXPECT_EQ(out, 2U);
}

TEST(NumericCast, NegativeZeroIsAcceptedAsZero) {
  // -0.0 is not a rejection case: it is numerically zero, and once std::stod
  // has parsed the text there is no way left to tell "-0" from "0". Refusing
  // it would mean rejecting a value that equals a valid one.
  std::uint64_t out = 99;
  EXPECT_TRUE(checked_double_to_uint64(-0.0, out));
  EXPECT_EQ(out, 0U);
}

TEST(NumericCast, IntToDoubleRoundTripIsExactWithinRange) {
  double out = 0.0;
  EXPECT_TRUE(checked_uint64_to_double(0U, out));
  EXPECT_EQ(out, 0.0);
  EXPECT_TRUE(checked_uint64_to_double(123456789U, out));
  EXPECT_EQ(out, 123456789.0);
  EXPECT_TRUE(checked_uint64_to_double(std::uint64_t{1} << 53U, out));
  EXPECT_EQ(out, 9007199254740992.0);
}

TEST(NumericCast, IntToDoubleRejectsLossyWidening) {
  double out = 99.0;
  EXPECT_FALSE(checked_uint64_to_double((std::uint64_t{1} << 53U) + 2U, out));
  EXPECT_EQ(out, 99.0);
  EXPECT_FALSE(checked_uint64_to_double(std::numeric_limits<std::uint64_t>::max(),
                                        out));
}

TEST(NumericCast, EveryRejectedValueIsAlsoRejectedByUint64) {
  // The two variants must agree, or the three call sites disagree about what a
  // valid payload looks like.
  const double kBad[] = {
      -1.0,                         0.5,          1e-12,
      1e300,       1e308,          9007199254740994.0,
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
  };
  for (const double bad : kBad) {
    std::uint64_t wide = 1;
    std::uint32_t narrow = 1;
    EXPECT_FALSE(checked_double_to_uint64(bad, wide)) << "wide " << bad;
    EXPECT_FALSE(checked_double_to_uint32(bad, narrow)) << "narrow " << bad;
  }
}

}  // namespace
}  // namespace warploom::core