//! @file test_pluecker_intersector.cpp
//! @brief Trial #554: Pluecker line-geometry ray-triangle test vs the
//! incumbent Moeller-Trumbore.
//!
//! The claim under test: evaluating ray/edge incidence as the permuted
//! inner product Lr (+) Le = U.Ve + V.Ue in P^5 replaces MT's
//! determinant + barycentric solve with something faster or more robust.
//!
//! Verdict rule (ADR-002): adopt (land `render/pluecker.hpp` + keep this
//! test green) only on 100% hit/miss agreement with MT, t/bary parity,
//! and a measured win or demonstrated robustness gap. Otherwise reject
//! with the numbers.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

// Deterministic RNG (splitmix64): the trial is reproducible by construction.
std::uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
std::uint64_t rng_next() {
  std::uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
double rng_unit() {  // [0,1)
  return static_cast<double>(rng_next() >> 11) * (1.0 / 9007199254740992.0);
}

struct Vec3 {
  double x, y, z;
};
Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double len(const Vec3& a) { return std::sqrt(dot(a, a)); }

// Incumbent: non-culling Moeller-Trumbore. Returns t or -1 on miss.
double intersect_mt(const Vec3& o, const Vec3& d, const Vec3 tri[3],
                    double* bu, double* bv) {
  const Vec3 e1 = tri[1] - tri[0];
  const Vec3 e2 = tri[2] - tri[0];
  const Vec3 p = cross(d, e2);
  const double det = dot(e1, p);
  if (det > -1e-12 && det < 1e-12) return -1.0;
  const double inv = 1.0 / det;
  const Vec3 tvec = o - tri[0];
  const double u = dot(tvec, p) * inv;
  if (u < 0.0 || u > 1.0) return -1.0;
  const Vec3 q = cross(tvec, e1);
  const double v = dot(d, q) * inv;
  if (v < 0.0 || u + v > 1.0) return -1.0;
  const double t = dot(e2, q) * inv;
  if (t < 0.0) return -1.0;
  if (bu != nullptr) *bu = u;
  if (bv != nullptr) *bv = v;
  return t;
}

// Challenger: Pluecker permuted-inner-product test. Edge j runs
// tri[j] -> tri[(j+1)%3]; w_j feeds barycentric vertex (j+2)%3.
double intersect_pluecker(const Vec3& o, const Vec3& d, const Vec3 tri[3],
                          double* bu, double* bv) {
  const Vec3 U = d;
  const Vec3 V = cross(o, d);
  double w[3];
  for (int j = 0; j < 3; ++j) {
    const Vec3 a = tri[j];
    const Vec3 b = tri[(j + 1) % 3];
    const Vec3 E = b - a;
    const Vec3 M = cross(a, b);
    w[j] = dot(U, M) + dot(V, E);
  }
  const bool pos = w[0] > 0.0 && w[1] > 0.0 && w[2] > 0.0;
  const bool neg = w[0] < 0.0 && w[1] < 0.0 && w[2] < 0.0;
  if (!pos && !neg) return -1.0;
  const double s = w[0] + w[1] + w[2];
  if (s > -1e-12 && s < 1e-12) return -1.0;
  // Plane solve for t (same division count as MT: one).
  const Vec3 n = cross(tri[1] - tri[0], tri[2] - tri[0]);
  const double denom = dot(n, d);
  if (denom > -1e-12 && denom < 1e-12) return -1.0;
  const double t = (dot(n, tri[0]) - dot(n, o)) / denom;
  if (t < 0.0) return -1.0;
  // Barycentric vertex (j+2)%3 takes w_j / s.
  const double b2 = w[0] / s;  // vertex 2
  const double b0 = w[1] / s;  // vertex 0
  const double b1 = w[2] / s;  // vertex 1
  if (bu != nullptr) *bu = b1;  // MT u weights vertex 1
  if (bv != nullptr) *bv = b2;  // MT v weights vertex 2
  return t;
}

}  // namespace

