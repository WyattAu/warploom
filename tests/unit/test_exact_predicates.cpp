//! Trial #831: exact multi-precision geometric predicates vs naive float.
//! Verifies warploom/core/exact_predicates.hpp against EXACT ground truth
//! from integer arithmetic on intended coordinates (no float in the
//! reference path). See ADR-003 for the verdict numbers.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "warploom/core/exact_predicates.hpp"

namespace {

namespace core = ::warploom::core;

struct Pt {
  double x, y;
};

struct IntPt {
  long long x, y;
};

// Exact sign from INTENDED integer coordinates (not the rounded doubles).
// Required past 2^53, where the double itself can no longer name the input.
int orient_truth(const IntPt& a, const IntPt& b, const IntPt& c) {
  return core::orient2d_exact(a.x, a.y, b.x, b.y, c.x, c.y);
}

Pt to_pt(const IntPt& p) {
  return Pt{static_cast<double>(p.x), static_cast<double>(p.y)};
}

int use_float(const Pt& a, const Pt& b, const Pt& c) {
  return core::orient2d_float(static_cast<float>(a.x), static_cast<float>(a.y),
                             static_cast<float>(b.x), static_cast<float>(b.y),
                             static_cast<float>(c.x), static_cast<float>(c.y));
}

int use_double(const Pt& a, const Pt& b, const Pt& c) {
  return core::orient2d_double(a.x, a.y, b.x, b.y, c.x, c.y);
}

int use_adaptive(const Pt& a, const Pt& b, const Pt& c, bool& fb) {
  return core::orient2d_adaptive(a.x, a.y, b.x, b.y, c.x, c.y, fb);
}

// Deterministic PRNG (xorshift64*, fixed seed): geometry trials must be
// reproducible run to run per the determinism contract.
std::uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
std::uint64_t next_u64() {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1DULL;
}

TEST(ExactPredicatesTrial, RandomSmallIntAllAgree) {
  // Sanity: generic (non-degenerate) data -- every implementation agrees.
  int disagreements = 0;
  for (int i = 0; i < 20000; ++i) {
    IntPt ip[3];
    for (auto& q : ip) {
      q.x = static_cast<long long>(next_u64() % 2000001ULL) - 1000000LL;
      q.y = static_cast<long long>(next_u64() % 2000001ULL) - 1000000LL;
    }
    const int truth =
        core::orient2d_exact(ip[0].x, ip[0].y, ip[1].x, ip[1].y, ip[2].x,
                             ip[2].y);
    Pt p[3] = {to_pt(ip[0]), to_pt(ip[1]), to_pt(ip[2])};
    if (use_float(p[0], p[1], p[2]) != truth) ++disagreements;
    if (use_double(p[0], p[1], p[2]) != truth) ++disagreements;
  }
  std::printf("[exact] random small-int: float+double disagreements=%d / 40000\n",
              disagreements);
  // Random data is almost never near-degenerate; agreement is expected here.
  EXPECT_LT(disagreements, 40);  // < 0.1%: documents the baseline, not a bar
}

TEST(ExactPredicatesTrial, AdversarialBezoutFloatFails) {
  // Bezout triples: det = -1 exactly with coordinates ~1e12. float32 has ~7
  // digits, so 1e12+1 rounds and the naive float determinant cancels to 0.
  const long long P = 1000000LL;
  const long long Q = 1000000LL;
  int float_wrong = 0, double_wrong = 0, exact_wrong = 0;
  for (int i = 0; i < 2000; ++i) {
    const long long pi = P + static_cast<long long>(i);
    const IntPt ia{0, 0};
    const IntPt ib{pi, 1};
    const IntPt ic{pi * Q + 1, Q};
    const int truth = orient_truth(ia, ib, ic);
    EXPECT_EQ(truth, -1) << "Bezout construction broken at i=" << i;
    if (truth != -1) continue;
    const Pt a = to_pt(ia);
    const Pt b = to_pt(ib);
    const Pt c = to_pt(ic);
    if (use_float(a, b, c) != truth) ++float_wrong;
    if (use_double(a, b, c) != truth) ++double_wrong;
    bool fb = false;
    if (use_adaptive(a, b, c, fb) != truth) ++exact_wrong;
  }
  std::printf("[exact] bezout ~1e12: float wrong=%d/2000 double wrong=%d/2000 adaptive wrong=%d/2000\n",
              float_wrong, double_wrong, exact_wrong);
  EXPECT_GT(float_wrong, 1900);  // float must demonstrably fail here
  EXPECT_EQ(double_wrong, 0);    // 1e12 < 2^53: double still exact
  EXPECT_EQ(exact_wrong, 0);
}

TEST(ExactPredicatesTrial, AdversarialBeyondDoubleFails) {
  // Same construction at ~2^54: pi*Q is a multiple of 4 (exactly
  // representable), so intended cx = pi*Q+1 rounds DOWN to pi*Q in double
  // and naive double cancels to 0. Ground truth comes from the intended
  // integers, which the double can no longer name.
  const long long P = 134217728LL;  // 2^27
  const long long Q = 134217728LL;
  int double_wrong = 0, adaptive_wrong_sign = 0, fallbacks = 0, tested = 0;
  for (int i = 0; i < 500; ++i) {
    const long long pi = P + static_cast<long long>(i * 2);
    const IntPt ia{0, 0};
    const IntPt ib{pi, 1};
    const IntPt ic{pi * Q + 1, Q};
    const int truth = orient_truth(ia, ib, ic);
    EXPECT_EQ(truth, -1) << "Bezout construction broken at i=" << i;
    if (truth != -1) continue;
    ++tested;
    const Pt a = to_pt(ia);
    const Pt b = to_pt(ib);
    const Pt c = to_pt(ic);
    if (use_double(a, b, c) != truth) ++double_wrong;
    bool fb = false;
    const int ad = use_adaptive(a, b, c, fb);
    // Wrong-SIGN check (not equality): 0 means "degenerate, caller decides".
    if ((truth == 1 && ad == -1) || (truth == -1 && ad == 1)) {
      ++adaptive_wrong_sign;
    }
    if (fb) ++fallbacks;
  }
  std::printf("[exact] bezout ~2^54: tested=%d double wrong=%d adaptive wrong-sign=%d fallbacks=%d\n",
              tested, double_wrong, adaptive_wrong_sign, fallbacks);
  // Past 2^53 the +1 is unrepresentable: NO algorithm on the rounded doubles
  // can recover -1 (the input is gone before the predicate runs). The helper
  // contract is therefore integer-valued doubles within +-2^53 -> exact sign;
  // outside it the filter must refuse a sign, never invent one: adaptive may
  // return 0 (degenerate, caller decides) but never the WRONG sign.
  EXPECT_GT(double_wrong, 400);   // even double has a cliff at 2^54
  EXPECT_EQ(adaptive_wrong_sign, 0);  // no wrong sign out, only 0s
  EXPECT_GT(fallbacks, 400);      // the filter caught every one, none slipped
}

TEST(ExactPredicatesTrial, Timing) {
  const Pt a{0.0, 0.0};
  const Pt b{1000000.0, 1.0};
  const Pt c{1000000000001.0, 1000000.0};
  volatile int sink = 0;
  constexpr int kIters = 2000000;
  auto bench = [&](auto fn) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) sink += fn(a, b, c);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count() / kIters * 1e9;
  };
  const double t_float = bench(use_float);
  const double t_double = bench(use_double);
  bool fb = false;
  const double t_adapt = bench([&](const Pt& x, const Pt& y, const Pt& z) {
    return use_adaptive(x, y, z, fb);
  });
  std::printf("[exact] ns/pred: float=%.1f double=%.1f adaptive=%.1f (sink %d)\n",
              t_float, t_double, t_adapt, sink);
  // No timing assertion: numbers are evidence for the ADR, not a gate.
  SUCCEED();
}

}  // namespace