TEST(PlueckerTrial, ParityWithMoellerTrumbore) {
  constexpr int kCases = 20000;
  int hits = 0;
  int disagreements = 0;
  double max_t_diff = 0.0;
  double max_bary_err = 0.0;
  for (int i = 0; i < kCases; ++i) {
    Vec3 tri[3];
    for (int k = 0; k < 3; ++k) {
      tri[k] = {rng_unit() * 20.0 - 10.0, rng_unit() * 20.0 - 10.0,
                rng_unit() * 20.0 - 10.0};
    }
    Vec3 n = cross(tri[1] - tri[0], tri[2] - tri[0]);
    const double area2 = len(n);
    if (area2 < 1e-6) {
      --i;
      continue;
    }
    n = n * (1.0 / area2);
    // Random interior point (sqrt-uniform barycentric).
    const double r1 = rng_unit(), r2 = rng_unit();
    const double a = 1.0 - std::sqrt(r1);
    const double b = std::sqrt(r1) * (1.0 - r2);
    const double c = std::sqrt(r1) * r2;
    const Vec3 P = tri[0] * a + tri[1] * b + tri[2] * c;
    // Tangent jitter keeps the ray off measure-zero alignments.
    Vec3 tangent = cross(n, Vec3{0.0, 1.0, 0.0});
    if (len(tangent) < 0.1) tangent = cross(n, Vec3{1.0, 0.0, 0.0});
    tangent = tangent * (1.0 / len(tangent));
    Vec3 dir = n * -1.0 + tangent * (rng_unit() * 0.6 - 0.3);
    dir = dir * (1.0 / len(dir));
    const double dist = 1.0 + rng_unit() * 49.0;
    const Vec3 org = P - dir * dist;

    double mtu = 0.0, mtv = 0.0, plu = 0.0, plv = 0.0;
    const double t_mt = intersect_mt(org, dir, tri, &mtu, &mtv);
    const double t_pl = intersect_pluecker(org, dir, tri, &plu, &plv);
    const bool hit_mt = t_mt >= 0.0;
    const bool hit_pl = t_pl >= 0.0;
    if (hit_mt != hit_pl) {
      ++disagreements;
      continue;
    }
    if (!hit_mt) continue;
    ++hits;
    max_t_diff = std::max(max_t_diff, std::fabs(t_mt - t_pl));
    max_t_diff = std::max(max_t_diff, std::fabs(t_mt - dist));
    // Barycentric reconstruction: hit point from each solver's (u,v).
    const Vec3 pm = tri[0] + (tri[1] - tri[0]) * mtu + (tri[2] - tri[0]) * mtv;
    const Vec3 pp = tri[0] + (tri[1] - tri[0]) * plu + (tri[2] - tri[0]) * plv;
    max_bary_err = std::max(max_bary_err, len(pp - P));
    max_bary_err = std::max(max_bary_err, len(pp - pm));
  }
  std::printf("[pluecker] cases=%d hits=%d disagreements=%d max_t_diff=%.3e "
              "max_bary_err=%.3e\n",
              kCases, hits, disagreements, max_t_diff, max_bary_err);
  EXPECT_EQ(disagreements, 0);
  EXPECT_GT(hits, kCases / 2);  // construction aims every ray at the triangle
  EXPECT_LT(max_t_diff, 1e-9);
  EXPECT_LT(max_bary_err, 1e-9);
}

TEST(PlueckerTrial, MissAgreement) {
  constexpr int kCases = 20000;
  int disagreements = 0;
  int mt_hits = 0, pl_hits = 0;
  for (int i = 0; i < kCases; ++i) {
    Vec3 tri[3];
    for (int k = 0; k < 3; ++k) {
      tri[k] = {rng_unit() * 20.0 - 10.0, rng_unit() * 20.0 - 10.0,
                rng_unit() * 20.0 - 10.0};
    }
    // Fully random ray: almost surely a miss for both.
    Vec3 org = {rng_unit() * 20.0 - 10.0, rng_unit() * 20.0 - 10.0,
                rng_unit() * 20.0 - 10.0};
    Vec3 dir = {rng_unit() * 2.0 - 1.0, rng_unit() * 2.0 - 1.0,
                rng_unit() * 2.0 - 1.0};
    dir = dir * (1.0 / len(dir));
    const double t_mt = intersect_mt(org, dir, tri, nullptr, nullptr);
    const double t_pl = intersect_pluecker(org, dir, tri, nullptr, nullptr);
    if ((t_mt >= 0.0) != (t_pl >= 0.0)) ++disagreements;
    if (t_mt >= 0.0) ++mt_hits;
    if (t_pl >= 0.0) ++pl_hits;
  }
  std::printf("[pluecker] random-ray hits mt=%d pl=%d disagreements=%d\n",
              mt_hits, pl_hits, disagreements);
  EXPECT_EQ(disagreements, 0);
}

TEST(PlueckerTrial, Timing) {
  // Fixed triangle + aimed ray, timed hot. Sinks accumulate into a printed
  // checksum so the optimizer cannot drop the work.
  const Vec3 tri[3] = {{-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0}, {0.0, 1.0, 0.0}};
  const Vec3 org = {0.1, 0.1, 5.0};
  const Vec3 dir = {0.0, 0.0, -1.0};
  constexpr int kIters = 2000000;
  double sink_mt = 0.0, sink_pl = 0.0;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; ++i) sink_mt += intersect_mt(org, dir, tri, nullptr, nullptr);
  auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; ++i) sink_pl += intersect_pluecker(org, dir, tri, nullptr, nullptr);
  auto t2 = std::chrono::steady_clock::now();
  const double mt_ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
  const double pl_ns =
      std::chrono::duration<double, std::nano>(t2 - t1).count() / kIters;
  std::printf("[pluecker] mt=%.2f ns/ray (sink %.6f) pluecker=%.2f ns/ray "
              "(sink %.6f) ratio=%.2fx\n",
              mt_ns, sink_mt, pl_ns, sink_pl, pl_ns / mt_ns);
  EXPECT_GT(sink_mt, 0.0);
  EXPECT_GT(sink_pl, 0.0);
}
