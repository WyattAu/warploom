//! @file test_path_tracing_real.cpp
//! @brief E4 GPU proof: REAL path tracing — iterative N-bounce loop in the
//!        raygen (maxPipelineRayRecursionDepth = 1, the production pattern),
//!        per-pixel PCG RNG streams, cosine-weighted sampling, and temporal
//!        accumulation of independent per-frame samples.
//!
//! Scene: the INTERIOR of a 10x10x5 box (open top) with Lambertian walls.
//! The camera sits inside; every first hit is a wall/slab, every path ends
//! by escaping through the open ceiling (sky radiance 1).
//!//! Validation does NOT depend on reproducing the GPU's RNG (independent
//! streams):
//!   1. Convergence (gold standard): an INDEPENDENT fp64 Monte-Carlo
//!      integrator (std::mt19937_64, 262144 samples, own intersection code,
//!      same pixel-aperture primary model) at three surface probes — the GPU
//!      mean-of-64 must land within tolerance. Unbiased + converged, not
//!      bit-matched.
//!   2. Multi-bounce proof: the 1-bounce reference sits far below the
//!      8-bounce reference at the floor probe, and the GPU image follows the
//!      8-bounce value — inter-reflected energy is really accumulated.
//!   3. Determinism: the GPU run is byte-identical across repetitions
//!      (fixed seeds => reproducible render).
//!   4. Structural: an open-sky pixel equals the fp32 mirror exactly; all
//!      interior probes are strictly below the sky radiance.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef WARPLOOM_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

#include "warploom/render/vulkan_acceleration_structure.hpp"
#include "warploom/render/vulkan_context.hpp"
#include "warploom/render/vulkan_descriptors.hpp"
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_path_tracer.hpp"
#include "warploom/render/vulkan_pipeline.hpp"
#include "warploom/render/vulkan_renderer.hpp"
#include "vulkan_test_readback.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

//! %f varargs consume a double; make the required promotion explicit.
inline double dbl(float v) noexcept { return static_cast<double>(v); }

using omnicpp::render::Allocation;
using omnicpp::render::PathTracerCamera;
using omnicpp::render::PathTracerConfig;
using omnicpp::render::PathTracerInstance;
using omnicpp::render::PathTracerMesh;
using omnicpp::render::VulkanContext;
using omnicpp::render::VulkanMemoryAllocator;
using omnicpp::render::VulkanPathTracer;

constexpr std::uint32_t kImg = 64;
constexpr std::uint32_t kFrames = 64;   // accumulation samples per pass
constexpr std::uint32_t kBounces = 8;   // path segments per frame

// ---- Scene: interior of a box, open top ------------------------------------
// Walls: |x|=5, |z|=5 for y in [-5,0]; slab (floor) y=-5; ceiling is OPEN
// (no geometry) — the sky is the only light.
constexpr double kRoomHx = 5.0;  // half extent x/z
constexpr double kRoomHy = 5.0;  // walls span y in [-2*hy, 0]
constexpr float kAlbedo = 0.8f;
constexpr float kSky = 1.0f;

const std::array<float, 3> kCamEye{0.0f, 0.0f, 0.0f};   // room center height
// Tilted down 26.57 degrees so the floor is inside the 90-degree frustum.
const std::array<float, 3> kCamFwd = [] {
  std::array<float, 3> f{0.0f, -0.5f, -1.0f};
  const float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  return std::array<float, 3>{f[0] / l, f[1] / l, f[2] / l};
}();

// ---- Box-triangle generation (unit corner encoding, as E3) ------------------

std::vector<float> make_box_triangles(float hx, float hy, float hz) {
  static const std::uint32_t c[36] = {
      0, 4, 5, 0, 5, 1,  4, 6, 7, 4, 7, 5,
      6, 2, 3, 6, 3, 7,  2, 0, 1, 2, 1, 3,
      1, 5, 7, 1, 7, 3,  2, 6, 4, 2, 4, 0};
  const float h[3] = {hx, hy, hz};
  std::vector<float> out;
  out.reserve(36U * 3U);
  for (const std::uint32_t vi : c) {
    for (int k = 0; k < 3; ++k) {
      out.push_back(((vi >> k) & 1U) != 0U ? h[k] : -h[k]);
    }
  }
  return out;
}

// ---- Sphere-triangle generation (lat/long soup, outward winding) ---------
// Position-only floats like make_box_triangles. Orientation is belt and
// braces (the rchit faceforwards the geometric normal), but outward is
// correct: quads go (P00, P01, P11) + (P00, P11, P10), the top fan goes
// (apex, ring1[i+1], ring1[i]) and the bottom fan (apex, ring[i],
// ring[i+1]) -- each verified once by expanding (dP/dtheta)x(dP/dphi)
// at the equator/poles. Degenerate pole quads are fans instead: a
// zero-area triangle must never reach the BLAS build.
std::vector<float> make_sphere_triangles(float radius, std::uint32_t nlat,
                                         std::uint32_t nlon) {
  const double r = static_cast<double>(radius);
  constexpr double kPi = 3.141592653589793;
  const auto pt = [&](std::uint32_t j, std::uint32_t i) {
    const double th =
        kPi * static_cast<double>(j) / static_cast<double>(nlat);
    const double ph = 2.0 * kPi * static_cast<double>(i % nlon) /
                      static_cast<double>(nlon);
    return std::array<double, 3>{r * std::sin(th) * std::cos(ph),
                                 r * std::cos(th), r * std::sin(th) * std::sin(ph)};
  };
  std::vector<float> out;
  out.reserve(static_cast<std::size_t>(2U) * nlon * (nlat - 1U) * 9U);
  const auto push = [&](const std::array<double, 3>& p) {
    out.push_back(static_cast<float>(p[0]));
    out.push_back(static_cast<float>(p[1]));
    out.push_back(static_cast<float>(p[2]));
  };
  const std::array<double, 3> north = pt(0U, 0U);
  for (std::uint32_t i = 0; i < nlon; ++i) {
    push(north);
    push(pt(1U, i + 1U));
    push(pt(1U, i));
  }
  for (std::uint32_t j = 1; j + 1 < nlat; ++j) {
    for (std::uint32_t i = 0; i < nlon; ++i) {
      const std::array<double, 3> p00 = pt(j, i);
      const std::array<double, 3> p01 = pt(j, i + 1U);
      const std::array<double, 3> p10 = pt(j + 1U, i);
      const std::array<double, 3> p11 = pt(j + 1U, i + 1U);
      push(p00);
      push(p01);
      push(p11);
      push(p00);
      push(p11);
      push(p10);
    }
  }
  const std::array<double, 3> south = pt(nlat, 0U);
  for (std::uint32_t i = 0; i < nlon; ++i) {
    push(south);
    push(pt(nlat - 1U, i));
    push(pt(nlat - 1U, i + 1U));
  }
  return out;
}

// ---- CPU reference integrator (fp64, independent RNG) -----------------------
//
// Surfaces: slab (floor) + 4 walls as boxes centered like the GPU scene.
// Face 4 (max |n.y|, normal +y) of the ceiling box is emissive in mode 1.

struct RoomHit {
  // 0 = none, 1..5 = slab, wall -z, wall +z, wall -x, wall +x. An index, so
  // unsigned: the 0 sentinel only works if nothing can go negative.
  std::size_t box;
  double t;
  double n[3];
};

// fp64 ray vs AABB (slab method), tmin = 0.
bool ray_box_fp64(const double o[3], const double d[3], const double c[3],
                  const double h[3], double& t_hit, double n_out[3]) {
  double tmin = 0.0, tmax = 1.0e30;
  int axis_min = -1;
  double sign_min = 0.0;
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(d[k]) < 1.0e-15) {
      if (std::fabs(o[k] - c[k]) > h[k]) return false;
      continue;
    }
    const double inv = 1.0 / d[k];
    double t0 = (c[k] - h[k] - o[k]) * inv;
    double t1 = (c[k] + h[k] - o[k]) * inv;
    double s = -1.0;
    if (t0 > t1) {
      std::swap(t0, t1);
      s = 1.0;
    }
    if (t0 > tmin) {
      tmin = t0;
      axis_min = k;
      sign_min = s;
    }
    tmax = std::min(tmax, t1);
    if (tmin > tmax) return false;
  }
  if (axis_min < 0) return false;  // origin inside: not used here
  t_hit = tmin;
  n_out[axis_min] = sign_min;
  return true;
}

RoomHit room_hit_fp64(const double o[3], const double d[3]) {
  struct B { double c[3], h[3]; };
  // slab (floor), wall -z, wall +z, wall -x, wall +x
  const B boxes[5] = {
      {{0, -2 * kRoomHy, 0}, {kRoomHx, kRoomHy, kRoomHx}},
      {{0, -kRoomHy, -kRoomHx}, {kRoomHx, kRoomHy, 0.01}},
      {{0, -kRoomHy, kRoomHx}, {kRoomHx, kRoomHy, 0.01}},
      {{-kRoomHx, -kRoomHy, 0}, {0.01, kRoomHy, kRoomHx}},
      {{kRoomHx, -kRoomHy, 0}, {0.01, kRoomHy, kRoomHx}}};
  RoomHit best{};
  best.box = 0;
  best.t = 1.0e30;
  for (std::size_t b = 0; b < static_cast<std::size_t>(5); ++b) {
    double t, n[3] = {0, 0, 0};
    if (ray_box_fp64(o, d, boxes[b].c, boxes[b].h, t, n) && t < best.t) {
      best = {b + 1U, t, {n[0], n[1], n[2]}};
    }
  }
  return best;
}

// ---- fp64 triangle soup (rotated-box reference) ----------------------------
// Möller–Trumbore, two-sided like the opaque GPU trace. The geometric
// normal is cross(e1, e2) flipped toward the incident ray (Faceforward),
// the same rule the fixed rchit applies, so the reference and the shader
// agree on tilted faces by construction. The flip matters: this repo's box
// winding makes the raw cross point INTO the solid, and without the flip
// both sides trap every path inside the geometry (GPU reads exactly 0).
struct TriSoupHit {
  bool hit{false};
  double t{1.0e30};
  double n[3]{0, 0, 0};
};

bool ray_tri_fp64(const double o[3], const double d[3], const double v0[3],
                  const double v1[3], const double v2[3], double& t_hit,
                  double n_out[3]) {
  const double e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
  const double e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
  const double p[3] = {d[1] * e2[2] - d[2] * e2[1],
                       d[2] * e2[0] - d[0] * e2[2],
                       d[0] * e2[1] - d[1] * e2[0]};
  const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::fabs(det) < 1.0e-18) return false;
  const double inv = 1.0 / det;
  const double tv[3] = {o[0] - v0[0], o[1] - v0[1], o[2] - v0[2]};
  const double u = (tv[0] * p[0] + tv[1] * p[1] + tv[2] * p[2]) * inv;
  if (u < 0.0 || u > 1.0) return false;
  const double q[3] = {tv[1] * e1[2] - tv[2] * e1[1],
                       tv[2] * e1[0] - tv[0] * e1[2],
                       tv[0] * e1[1] - tv[1] * e1[0]};
  const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0.0 || u + v > 1.0) return false;
  const double t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
  if (t <= 1.0e-9) return false;
  const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                       e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0]};
  const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (l <= 0.0) return false;
  double nx = n[0] / l, ny = n[1] / l, nz = n[2] / l;
  // Faceforward: match the rchit rule so the lobe samples the hemisphere
  // the ray came from. Without this the soup traps paths exactly like the
  // unflipped shader did (both read 0 and agree vacuously).
  if (nx * d[0] + ny * d[1] + nz * d[2] > 0.0) {
    nx = -nx; ny = -ny; nz = -nz;
  }
  n_out[0] = nx;
  n_out[1] = ny;
  n_out[2] = nz;
  t_hit = t;
  return true;
}

// Cosine-weighted hemisphere sample around +Z rotated into n (same Duff
// ONB as the shader), fp64, from an independent mt19937_64 stream.
void cosine_dir_fp64(const double n[3], std::mt19937_64& rng, double out[3]) {
  std::uniform_real_distribution<double> U(0.0, 1.0);
  const double r1 = U(rng);
  const double r2 = U(rng);
  const double phi = 6.283185307179586 * r1;
  const double s = std::sqrt(r2);
  const double tz[3] = {s * std::cos(phi), s * std::sin(phi),
                        std::sqrt(std::max(0.0, 1.0 - r2))};
  // True Duff et al. 2017 ONB (matches pt_real.rgen); the old projection
  // degenerated for z-axis normals (zero vector, normalized).
  const double sg = n[2] < 0.0 ? -1.0 : 1.0;
  const double a = -1.0 / (sg + n[2]);
  const double b = n[0] * n[1] * a;
  const double txv[3] = {1.0 + sg * n[0] * n[0] * a, sg * b, -sg * n[0]};
  const double tyv[3] = {b, sg + n[1] * n[1] * a, -n[1]};
  for (std::size_t k = 0; k < static_cast<std::size_t>(3); ++k) {
    out[k] = txv[k] * tz[0] + tyv[k] * tz[1] + n[k] * tz[2];
  }
}

// One full path. Grey albedo; sky = 1. Truncation at `bounces` contributes
// 0 (same bias direction as the GPU kernel).
struct PtScene {
  double albedo;
};

void trace_path_fp64(const double o0[3], const double d0[3], int bounces,
                     const PtScene& sc, std::mt19937_64& rng, double L[3]) {
  double T = 1.0;  // scalar throughput (grey albedo)
  double o[3] = {o0[0], o0[1], o0[2]};
  double d[3] = {d0[0], d0[1], d0[2]};
  for (std::size_t b = 0; b < static_cast<std::size_t>(bounces); ++b) {
    const RoomHit h = room_hit_fp64(o, d);
    if (h.box == 0) {
      L[0] += T; L[1] += T; L[2] += T;  // sky = 1
      return;
    }
    const double hit[3] = {o[0] + h.t * d[0], o[1] + h.t * d[1],
                           o[2] + h.t * d[2]};
    T *= sc.albedo;
    double nd[3];
    cosine_dir_fp64(h.n, rng, nd);
    for (std::size_t k = 0; k < static_cast<std::size_t>(3); ++k) {
      o[k] = hit[k] + h.n[k] * 1.0e-4;
      d[k] = nd[k];
    }
  }
}

// fp64 MC estimate of a probe pixel's radiance: the primary ray is jittered
// over the SAME pixel aperture as the GPU (uniform +-1/kImg in NDC), so both
// estimators integrate the same directional cone — the reference differs only
// in RNG recipe (mt19937_64 vs PCG), fp64 vs fp32 math, and intersection
// code. 262144 samples => sigma << tolerance.
std::array<double, 3> mc_reference(double ndc_x, double ndc_y,
                                   const double right[3], const double up[3],
                                   const double fwd[3], int bounces,
                                   const PtScene& sc, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> U(-1.0 / kImg, 1.0 / kImg);
  std::array<double, 3> acc{0.0, 0.0, 0.0};
  constexpr int kSpp = 262144;
  for (std::size_t s = 0; s < static_cast<std::size_t>(kSpp); ++s) {
    const double jx = U(rng);
    const double jy = U(rng);
    const double ndx = ndc_x + jx;
    const double ndy = ndc_y + jy;
    double d[3] = {ndx * right[0] - ndy * up[0] + fwd[0],
                   ndx * right[1] - ndy * up[1] + fwd[1],
                   ndx * right[2] - ndy * up[2] + fwd[2]};
    const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    d[0] /= dl; d[1] /= dl; d[2] /= dl;
    double L[3] = {0, 0, 0};
    const double o0[3] = {0.0, 0.0, 0.0};
    trace_path_fp64(o0, d, bounces, sc, rng, L);
    acc[0] += L[0]; acc[1] += L[1]; acc[2] += L[2];
  }
  for (std::size_t c = 0; c < static_cast<std::size_t>(3); ++c) acc[c] /= kSpp;
  return acc;
}

// ---- fp32 mirror of the GPU path (structural + single-bounce checks) -------

float rand01(std::uint32_t& state) noexcept {
  state = state * 747796405u + 2891336453u;
  const std::uint32_t word =
      ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return static_cast<float>(((word >> 22u) ^ word) & 0x00ffffffu) *
         (1.0f / 16777216.0f);
}

// fp32 ray vs the 5 room boxes; returns 0 = miss.
int room_hit_fp32(const std::array<float, 3>& o,
                  const std::array<float, 3>& d, float& t_hit,
                  std::array<float, 3>& n_out) noexcept {
  struct B { std::array<float, 3> c, h; };
  const B boxes[5] = {
      {{0.0f, -2 * kRoomHy, 0.0f}, {kRoomHx, kRoomHy, kRoomHx}},
      {{0.0f, -kRoomHy, -kRoomHx}, {kRoomHx, kRoomHy, 0.01f}},
      {{0.0f, -kRoomHy, kRoomHx}, {kRoomHx, kRoomHy, 0.01f}},
      {{-kRoomHx, -kRoomHy, 0.0f}, {0.01f, kRoomHy, kRoomHx}},
      {{kRoomHx, -kRoomHy, 0.0f}, {0.01f, kRoomHy, kRoomHx}}};
  int best_box = 0;
  float best_t = 1.0e30f;
  std::array<float, 3> best_n{0.0f, 0.0f, 0.0f};
  for (int b = 0; b < 5; ++b) {
    float tmin = 0.0f, tmax = 1.0e30f;
    int axis = -1;
    float sgn = 0.0f;
    bool hit = true;
    for (int k = 0; k < 3; ++k) {
      const float dk = d[static_cast<std::size_t>(k)];
      const float ok = o[static_cast<std::size_t>(k)];
      const float ck = boxes[static_cast<std::size_t>(b)].c[static_cast<std::size_t>(k)];
      const float hk = boxes[static_cast<std::size_t>(b)].h[static_cast<std::size_t>(k)];
      if (std::fabs(dk) < 1.0e-9f) {
        if (std::fabs(ok - ck) > hk) { hit = false; break; }
        continue;
      }
      const float inv = 1.0f / dk;
      float t0 = (ck - hk - ok) * inv;
      float t1 = (ck + hk - ok) * inv;
      float s = -1.0f;
      if (t0 > t1) { std::swap(t0, t1); s = 1.0f; }
      if (t0 > tmin) {
        tmin = t0;
        axis = k;
        sgn = s;
      }
      tmax = std::min(tmax, t1);
      if (tmin > tmax) { hit = false; break; }
    }
    if (hit && axis >= 0 && tmin < best_t) {
      best_t = tmin;
      best_box = b + 1;
      best_n = {0.0f, 0.0f, 0.0f};
      best_n[static_cast<std::size_t>(axis)] = sgn;
    }
  }
  t_hit = best_t;
  n_out = best_n;
  return best_box;
}

// GPU raygen fp32 mirror (mode 0, first two segments only — used for the
// no-bounce floor and sky-exactness checks; multi-bounce agreement is
// statistical via the fp64 integrator).
std::array<float, 3> gpu_first_segments(std::uint32_t px, std::uint32_t py,
                                        std::uint32_t frame,
                                        const std::array<float, 3>& right,
                                        const std::array<float, 3>& up,
                                        int segments) {
  std::uint32_t rng = px + kImg * (py + kImg * frame);
  const float jx = rand01(rng) - 0.5f;
  const float jy = rand01(rng) - 0.5f;
  const float u = (static_cast<float>(px) + jx) / static_cast<float>(kImg) * 2.0f - 1.0f;
  const float v = (static_cast<float>(py) + jy) / static_cast<float>(kImg) * 2.0f - 1.0f;
  std::array<float, 3> d{u * right[0] + (-v) * up[0] + kCamFwd[0],
                         u * right[1] + (-v) * up[1] + kCamFwd[1],
                         u * right[2] + (-v) * up[2] + kCamFwd[2]};
  const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  d = {d[0] / dl, d[1] / dl, d[2] / dl};

  std::array<float, 3> radiance{0.0f, 0.0f, 0.0f};
  float T = 1.0f;
  std::array<float, 3> o = kCamEye;
  for (std::size_t s = 0; s < static_cast<std::size_t>(segments); ++s) {
    float t;
    std::array<float, 3> n{};
    const int box = room_hit_fp32(o, d, t, n);
    if (box == 0) {
      radiance[0] += T * kSky; radiance[1] += T * kSky; radiance[2] += T * kSky;
      break;
    }
    T *= kAlbedo;  // grey
    // Cosine sample (same Duff ONB as the shader), fp32.
    const float r1 = rand01(rng);
    const float r2 = rand01(rng);
    const float phi = 6.28318530718f * r1;
    const float sq = std::sqrt(r2);
    const float tz[3] = {sq * std::cos(phi), sq * std::sin(phi),
                         std::sqrt(std::max(0.0f, 1.0f - r2))};
    const float sg = n[2] < 0.0f ? -1.0f : 1.0f;
    const float a = -1.0f / (sg + n[2]);
    const float b = n[0] * n[1] * a;
    const float txv[3] = {1.0f + sg * n[0] * n[0] * a, sg * b, -sg * n[0]};
    const float tyv[3] = {b, sg + n[1] * n[1] * a, -n[1]};
    std::array<float, 3> nd{txv[0] * tz[0] + tyv[0] * tz[1] + n[0] * tz[2],
                            txv[1] * tz[0] + tyv[1] * tz[1] + n[1] * tz[2],
                            txv[2] * tz[0] + tyv[2] * tz[1] + n[2] * tz[2]};
    const float nl = std::sqrt(nd[0] * nd[0] + nd[1] * nd[1] + nd[2] * nd[2]);
    nd = {nd[0] / nl, nd[1] / nl, nd[2] / nl};
    o = {o[0] + d[0] * t + n[0] * 1.0e-3f, o[1] + d[1] * t + n[1] * 1.0e-3f,
         o[2] + d[2] * t + n[2] * 1.0e-3f};
    d = nd;
  }
  return radiance;
}

bool submit_and_wait(VkDevice dev, VkQueue queue, VkCommandBuffer cb) {
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence{VK_NULL_HANDLE};
  if (vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS) return false;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
  if (r == VK_SUCCESS) {
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
  }
  vkDestroyFence(dev, fence, nullptr);
  return r == VK_SUCCESS;
}

}  // namespace

TEST(path_tracing_real, loop_pt_interior_mc_determinism) {
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("path_tracing_real", true).is_ok());
  if (!ctx.has_ray_tracing()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing (ray query) not available";
  }
  if (!ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

  // ---- Camera basis (fp32, matches the shaders) ------------------------------
  const std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
  std::array<float, 3> right{kCamFwd[1] * world_up[2] - kCamFwd[2] * world_up[1],
                             kCamFwd[2] * world_up[0] - kCamFwd[0] * world_up[2],
                             kCamFwd[0] * world_up[1] - kCamFwd[1] * world_up[0]};
  right = {right[0] / std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]),
           right[1] / std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]),
           right[2] / std::sqrt(right[0]*right[0]+right[1]*right[1]+right[2]*right[2])};
  const std::array<float, 3> up{right[1] * kCamFwd[2] - right[2] * kCamFwd[1],
                                right[2] * kCamFwd[0] - right[0] * kCamFwd[2],
                                right[0] * kCamFwd[1] - right[1] * kCamFwd[0]};

  // ---- Probe pixels: aimed at fp32-verified wall/slab/sky targets -----------
  auto pixel_for = [&](const std::array<float, 3>& p) {
    const std::array<float, 3> d = {
        p[0] - kCamEye[0], p[1] - kCamEye[1], p[2] - kCamEye[2]};
    const float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    const std::array<float, 3> dn{d[0] / dl, d[1] / dl, d[2] / dl};
    const float df = dn[0]*kCamFwd[0] + dn[1]*kCamFwd[1] + dn[2]*kCamFwd[2];
    const float dr = dn[0]*right[0] + dn[1]*right[1] + dn[2]*right[2];
    const float du = dn[0]*up[0] + dn[1]*up[1] + dn[2]*up[2];
    const float nx = dr / df;
    const float ny = -du / df;
    return std::make_pair(
        static_cast<int>((nx + 1.0f) * 0.5f * static_cast<float>(kImg)),
        static_cast<int>((ny + 1.0f) * 0.5f * static_cast<float>(kImg)));
  };
  // fp32-verified in-frustum targets (CPU room_hit_fp32 re-verifies each):
  //   floor (0,-5,-3): first hit = slab top.
  //   wall -z low  (0,-0.5,-5) and high (0,-2.5,-5): first hit = wall -z.
  //   open top (0,3,-20): ray rises over the wall tops -> miss (sky).
  const auto [sky_px, sky_py] = pixel_for({0.0f, 3.0f, -20.0f});
  const auto [slab_px, slab_py] = pixel_for({0.0f, -5.0f, -3.0f});
  const auto [wz_px, wz_py] = pixel_for({0.0f, -0.5f, -5.0f});
  const auto [wx_px, wx_py] = pixel_for({0.0f, -2.5f, -5.0f});
  ASSERT_GE(sky_px, 1); ASSERT_LT(sky_px, static_cast<int>(kImg) - 1);
  ASSERT_GE(sky_py, 1); ASSERT_LT(sky_py, static_cast<int>(kImg) - 1);
  ASSERT_GE(slab_px, 1); ASSERT_LT(slab_px, static_cast<int>(kImg) - 1);
  ASSERT_GE(slab_py, 1); ASSERT_LT(slab_py, static_cast<int>(kImg) - 1);
  ASSERT_GE(wz_px, 1); ASSERT_LT(wz_px, static_cast<int>(kImg) - 1);
  ASSERT_GE(wz_py, 1); ASSERT_LT(wz_py, static_cast<int>(kImg) - 1);
  ASSERT_GE(wx_px, 1); ASSERT_LT(wx_px, static_cast<int>(kImg) - 1);
  ASSERT_GE(wx_py, 1); ASSERT_LT(wx_py, static_cast<int>(kImg) - 1);

  // ---- Geometry: caller-owned vertex buffers -> meshes -----------------------
  // The path tracer owns the acceleration structures and the accumulation, not
  // the geometry buffers: device addresses in, BLASes out. This split is what
  // lets the same object serve a 5-box CPU-mirrored test room and, later, a
  // viewport scene whose meshes the app already uploads.
  const std::string sd = WARPLOOM_TEST_SHADER_DIR;
  std::vector<std::vector<float>> tri_data;
  auto add_box = [&](float hx, float hy, float hz) {
    tri_data.push_back(make_box_triangles(hx, hy, hz));
  };
  add_box(kRoomHx, kRoomHy, kRoomHx);  // floor
  add_box(kRoomHx, kRoomHy, 0.01f);    // wall -z
  add_box(kRoomHx, kRoomHy, 0.01f);    // wall +z
  add_box(0.01f, kRoomHy, kRoomHx);    // wall -x
  add_box(0.01f, kRoomHy, kRoomHx);    // wall +x

  std::vector<VkBuffer> geom_buffers;
  std::vector<Allocation> geom_allocs;
  for (std::size_t i = 0; i < tri_data.size(); ++i) {
    auto r = alloc.create_buffer(
        tri_data[i].size() * sizeof(float),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(r.is_ok());
    geom_allocs.push_back(r.value());
    geom_buffers.push_back(r.value().buffer);
    std::memcpy(r.value().mapped, tri_data[i].data(),
                tri_data[i].size() * sizeof(float));
  }
  auto addr_of = [&](VkBuffer b) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b;
    return vkGetBufferDeviceAddress(ctx.device(), &info);
  };
  std::vector<PathTracerMesh> meshes;
  for (std::size_t i = 0; i < tri_data.size(); ++i) {
    PathTracerMesh m{};
    m.vertex_buffer_address = addr_of(geom_buffers[i]);
    m.triangle_count = 12U;
    m.max_vertex = 35U;
    meshes.push_back(m);
  }

  const std::array<std::array<float, 3>, 5> centers = {
      std::array<float, 3>{0.0f, -2 * kRoomHy, 0.0f},
      std::array<float, 3>{0.0f, -kRoomHy, -kRoomHx},
      std::array<float, 3>{0.0f, -kRoomHy, kRoomHx},
      std::array<float, 3>{-kRoomHx, -kRoomHy, 0.0f},
      std::array<float, 3>{kRoomHx, -kRoomHy, 0.0f}};
  std::vector<PathTracerInstance> instances;
  for (std::size_t i = 0; i < centers.size(); ++i) {
    PathTracerInstance inst = PathTracerInstance::translated(centers[i], kAlbedo);
    inst.custom_index = static_cast<std::uint32_t>(i);
    instances.push_back(inst);
  }

  PathTracerConfig pt_cfg{};
  pt_cfg.shader_dir = sd;
  pt_cfg.width = kImg;
  pt_cfg.height = kImg;
  pt_cfg.max_bounces = kBounces;
  pt_cfg.meshes = meshes;
  pt_cfg.instances = instances;

  VulkanPathTracer tracer;
  ASSERT_TRUE(tracer
                  .create(ctx.physical_device(), ctx.device(),
                          ctx.graphics_queue(), qf, alloc, pt_cfg)
                  .is_ok());
  ASSERT_TRUE(tracer.ready()) << "path tracer refused on this device";

  PathTracerCamera cam{};
  cam.position = kCamEye;
  cam.right = right;
  cam.up = up;
  cam.forward = kCamFwd;
  tracer.set_camera(cam);

  auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool_r.is_ok());
  VkCommandPool cmd_pool = pool_r.value();

  // One accumulation sequence: clear on the first round, add the rest. The
  // tracer owns the image, the clear barrier and the cross-round visibility
  // barrier -- this lambda is now just "record and submit".
  auto record_round = [&](std::uint32_t frame, bool clear_first) {
    auto r_cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), cmd_pool);
    EXPECT_TRUE(r_cb_r.is_ok());
    VkCommandBuffer cb = r_cb_r.value();
    VkCommandBufferBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &rbi), VK_SUCCESS);
    tracer.record_round(cb, frame, clear_first);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    EXPECT_TRUE(submit_and_wait(ctx.device(), ctx.graphics_queue(), cb));
  };

  auto run_pass = [&]() -> std::vector<float> {
    record_round(0, true);
    for (std::uint32_t f = 1; f < kFrames; ++f) {
      record_round(f, false);
    }
    EXPECT_EQ(tracer.sample_count(), kFrames);
    auto mean = tracer.read_mean();
    EXPECT_TRUE(mean.is_ok());
    return mean.is_ok() ? mean.value() : std::vector<float>{};
  };

  auto pixel = [](const std::vector<float>& img, int px, int py,
                  int channel) {
    const std::size_t idx = (static_cast<std::size_t>(py) * kImg +
                             static_cast<std::size_t>(px)) * 4U +
                            static_cast<std::size_t>(channel);
    return img[idx];
  };

  // ================== Pass 1: probes vs independent fp64 MC ==================
  const std::vector<float> img = run_pass();

  // TEMP DEBUG: classify every pixel by red-channel mean.
  if (std::getenv("WARPLOOM_PT_DEBUG") != nullptr) {
    for (std::uint32_t y = 0; y < kImg; ++y) {
      std::string row;
      for (std::uint32_t x = 0; x < kImg; ++x) {
        const float v = pixel(img, static_cast<int>(x), static_cast<int>(y), 0);
        const float g = pixel(img, static_cast<int>(x), static_cast<int>(y), 1);
        row += v > 0.95f ? 'S' : (v > 0.01f ? '#' : (g > 0.01f ? 'g' : '.'));
      }
      std::fprintf(stderr, "%s\n", row.c_str());
    }
    std::fprintf(stderr, "sky=(%d,%d) slab=(%d,%d) wz=(%d,%d) wx=(%d,%d)\n",
                 sky_px, sky_py, slab_px, slab_py, wz_px, wz_py, wx_px,
                 wx_py);
    for (auto [px, py, nm] : {std::tuple{sky_px, sky_py, "sky"},
                              std::tuple{slab_px, slab_py, "slab"},
                              std::tuple{wz_px, wz_py, "wz"},
                              std::tuple{wx_px, wx_py, "wx"},
                              std::tuple{32, 30, "mid"},
                              std::tuple{60, 30, "right"},
                              std::tuple{10, 45, "floor-left"}}) {
      std::fprintf(stderr, "%s(%d,%d) rgb=(%.4f, %.4f, %.4f) a=%.4f\n", nm,
                   px, py, dbl(pixel(img, px, py, 0)),
                   dbl(pixel(img, px, py, 1)), dbl(pixel(img, px, py, 2)),
                   dbl(pixel(img, px, py, 3)));
    }
  }

  // Ground truth = an independent fp64 Monte-Carlo integrator (mt19937_64,
  // its own intersection code) driven over the same pixel aperture. The GPU
  // mean-of-64 must land on it; tolerance covers GPU MC noise (~2.5 sigma)
  // plus fp32/fp64 drift. No bit-matching, no shared RNG.
  auto probe_dir_ndc = [&](const std::array<float, 3>& target) {
    const std::array<float, 3> d{target[0] - kCamEye[0],
                                 target[1] - kCamEye[1],
                                 target[2] - kCamEye[2]};
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const std::array<float, 3> dn{d[0] / dl, d[1] / dl, d[2] / dl};
    const float df =
        dn[0] * kCamFwd[0] + dn[1] * kCamFwd[1] + dn[2] * kCamFwd[2];
    const float dr = dn[0] * right[0] + dn[1] * right[1] + dn[2] * right[2];
    const float du = dn[0] * up[0] + dn[1] * up[1] + dn[2] * up[2];
    return std::make_pair(static_cast<double>(dr / df),
                          static_cast<double>(-du / df));
  };
  const double R[3] = {static_cast<double>(right[0]),
                     static_cast<double>(right[1]),
                     static_cast<double>(right[2])};
  const double Uv[3] = {static_cast<double>(up[0]),
                      static_cast<double>(up[1]),
                      static_cast<double>(up[2])};
  const double F[3] = {static_cast<double>(kCamFwd[0]),
                     static_cast<double>(kCamFwd[1]),
                     static_cast<double>(kCamFwd[2])};
  PtScene sc{};
  sc.albedo = static_cast<double>(kAlbedo);
  const std::pair<const char*, std::array<float, 3>> probes[3] = {
      {"slab", {0.0f, -5.0f, -3.0f}},
      {"wall-z", {0.0f, -0.5f, -5.0f}},
      {"wall-z-hi", {0.0f, -2.5f, -5.0f}}};
  const int probe_px[3] = {slab_px, wz_px, wx_px};
  const int probe_py[3] = {slab_py, wz_py, wx_py};
  std::array<std::array<double, 3>, 3> ref8{};
  for (std::size_t i = 0; i < static_cast<std::size_t>(3); ++i) {
    const auto [nx, ny] = probe_dir_ndc(probes[static_cast<std::size_t>(i)].second);
    ref8[static_cast<std::size_t>(i)] = mc_reference(
        nx, ny, R, Uv, F, static_cast<int>(kBounces), sc,
        0x9E3779B97F4A7C15ULL + 0x1000ULL * static_cast<std::uint64_t>(i));
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(
          pixel(img, probe_px[i], probe_py[i], c),
          ref8[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)], 0.40)
          << probes[static_cast<std::size_t>(i)].first << " channel " << c
          << " (gpu=" << pixel(img, probe_px[i], probe_py[i], c)
          << ", fp64mc="
          << ref8[static_cast<std::size_t>(i)][static_cast<std::size_t>(c)]
          << ")";
    }
  }

  // Multi-bounce proof: the 1-bounce reference at the slab probe is far
  // below the 8-bounce reference (inter-reflected energy), and the GPU
  // image follows the 8-bounce value (checked above), not the 1-bounce one.
  {
    const auto [nx, ny] = probe_dir_ndc(probes[0].second);
    const auto ref1 = mc_reference(nx, ny, R, Uv, F, 1, sc,
                                   0x9E3779B97F4A7C15ULL);
    double gap = 0.0;
    for (int c = 0; c < 3; ++c) {
      gap += ref8[0][static_cast<std::size_t>(c)] -
             ref1[static_cast<std::size_t>(c)];
    }
    EXPECT_GT(gap, 0.15)
        << "multi-bounce energy must exceed the 1-bounce floor";
  }

  // Absorption sanity: interior probes are strictly below the sky radiance.
  for (std::size_t i = 0; i < static_cast<std::size_t>(3); ++i) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_LT(pixel(img, probe_px[i], probe_py[i], c),
                static_cast<float>(kSky) - 0.05f)
          << probes[static_cast<std::size_t>(i)].first
          << " absorption sanity channel " << c;
    }
  }

  // GPU-side anti-vacuity: the agreement checks above compare against a CPU
  // model that once SHARED the shader's sign error (inward normals trapped
  // every path on both sides, GPU and reference agreeing at exactly 0). Pin
  // the GPU image itself instead. Trapped transport reads 0; a collapsed
  // cosine lobe (the naive ONB on z-facing walls) reads exactly albedo.
  // Correct multi-bounce transport from these probes is strictly between.
  for (std::size_t i = 0; i < static_cast<std::size_t>(3); ++i) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_GT(pixel(img, probe_px[i], probe_py[i], c), 0.15F)
          << probes[static_cast<std::size_t>(i)].first
          << " trapped transport reads 0, channel " << c;
    }
  }
  for (std::size_t i = 1; i < static_cast<std::size_t>(3); ++i) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_LT(pixel(img, probe_px[i], probe_py[i], c), 0.79F)
          << probes[static_cast<std::size_t>(i)].first
          << " collapsed lobe reads exactly albedo, channel " << c;
    }
  }

  // Sky probe: exact radiance (fp32 mirror, first segment must miss).
  {
    const auto expect = gpu_first_segments(
        static_cast<std::uint32_t>(sky_px), static_cast<std::uint32_t>(sky_py),
        0U, right, up, 1);
    for (int c = 0; c < 3; ++c) {
      EXPECT_FLOAT_EQ(pixel(img, sky_px, sky_py, c),
                      expect[static_cast<std::size_t>(c)])
          << "open-sky probe channel " << c;
    }
  }

  // ================== Pass 2: determinism ====================================
  const std::vector<float> img2 = run_pass();
  ASSERT_EQ(img2.size(), img.size());
  EXPECT_EQ(std::memcmp(img2.data(), img.data(), img.size() * sizeof(float)),
            0) << "PT accumulation must be byte-identical across repeats";

  // Teardown order matters and is asserted by the leak check in the suite
  // teardown: the tracer releases AS storage and buffers BEFORE the allocator
  // that sub-allocated them, and the caller-owned geometry buffers outlive the
  // BLASes that reference their addresses.
  vkDestroyCommandPool(ctx.device(), cmd_pool, nullptr);
  tracer.destroy();
  for (auto& ga : geom_allocs) alloc.destroy_allocation(ga);
  alloc.cleanup();
  ctx.cleanup();
}

//! Faceforward orientation is outward, pinned with no Monte Carlo tolerance.
//! Dims.w = 2 (PathTracerConfig::visualize_normals) encodes the first-bounce
//! shading normal as color through the accumulation path: albedo * (n * 0.5
//! + 0.5). A sign error reads the inward color instead -- for the slab top
//! (0.4, 0.0, 0.4) rather than (0.4, 0.8, 0.4) -- and no tolerance or
//! reference model is involved: the expected values are exact axis colors.
//! This is the committed form of the transient normal-as-color measurement
//! that exposed the inward-normal defect (loop25): the MC agreement checks
//! cannot see a face-away flip in the all-grey room (through-solid
//! transport lands inside tolerance), so orientation gets its own
//! instrument rather than sharing the MC's blindness.
TEST(path_tracing_real, faceforward_normals_face_outward) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("path_tracing_real_vis", true).is_ok());
  if (!ctx.has_ray_tracing() || !ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

  const std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
  std::array<float, 3> right{kCamFwd[1] * world_up[2] - kCamFwd[2] * world_up[1],
                             kCamFwd[2] * world_up[0] - kCamFwd[0] * world_up[2],
                             kCamFwd[0] * world_up[1] - kCamFwd[1] * world_up[0]};
  {
    const float l =
        std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    right = {right[0] / l, right[1] / l, right[2] / l};
  }
  const std::array<float, 3> up{right[1] * kCamFwd[2] - right[2] * kCamFwd[1],
                                right[2] * kCamFwd[0] - right[0] * kCamFwd[2],
                                right[0] * kCamFwd[1] - right[1] * kCamFwd[0]};
  auto pixel_for = [&](const std::array<float, 3>& p) {
    const std::array<float, 3> d = {p[0] - kCamEye[0], p[1] - kCamEye[1],
                                    p[2] - kCamEye[2]};
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const std::array<float, 3> dn{d[0] / dl, d[1] / dl, d[2] / dl};
    const float df = dn[0] * kCamFwd[0] + dn[1] * kCamFwd[1] + dn[2] * kCamFwd[2];
    const float dr = dn[0] * right[0] + dn[1] * right[1] + dn[2] * right[2];
    const float du = dn[0] * up[0] + dn[1] * up[1] + dn[2] * up[2];
    return std::make_pair(
        static_cast<int>((dr / df + 1.0f) * 0.5f * static_cast<float>(kImg)),
        static_cast<int>((-du / df + 1.0f) * 0.5f * static_cast<float>(kImg)));
  };
  const auto [slab_px, slab_py] = pixel_for({0.0f, -5.0f, -3.0f});
  const auto [wz_px, wz_py] = pixel_for({0.0f, -2.5f, -5.0f});
  const auto [wx_px, wx_py] = pixel_for({5.0f, -4.0f, -4.0f});
  for (int v : {slab_px, slab_py, wz_px, wz_py, wx_px, wx_py}) {
    ASSERT_GE(v, 1);
    ASSERT_LT(v, static_cast<int>(kImg) - 1);
  }

  const std::string sd = WARPLOOM_TEST_SHADER_DIR;
  std::vector<std::vector<float>> tri_data;
  tri_data.push_back(make_box_triangles(static_cast<float>(kRoomHx),
                                        static_cast<float>(kRoomHy),
                                        static_cast<float>(kRoomHx)));
  tri_data.push_back(make_box_triangles(static_cast<float>(kRoomHx),
                                        static_cast<float>(kRoomHy), 0.01f));
  tri_data.push_back(make_box_triangles(static_cast<float>(kRoomHx),
                                        static_cast<float>(kRoomHy), 0.01f));
  tri_data.push_back(make_box_triangles(0.01f, static_cast<float>(kRoomHy),
                                        static_cast<float>(kRoomHx)));
  tri_data.push_back(make_box_triangles(0.01f, static_cast<float>(kRoomHy),
                                        static_cast<float>(kRoomHx)));
  std::vector<Allocation> geom_allocs;
  for (auto& td : tri_data) {
    auto r = alloc.create_buffer(
        td.size() * sizeof(float),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(r.is_ok());
    geom_allocs.push_back(r.value());
    std::memcpy(r.value().mapped, td.data(), td.size() * sizeof(float));
  }
  auto addr_of = [&](VkBuffer b) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b;
    return vkGetBufferDeviceAddress(ctx.device(), &info);
  };
  std::vector<PathTracerMesh> meshes;
  for (auto& ga : geom_allocs) {
    PathTracerMesh m{};
    m.vertex_buffer_address = addr_of(ga.buffer);
    m.triangle_count = 12U;
    m.max_vertex = 35U;
    meshes.push_back(m);
  }
  const std::array<std::array<float, 3>, 5> centers = {
      std::array<float, 3>{0.0f, -2 * kRoomHy, 0.0f},
      std::array<float, 3>{0.0f, -kRoomHy, -kRoomHx},
      std::array<float, 3>{0.0f, -kRoomHy, kRoomHx},
      std::array<float, 3>{-kRoomHx, -kRoomHy, 0.0f},
      std::array<float, 3>{kRoomHx, -kRoomHy, 0.0f}};
  std::vector<PathTracerInstance> instances;
  for (std::size_t i = 0; i < centers.size(); ++i) {
    PathTracerInstance inst = PathTracerInstance::translated(centers[i], kAlbedo);
    inst.custom_index = static_cast<std::uint32_t>(i);
    instances.push_back(inst);
  }

  PathTracerConfig pt_cfg{};
  pt_cfg.shader_dir = sd;
  pt_cfg.width = kImg;
  pt_cfg.height = kImg;
  pt_cfg.max_bounces = 2U;
  pt_cfg.meshes = meshes;
  pt_cfg.instances = instances;
  pt_cfg.visualize_normals = true;
  VulkanPathTracer tracer;
  ASSERT_TRUE(tracer
                  .create(ctx.physical_device(), ctx.device(),
                          ctx.graphics_queue(), qf, alloc, pt_cfg)
                  .is_ok());
  ASSERT_TRUE(tracer.ready());
  PathTracerCamera cam{};
  cam.position = kCamEye;
  cam.right = right;
  cam.up = up;
  cam.forward = kCamFwd;
  tracer.set_camera(cam);

  auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool_r.is_ok());
  auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      ctx.device(), pool_r.value());
  ASSERT_TRUE(cb_r.is_ok());
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb_r.value(), &bi), VK_SUCCESS);
  tracer.record_round(cb_r.value(), 0U, true);
  ASSERT_EQ(vkEndCommandBuffer(cb_r.value()), VK_SUCCESS);
  EXPECT_TRUE(submit_and_wait(ctx.device(), ctx.graphics_queue(), cb_r.value()));
  ASSERT_EQ(tracer.sample_count(), 1U);
  auto img_r = tracer.read_mean();
  ASSERT_TRUE(img_r.is_ok());
  const std::vector<float>& img = img_r.value();
  auto pixel = [&](int px, int py, int c) {
    return img[(static_cast<std::size_t>(py) * kImg +
                static_cast<std::size_t>(px)) *
                   4U +
               static_cast<std::size_t>(c)];
  };
  // albedo * (n * 0.5 + 0.5), tight: wrong-side colors differ by 0.4.
  // Normals are room-side: the wall standing at +x faces the room in -X.
  const std::array<std::array<float, 3>, 3> expect = {
      std::array<float, 3>{0.4f, 0.8f, 0.4f},  // slab top, +Y
      std::array<float, 3>{0.4f, 0.4f, 0.8f},  // wall -z, +Z
      std::array<float, 3>{0.0f, 0.4f, 0.4f}};  // wall +x, -X
  const int px[3] = {slab_px, wz_px, wx_px};
  const int py[3] = {slab_py, wz_py, wx_py};
  for (std::size_t i = 0; i < 3U; ++i) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(pixel(px[i], py[i], c), expect[i][static_cast<std::size_t>(c)],
                  5e-3)
          << "probe " << i << " channel " << c;
    }
  }

  vkDestroyCommandPool(ctx.device(), pool_r.value(), nullptr);
  tracer.destroy();
  for (auto& ga : geom_allocs) alloc.destroy_allocation(ga);
  alloc.cleanup();
  ctx.cleanup();
#else
  GTEST_SKIP() << "Vulkan unavailable";
#endif
}

//! One BLAS shared by two instances renders bit-identically to two duplicate
//! BLASes, and an out-of-range mesh_index refuses cleanly. The scene is
//! small on purpose: floor plus two translated unit cubes, 8 frames, full
//! image memcmp -- no reference model needed because both configs describe
//! the same geometry; only the BLAS count differs (3 vs 2).
TEST(path_tracing_real, shared_mesh_renders_identically) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("path_tracing_real_shared", true).is_ok());
  if (!ctx.has_ray_tracing() || !ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
  std::array<float, 3> right{kCamFwd[1] * world_up[2] - kCamFwd[2] * world_up[1],
                             kCamFwd[2] * world_up[0] - kCamFwd[0] * world_up[2],
                             kCamFwd[0] * world_up[1] - kCamFwd[1] * world_up[0]};
  {
    const float l =
        std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    right = {right[0] / l, right[1] / l, right[2] / l};
  }
  const std::array<float, 3> up{right[1] * kCamFwd[2] - right[2] * kCamFwd[1],
                                right[2] * kCamFwd[0] - right[0] * kCamFwd[2],
                                right[0] * kCamFwd[1] - right[1] * kCamFwd[0]};
  PathTracerCamera cam{};
  cam.position = kCamEye;
  cam.right = right;
  cam.up = up;
  cam.forward = kCamFwd;

  const std::vector<float> floor_tris = make_box_triangles(5.0f, 5.0f, 5.0f);
  const std::vector<float> cube_tris = make_box_triangles(1.0f, 1.0f, 1.0f);
  const std::array<float, 3> floor_at{0.0f, -10.0f, 0.0f};
  const std::array<float, 3> cube_a_at{-1.5f, -3.5f, -4.0f};
  const std::array<float, 3> cube_b_at{1.5f, -2.5f, -6.0f};
  auto run_scene = [&](std::vector<std::vector<float>> tris,
                       std::vector<PathTracerInstance> insts) {
    std::vector<Allocation> geos;
    std::vector<PathTracerMesh> meshes;
    for (auto& td : tris) {
      auto r = alloc.create_buffer(
          td.size() * sizeof(float),
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!r.is_ok()) return std::vector<float>{};
      geos.push_back(r.value());
      std::memcpy(r.value().mapped, td.data(), td.size() * sizeof(float));
      VkBufferDeviceAddressInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
      ai.buffer = r.value().buffer;
      PathTracerMesh m{};
      m.vertex_buffer_address = vkGetBufferDeviceAddress(ctx.device(), &ai);
      m.triangle_count = 12U;
      m.max_vertex = 35U;
      meshes.push_back(m);
    }
    PathTracerConfig cfg{};
    cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
    cfg.width = kImg;
    cfg.height = kImg;
    cfg.max_bounces = 4U;
    cfg.meshes = meshes;
    cfg.instances = insts;
    VulkanPathTracer tracer;
    const bool created = tracer
                             .create(ctx.physical_device(), ctx.device(),
                                     ctx.graphics_queue(), qf, alloc, cfg)
                             .is_ok();
    std::vector<float> img;
    if (created && tracer.ready()) {
      tracer.set_camera(cam);
      auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
          ctx.device(), qf);
      if (pool_r.is_ok()) {
        for (std::uint32_t f = 0; f < 8U; ++f) {
          auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
              ctx.device(), pool_r.value());
          if (!cb_r.is_ok()) break;
          VkCommandBufferBeginInfo bi{};
          bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
          bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
          if (vkBeginCommandBuffer(cb_r.value(), &bi) != VK_SUCCESS) break;
          tracer.record_round(cb_r.value(), f, f == 0U);
          if (vkEndCommandBuffer(cb_r.value()) != VK_SUCCESS) break;
          if (!submit_and_wait(ctx.device(), ctx.graphics_queue(),
                               cb_r.value())) {
            break;
          }
        }
        auto img_r = tracer.read_mean();
        if (img_r.is_ok()) img = img_r.value();
        vkDestroyCommandPool(ctx.device(), pool_r.value(), nullptr);
      }
    }
    tracer.destroy();
    for (auto& g : geos) alloc.destroy_allocation(g);
    return img;
  };

  PathTracerInstance floor_inst =
      PathTracerInstance::translated(floor_at, kAlbedo);
  const PathTracerInstance cube_a =
      PathTracerInstance::translated(cube_a_at, kAlbedo);
  const PathTracerInstance cube_b =
      PathTracerInstance::translated(cube_b_at, kAlbedo);
  // Duplicate-BLAS config: three buffers, identity mapping (sentinel).
  const std::vector<float> img_dup = run_scene({floor_tris, cube_tris, cube_tris},
                                               {floor_inst, cube_a, cube_b});
  // Shared-BLAS config: two buffers, both cubes name mesh 1.
  PathTracerInstance cube_a_s = cube_a;
  PathTracerInstance cube_b_s = cube_b;
  cube_a_s.mesh_index = 1U;
  cube_b_s.mesh_index = 1U;
  const std::vector<float> img_shared =
      run_scene({floor_tris, cube_tris}, {floor_inst, cube_a_s, cube_b_s});
  ASSERT_FALSE(img_dup.empty());
  ASSERT_EQ(img_dup.size(), img_shared.size());
  EXPECT_EQ(std::memcmp(img_dup.data(), img_shared.data(),
                        img_dup.size() * sizeof(float)),
            0)
      << "shared BLAS must render bit-identically to duplicate BLASes";

  // Out-of-range mesh_index refuses cleanly: empty image, never ready.
  {
    PathTracerInstance bad =
        PathTracerInstance::translated(cube_a_at, kAlbedo);
    bad.mesh_index = 7U;
    EXPECT_TRUE(run_scene({floor_tris, cube_tris}, {floor_inst, bad}).empty());
  }
  alloc.cleanup();
  ctx.cleanup();
#else
  GTEST_SKIP() << "Vulkan unavailable";
#endif
}

//! An app-scene-shaped PT config -- several meshes with general (rotated +
//! translated) instance transforms, the shape setup_path_tracing builds for
//! mannequin/city scenes -- renders deterministically and every mesh
//! contributes. No CPU reference: the claims are (a) a finite in-range
//! image, (b) a byte-identical repeat, and (c) removing the third mesh
//! changes the image, so an unwired mesh cannot hide behind a passing test.
TEST(path_tracing_real, app_scene_general_transforms_contribute) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("path_tracing_real_appscene", true).is_ok());
  if (!ctx.has_ray_tracing() || !ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
  std::array<float, 3> right{kCamFwd[1] * world_up[2] - kCamFwd[2] * world_up[1],
                             kCamFwd[2] * world_up[0] - kCamFwd[0] * world_up[2],
                             kCamFwd[0] * world_up[1] - kCamFwd[1] * world_up[0]};
  {
    const float l =
        std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    right = {right[0] / l, right[1] / l, right[2] / l};
  }
  const std::array<float, 3> up{right[1] * kCamFwd[2] - right[2] * kCamFwd[1],
                                right[2] * kCamFwd[0] - right[0] * kCamFwd[2],
                                right[0] * kCamFwd[1] - right[1] * kCamFwd[0]};
  PathTracerCamera cam{};
  cam.position = kCamEye;
  cam.right = right;
  cam.up = up;
  cam.forward = kCamFwd;

  const std::vector<float> floor_tris = make_box_triangles(5.0f, 5.0f, 5.0f);
  const std::vector<float> cube_tris = make_box_triangles(1.0f, 1.0f, 1.0f);
  const std::array<float, 3> floor_at{0.0f, -10.0f, 0.0f};
  const std::array<float, 3> cube_at{-1.5f, -3.5f, -4.0f};
  auto run_scene = [&](std::vector<std::vector<float>> tris,
                       std::vector<PathTracerInstance> insts) {
    std::vector<Allocation> geos;
    std::vector<PathTracerMesh> meshes;
    for (auto& td : tris) {
      auto r = alloc.create_buffer(
          td.size() * sizeof(float),
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!r.is_ok()) return std::vector<float>{};
      geos.push_back(r.value());
      std::memcpy(r.value().mapped, td.data(), td.size() * sizeof(float));
      VkBufferDeviceAddressInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
      ai.buffer = r.value().buffer;
      PathTracerMesh m{};
      m.vertex_buffer_address = vkGetBufferDeviceAddress(ctx.device(), &ai);
      m.triangle_count = 12U;
      m.max_vertex = 35U;
      meshes.push_back(m);
    }
    PathTracerConfig cfg{};
    cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
    cfg.width = kImg;
    cfg.height = kImg;
    cfg.max_bounces = 4U;
    cfg.meshes = meshes;
    cfg.instances = insts;
    VulkanPathTracer tracer;
    const bool created = tracer
                             .create(ctx.physical_device(), ctx.device(),
                                     ctx.graphics_queue(), qf, alloc, cfg)
                             .is_ok();
    std::vector<float> img;
    if (created && tracer.ready()) {
      tracer.set_camera(cam);
      auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
          ctx.device(), qf);
      if (pool_r.is_ok()) {
        for (std::uint32_t f = 0; f < 8U; ++f) {
          auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
              ctx.device(), pool_r.value());
          if (!cb_r.is_ok()) break;
          VkCommandBufferBeginInfo bi{};
          bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
          bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
          if (vkBeginCommandBuffer(cb_r.value(), &bi) != VK_SUCCESS) break;
          tracer.record_round(cb_r.value(), f, f == 0U);
          if (vkEndCommandBuffer(cb_r.value()) != VK_SUCCESS) break;
          if (!submit_and_wait(ctx.device(), ctx.graphics_queue(),
                               cb_r.value())) {
            break;
          }
        }
        auto img_r = tracer.read_mean();
        if (img_r.is_ok()) img = img_r.value();
        vkDestroyCommandPool(ctx.device(), pool_r.value(), nullptr);
      }
    }
    tracer.destroy();
    for (auto& g : geos) alloc.destroy_allocation(g);
    return img;
  };

  // Third mesh through a general transform: Y-rotated 30 degrees and
  // translated, the mannequin-part shape (object.model * bone), row-major
  // 3x4 like the rotated-box probe. Explicit mesh_index, not the sentinel.
  const double kAng = 30.0 * 3.141592653589793 / 180.0;
  const float c = static_cast<float>(std::cos(kAng));
  const float s = static_cast<float>(std::sin(kAng));
  PathTracerInstance posed{};
  posed.center = {1.5f, -2.5f, -6.0f, 0.0F};
  posed.albedo = {0.8f, 0.8f, 0.8f, 0.0F};
  posed.mesh_index = 1U;
  posed.transform = {c, 0.0f, s, 1.5f,  //
                     0.0f, 1.0f, 0.0f, -2.5f,  //
                     -s, 0.0f, c, -6.0f};
  const std::vector<float> img_full =
      run_scene({floor_tris, cube_tris},
                {PathTracerInstance::translated(floor_at, kAlbedo),
                 PathTracerInstance::translated(cube_at, kAlbedo), posed});
  ASSERT_FALSE(img_full.empty());
  ASSERT_EQ(img_full.size(), static_cast<std::size_t>(kImg) * kImg * 4U);
  for (float v : img_full) {
    EXPECT_TRUE(std::isfinite(v)) << "non-finite accumulation mean";
    EXPECT_GE(v, 0.0f);
    EXPECT_LE(v, 1.0f + 1e-6f);
  }
  // Deterministic: an identical second run is bit-identical.
  const std::vector<float> img_repeat =
      run_scene({floor_tris, cube_tris},
                {PathTracerInstance::translated(floor_at, kAlbedo),
                 PathTracerInstance::translated(cube_at, kAlbedo), posed});
  ASSERT_EQ(img_repeat.size(), img_full.size());
  EXPECT_EQ(std::memcmp(img_full.data(), img_repeat.data(),
                        img_full.size() * sizeof(float)),
            0)
      << "identical app-scene config must render bit-identically";
  // The third mesh contributes: without it the image differs.
  const std::vector<float> img_pair =
      run_scene({floor_tris, cube_tris},
                {PathTracerInstance::translated(floor_at, kAlbedo),
                 PathTracerInstance::translated(cube_at, kAlbedo)});
  ASSERT_EQ(img_pair.size(), img_full.size());
  EXPECT_NE(std::memcmp(img_full.data(), img_pair.data(),
                        img_full.size() * sizeof(float)),
            0)
      << "a third mesh that changes nothing is an unwired mesh";
  alloc.cleanup();
  ctx.cleanup();
#else
  GTEST_SKIP() << "Vulkan unavailable";
#endif
}

//! Geometric rchit normals agree with an independent fp64 reference on a
//! ROTATED box. The old dominant-axis normal (hit-minus-center) is exact for
//! axis-aligned boxes, which is why every existing probe passes, but reports
//! side normals over most of a thin slab's top face and garbage for any
//! rotated instance -- the thing roadmap item 2 needs. This test renders a
//! 30-degree-rotated box over the floor and compares two probes against a
//! 262144-sample fp64 MC with its own triangle intersection code. It fails
//! on the dominant-axis shader (biased bounce hemispheres) and passes on
//! the geometric one; the floor control probe passes on both.
TEST(path_tracing_real, rotated_box_matches_reference) {
#if WARPLOOM_VULKAN_TYPES_AVAILABLE && defined(WARPLOOM_TEST_SHADER_DIR)
  if (!omnicpp::render::VulkanContext::is_available()) {
    GTEST_SKIP() << "Vulkan loader unavailable";
  }
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtrRotated", true).is_ok());
  if (!ctx.has_ray_tracing() || !ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

  // ---- Camera basis (fp32, matches the shaders) ---------------------------
  const std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
  std::array<float, 3> right{kCamFwd[1] * world_up[2] - kCamFwd[2] * world_up[1],
                             kCamFwd[2] * world_up[0] - kCamFwd[0] * world_up[2],
                             kCamFwd[0] * world_up[1] - kCamFwd[1] * world_up[0]};
  {
    const float l =
        std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    right = {right[0] / l, right[1] / l, right[2] / l};
  }
  const std::array<float, 3> up{right[1] * kCamFwd[2] - right[2] * kCamFwd[1],
                                right[2] * kCamFwd[0] - right[0] * kCamFwd[2],
                                right[0] * kCamFwd[1] - right[1] * kCamFwd[0]};

  // ---- Rotated box: 30 degrees about Y, sitting on the floor ---------------
  // Object space stays axis-aligned; the INSTANCE transform carries the
  // rotation -- the path the viewport will use, exercising WorldToObject.
  // (Baking the rotation into the vertices instead would leave the
  // instance transform identity and test nothing new.)
  constexpr double kAng = 30.0 * 3.141592653589793 / 180.0;
  const double ca = std::cos(kAng), sa = std::sin(kAng);
  constexpr float kBoxH = 1.5f;
  constexpr std::array<float, 3> kBoxC{2.0f, -3.5f, -2.5f};
  const std::vector<float> box_tris = make_box_triangles(kBoxH, kBoxH, kBoxH);
  // fp64 soup for the reference: same rotation in double precision.
  struct Tri { double v[3][3]; };
  std::vector<Tri> soup;
  {
    std::vector<float> base = make_box_triangles(kBoxH, kBoxH, kBoxH);
    for (std::size_t t = 0; t < base.size(); t += 9U) {
      Tri tri{};
      for (std::size_t k = 0; k < 3U; ++k) {
        const double x = static_cast<double>(base[t + k * 3U]);
        const double y = static_cast<double>(base[t + k * 3U + 1U]);
        const double z = static_cast<double>(base[t + k * 3U + 2U]);
        tri.v[k][0] = x * ca + z * sa + static_cast<double>(kBoxC[0]);
        tri.v[k][1] = y + static_cast<double>(kBoxC[1]);
        tri.v[k][2] = -x * sa + z * ca + static_cast<double>(kBoxC[2]);
      }
      soup.push_back(tri);
    }
  }

  // ---- GPU geometry: floor + 4 walls (AABB boxes) + rotated box -----------
  const std::string sd = WARPLOOM_TEST_SHADER_DIR;
  std::vector<std::vector<float>> tri_data;
  tri_data.push_back(make_box_triangles(static_cast<float>(kRoomHx),
                                        static_cast<float>(kRoomHy),
                                        static_cast<float>(kRoomHx)));
  tri_data.push_back(make_box_triangles(static_cast<float>(kRoomHx),
                                        static_cast<float>(kRoomHy), 0.01f));
  tri_data.push_back(make_box_triangles(static_cast<float>(kRoomHx),
                                        static_cast<float>(kRoomHy), 0.01f));
  tri_data.push_back(make_box_triangles(0.01f, static_cast<float>(kRoomHy),
                                        static_cast<float>(kRoomHx)));
  tri_data.push_back(make_box_triangles(0.01f, static_cast<float>(kRoomHy),
                                        static_cast<float>(kRoomHx)));
  tri_data.push_back(box_tris);
  std::vector<Allocation> geom_allocs;
  std::vector<VkBuffer> geom_buffers;
  for (auto& td : tri_data) {
    auto r = alloc.create_buffer(
        td.size() * sizeof(float),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(r.is_ok());
    geom_allocs.push_back(r.value());
    geom_buffers.push_back(r.value().buffer);
    std::memcpy(r.value().mapped, td.data(), td.size() * sizeof(float));
  }
  auto addr_of = [&](VkBuffer b) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b;
    return vkGetBufferDeviceAddress(ctx.device(), &info);
  };
  std::vector<PathTracerMesh> meshes;
  for (auto b : geom_buffers) {
    PathTracerMesh m{};
    m.vertex_buffer_address = addr_of(b);
    m.triangle_count = 12U;
    m.max_vertex = 35U;
    meshes.push_back(m);
  }
  const std::array<std::array<float, 3>, 6> centers = {
      std::array<float, 3>{0.0f, -2 * static_cast<float>(kRoomHy), 0.0f},
      std::array<float, 3>{0.0f, -static_cast<float>(kRoomHy),
                           -static_cast<float>(kRoomHx)},
      std::array<float, 3>{0.0f, -static_cast<float>(kRoomHy),
                           static_cast<float>(kRoomHx)},
      std::array<float, 3>{-static_cast<float>(kRoomHx),
                           -static_cast<float>(kRoomHy), 0.0f},
      std::array<float, 3>{static_cast<float>(kRoomHx),
                           -static_cast<float>(kRoomHy), 0.0f},
      kBoxC};
  std::vector<PathTracerInstance> instances;
  for (std::size_t i = 0; i < centers.size(); ++i) {
    PathTracerInstance inst = PathTracerInstance::translated(centers[i], kAlbedo);
    inst.custom_index = static_cast<std::uint32_t>(i);
    instances.push_back(inst);
  }
  // Instance 5 carries the Y rotation in its TLAS transform (row-major 3x4:
  // [cos,0,sin,tx, 0,1,0,ty, -sin,0,cos,tz]).
  {
    const float c = static_cast<float>(ca), s = static_cast<float>(sa);
    instances[5].transform = {c, 0.0f, s, kBoxC[0],  //
                              0.0f, 1.0f, 0.0f, kBoxC[1],  //
                              -s, 0.0f, c, kBoxC[2]};
  }

  PathTracerConfig pt_cfg{};
  pt_cfg.shader_dir = sd;
  pt_cfg.width = kImg;
  pt_cfg.height = kImg;
  pt_cfg.max_bounces = kBounces;
  pt_cfg.meshes = meshes;
  pt_cfg.instances = instances;
  VulkanPathTracer tracer;
  ASSERT_TRUE(tracer
                  .create(ctx.physical_device(), ctx.device(),
                          ctx.graphics_queue(), qf, alloc, pt_cfg)
                  .is_ok());
  ASSERT_TRUE(tracer.ready());
  PathTracerCamera cam{};
  cam.position = kCamEye;
  cam.right = right;
  cam.up = up;
  cam.forward = kCamFwd;
  tracer.set_camera(cam);

  auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(ctx.device(), qf);
  ASSERT_TRUE(pool_r.is_ok());
  VkCommandPool cmd_pool = pool_r.value();
  auto record_round = [&](std::uint32_t frame, bool clear_first) {
    auto r_cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), cmd_pool);
    ASSERT_TRUE(r_cb_r.is_ok());
    VkCommandBuffer cb = r_cb_r.value();
    VkCommandBufferBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &rbi), VK_SUCCESS);
    tracer.record_round(cb, frame, clear_first);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    EXPECT_TRUE(submit_and_wait(ctx.device(), ctx.graphics_queue(), cb));
  };
  for (std::uint32_t f = 0; f < kFrames; ++f) record_round(f, f == 0U);
  EXPECT_EQ(tracer.sample_count(), kFrames);
  auto mean = tracer.read_mean();
  ASSERT_TRUE(mean.is_ok());
  const std::vector<float>& img = mean.value();
  auto pixel = [&](int px, int py, int c) {
    return img[(static_cast<std::size_t>(py) * kImg +
                static_cast<std::size_t>(px)) * 4U +
               static_cast<std::size_t>(c)];
  };
  auto pixel_for = [&](const std::array<float, 3>& p) {
    const std::array<float, 3> d = {p[0] - kCamEye[0], p[1] - kCamEye[1],
                                    p[2] - kCamEye[2]};
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float df = (d[0] * kCamFwd[0] + d[1] * kCamFwd[1] + d[2] * kCamFwd[2]) / dl;
    const float dr = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / dl;
    const float du = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) / dl;
    const float nx = dr / df, ny = -du / df;
    return std::make_pair(
        static_cast<int>((nx + 1.0f) * 0.5f * static_cast<float>(kImg)),
        static_cast<int>((ny + 1.0f) * 0.5f * static_cast<float>(kImg)));
  };

  // True Duff et al. 2017 ONB, fp64 mirror of pt_real.rgen (now that both
  // the shader and cosine_dir_fp64 use it, this is the same form; kept as
  // a separate lambda because the rotated-box reference predates the fix).
  auto cosine_duff = [](const double n[3], std::mt19937_64& rng, double out[3]) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const double r1 = U(rng), r2 = U(rng);
    const double phi = 6.283185307179586 * r1;
    const double s = std::sqrt(r2);
    const double t[3] = {s * std::cos(phi), s * std::sin(phi),
                         std::sqrt(std::max(0.0, 1.0 - r2))};
    const double sg = n[2] < 0.0 ? -1.0 : 1.0;
    const double a = -1.0 / (sg + n[2]);
    const double b = n[0] * n[1] * a;
    const double txv[3] = {1.0 + sg * n[0] * n[0] * a, sg * b, -sg * n[0]};
    const double tyv[3] = {b, sg + n[1] * n[1] * a, -n[1]};
    for (int k = 0; k < 3; ++k)
      out[k] = txv[k] * t[0] + tyv[k] * t[1] + n[k] * t[2];
  };

  // Reference trace: room AABBs plus the rotated triangle soup. Returns
  // hit status, surface id (1..5 room, 6 box), distance and normal.
  struct RefHit {
    bool found{false};
    int id{0};
    double t{1.0e30};
    double n[3]{0, 0, 0};
  };
  auto ref_trace = [&](const double o[3], const double d[3]) {
    RefHit best{};
    RoomHit b = room_hit_fp64(o, d);
    if (b.box != 0) {
      best.found = true;
      best.id = static_cast<int>(b.box);
      best.t = b.t;
      best.n[0] = b.n[0]; best.n[1] = b.n[1]; best.n[2] = b.n[2];
    }
    for (const auto& tri : soup) {
      double t;
      double n[3];
      if (ray_tri_fp64(o, d, tri.v[0], tri.v[1], tri.v[2], t, n) && t < best.t) {
        best.found = true;
        best.id = 6;
        best.t = t;
        best.n[0] = n[0]; best.n[1] = n[1]; best.n[2] = n[2];
      }
    }
    return best;
  };
  PtScene sc{};
  sc.albedo = static_cast<double>(kAlbedo);
  auto mc_ref = [&](double ndc_x, double ndc_y, const double R[3],
                    const double Uv[3], const double F[3], std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(-1.0 / kImg, 1.0 / kImg);
    std::array<double, 3> acc{0.0, 0.0, 0.0};
    constexpr int kSpp = 262144;
    for (int s = 0; s < kSpp; ++s) {
      const double jx = U(rng), jy = U(rng);
      double d[3] = {(ndc_x + jx) * R[0] - (ndc_y + jy) * Uv[0] + F[0],
                     (ndc_x + jx) * R[1] - (ndc_y + jy) * Uv[1] + F[1],
                     (ndc_x + jx) * R[2] - (ndc_y + jy) * Uv[2] + F[2]};
      const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      d[0] /= dl; d[1] /= dl; d[2] /= dl;
      double o[3] = {0.0, 0.0, 0.0};
      double T = 1.0;
      double L[3] = {0, 0, 0};
      for (int bix = 0; bix < static_cast<int>(kBounces); ++bix) {
        const RefHit h = ref_trace(o, d);
        if (!h.found) { L[0] += T; L[1] += T; L[2] += T; break; }
        T *= sc.albedo;
        double nd[3];
        cosine_duff(h.n, rng, nd);
        // March: hit point plus a normal offset against self-intersection.
        // Each line touches only its own component while d is still the
        // incident direction, so the three updates compose exactly.
        o[0] += d[0] * h.t + h.n[0] * 1.0e-4;
        o[1] += d[1] * h.t + h.n[1] * 1.0e-4;
        o[2] += d[2] * h.t + h.n[2] * 1.0e-4;
        d[0] = nd[0]; d[1] = nd[1]; d[2] = nd[2];
      }
      acc[0] += L[0]; acc[1] += L[1]; acc[2] += L[2];
    }
    for (std::size_t c = 0; c < 3U; ++c) {
      acc[c] /= static_cast<double>(kSpp);
    }
    return acc;
  };
  const double R[3] = {static_cast<double>(right[0]),
                       static_cast<double>(right[1]),
                       static_cast<double>(right[2])};
  const double Uv[3] = {static_cast<double>(up[0]),
                        static_cast<double>(up[1]),
                        static_cast<double>(up[2])};
  const double F[3] = {static_cast<double>(kCamFwd[0]),
                       static_cast<double>(kCamFwd[1]),
                       static_cast<double>(kCamFwd[2])};
  auto probe_ndc = [&](const std::array<float, 3>& target) {
    const float dx = target[0] - kCamEye[0], dy = target[1] - kCamEye[1],
                    dz = target[2] - kCamEye[2];
    const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float df = (dx * kCamFwd[0] + dy * kCamFwd[1] + dz * kCamFwd[2]) / dl;
    const float dr = (dx * right[0] + dy * right[1] + dz * right[2]) / dl;
    const float du = (dx * up[0] + dy * up[1] + dz * up[2]) / dl;
    return std::make_pair(static_cast<double>(dr / df),
                          static_cast<double>(-du / df));
  };
  // Box probe: centre of the face pointing at the camera. The face normal
  // is Ry(30deg) applied to -z: (-sin30, 0, -cos30); the surface point is
  // centre + normal * half-extent.
  const double fn[3] = {-0.5, 0.0, -0.8660254037844386};
  const std::array<float, 3> box_target{
      static_cast<float>(static_cast<double>(kBoxC[0]) +
                         fn[0] * static_cast<double>(kBoxH)),
      static_cast<float>(kBoxC[1]),
      static_cast<float>(static_cast<double>(kBoxC[2]) +
                         fn[2] * static_cast<double>(kBoxH))};
  const std::array<float, 3> floor_target{-3.0f, -5.0f, -2.0f};
  const auto [box_px, box_py] = pixel_for(box_target);
  const auto [fl_px, fl_py] = pixel_for(floor_target);
  ASSERT_GE(box_px, 1); ASSERT_LT(box_px, static_cast<int>(kImg) - 1);
  ASSERT_GE(box_py, 1); ASSERT_LT(box_py, static_cast<int>(kImg) - 1);
  ASSERT_GE(fl_px, 1); ASSERT_LT(fl_px, static_cast<int>(kImg) - 1);
  ASSERT_GE(fl_py, 1); ASSERT_LT(fl_py, static_cast<int>(kImg) - 1);
  // Anti-vacuity: the reference must first-hit the intended surfaces.
  {
    const auto [bx, by] = probe_ndc(box_target);
    double d[3] = {bx * R[0] - by * Uv[0] + F[0], bx * R[1] - by * Uv[1] + F[1],
                   bx * R[2] - by * Uv[2] + F[2]};
    const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    d[0] /= dl; d[1] /= dl; d[2] /= dl;
    const double o[3] = {0.0, 0.0, 0.0};
    EXPECT_EQ(ref_trace(o, d).id, 6) << "box probe misses the rotated box";
    const auto [fx, fy] = probe_ndc(floor_target);
    double fd[3] = {fx * R[0] - fy * Uv[0] + F[0], fx * R[1] - fy * Uv[1] + F[1],
                    fx * R[2] - fy * Uv[2] + F[2]};
    const double fl = std::sqrt(fd[0] * fd[0] + fd[1] * fd[1] + fd[2] * fd[2]);
    fd[0] /= fl; fd[1] /= fl; fd[2] /= fl;
    EXPECT_EQ(ref_trace(o, fd).id, 1) << "floor probe misses the floor";
  }
  const auto [box_nx, box_ny] = probe_ndc(box_target);
  const auto box_ref = mc_ref(box_nx, box_ny, R, Uv, F, 0x51EDU);
  const auto [fl_nx, fl_ny] = probe_ndc(floor_target);
  const auto floor_ref = mc_ref(fl_nx, fl_ny, R, Uv, F, 0xF100D5EEDULL);
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(pixel(box_px, box_py, c), box_ref[static_cast<std::size_t>(c)],
                0.40)
        << "rotated-box probe channel " << c << " (gpu="
        << pixel(box_px, box_py, c) << ", fp64mc="
        << box_ref[static_cast<std::size_t>(c)] << ")";
    EXPECT_NEAR(pixel(fl_px, fl_py, c), floor_ref[static_cast<std::size_t>(c)],
                0.40)
        << "floor control channel " << c;
  }

  vkDestroyCommandPool(ctx.device(), cmd_pool, nullptr);
  tracer.destroy();
  for (auto& ga : geom_allocs) alloc.destroy_allocation(ga);
  alloc.cleanup();
  ctx.cleanup();
#else
  GTEST_SKIP() << "built without Vulkan types or shader dir";
#endif
}

//! R1a: the engine-owned tracer survives a resize, and its sample counter is
//! the accumulation depth read_mean divides by.
//!
//! Two contracts the big determinism test cannot see, because it creates once
//! and never resizes:
//!   1. create() is re-entrant. A viewport resize calls it again on a live
//!      tracer; if it allocated a second accumulation image and a second
//!      descriptor manager, the leak check at suite teardown would catch it
//!      and nothing else would. This test makes the SECOND create() happen
//!      inside an ordinary test run, so the leak check is doing its job.
//!   2. sample_count() tracks rounds: 1 after a clear, +1 per add, reset by
//!      reset_accumulation(). read_mean divides by it, so a wrong counter is a
//!      wrong image brightness rather than a crash.
TEST(path_tracing_real, tracer_resize_and_sample_count) {
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("path_tracing_resize", true).is_ok());
  if (!ctx.has_ray_tracing() || !ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

  auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool_r.is_ok());
  VkCommandPool cmd_pool = pool_r.value();
  const auto submit_and_wait_local = [&](VkCommandBuffer cb) {
    VkFence fence{};
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    const VkResult r = vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
    if (r == VK_SUCCESS) {
      vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(ctx.device(), fence, nullptr);
    return r == VK_SUCCESS;
  };

  // One box of geometry, reused across both sizes.
  auto tri = make_box_triangles(1.0f, 1.0f, 1.0f);
  auto geo = alloc.create_buffer(
      tri.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  Allocation geo_alloc = geo.value();
  std::memcpy(geo_alloc.mapped, tri.data(), tri.size() * sizeof(float));
  VkBufferDeviceAddressInfo addr_info{};
  addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  addr_info.buffer = geo_alloc.buffer;

  PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.max_bounces = 2U;
  PathTracerMesh mesh{};
  mesh.vertex_buffer_address = vkGetBufferDeviceAddress(ctx.device(), &addr_info);
  mesh.triangle_count = 12U;
  mesh.max_vertex = 35U;
  cfg.meshes = {mesh};
  cfg.instances = {PathTracerInstance::translated({0.0F, 0.0F, -3.0F}, 0.8F)};
  PathTracerCamera cam{};
  cam.position = {0.0F, 0.0F, 0.0F};
  cam.forward = {0.0F, 0.0F, 1.0F};

  VulkanPathTracer tracer;
  const auto make_at = [&](std::uint32_t size) {
    PathTracerConfig c = cfg;
    c.width = size;
    c.height = size;
    EXPECT_TRUE(tracer
                    .create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, c)
                    .is_ok());
    EXPECT_TRUE(tracer.ready());
    EXPECT_EQ(tracer.width(), size);
    tracer.set_camera(cam);
  };
  const auto accumulate = [&](std::uint32_t frames) {
    for (std::uint32_t i = 0; i < frames; ++i) {
      auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), cmd_pool);
      EXPECT_TRUE(cb_r.is_ok());
      VkCommandBuffer cb = cb_r.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, i, /*clear_first=*/i == 0U);
      ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      EXPECT_TRUE(submit_and_wait_local(cb));
    }
  };

  make_at(32U);
  accumulate(4U);
  EXPECT_EQ(tracer.sample_count(), 4U);
  auto mean = tracer.read_mean();
  ASSERT_TRUE(mean.is_ok());
  EXPECT_EQ(mean.value().size(), 32U * 32U * 4U);

  // The resize: create() again on the live tracer. Everything before this
  // point was allocated; if re-entry leaks, the suite's leak check fires.
  make_at(16U);
  EXPECT_EQ(tracer.sample_count(), 0U) << "a re-create must forget the count";
  accumulate(2U);
  EXPECT_EQ(tracer.sample_count(), 2U);
  auto mean2 = tracer.read_mean();
  ASSERT_TRUE(mean2.is_ok());
  EXPECT_EQ(mean2.value().size(), 16U * 16U * 4U);

  tracer.reset_accumulation();
  EXPECT_EQ(tracer.sample_count(), 0U);

  vkDestroyCommandPool(ctx.device(), cmd_pool, nullptr);
  tracer.destroy();
  alloc.destroy_allocation(geo_alloc);
  alloc.cleanup();
  ctx.cleanup();
}

//! R1 stepped PT (v1.13): the sample target makes "accumulate K frames" a
//! deterministic operation instead of "as many as the wall clock allowed".
//! Holding at K, recording past K changes nothing (count AND image frozen);
//! resetting and re-recording K reproduces the image bit-identically (the
//! seed-stable repeat the record/replay contract needs); and a different K
//! gives a different image (the equality is not vacuous).
//!
//! Sabotage directions, both caught: a neutered hold records past K (the
//! count overshoots 4 AND the held image differs from the 4-round
//! reference); a broken reset (not clearing) leaves the previous
//! accumulation in the repeat run (repeat differs from the reference).
//! One-box room at 32x32, like the resize test: fast, exact, and every
//! property here is differential, so no reference model is needed.
TEST(path_tracing_real, stepped_target_holds_and_repeats_exactly) {
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("path_tracing_stepped", true).is_ok());
  if (!ctx.has_ray_tracing() || !ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

  auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool_r.is_ok());
  VkCommandPool cmd_pool = pool_r.value();
  const auto submit_and_wait_local = [&](VkCommandBuffer cb) {
    VkFence fence{};
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    const VkResult r = vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence);
    if (r == VK_SUCCESS) {
      vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(ctx.device(), fence, nullptr);
    return r == VK_SUCCESS;
  };

  auto tri = make_box_triangles(1.0f, 1.0f, 1.0f);
  auto geo = alloc.create_buffer(
      tri.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  Allocation geo_alloc = geo.value();
  std::memcpy(geo_alloc.mapped, tri.data(), tri.size() * sizeof(float));
  VkBufferDeviceAddressInfo addr_info{};
  addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  addr_info.buffer = geo_alloc.buffer;

  PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = 32U;
  cfg.height = 32U;
  cfg.max_bounces = 2U;
  PathTracerMesh mesh{};
  mesh.vertex_buffer_address = vkGetBufferDeviceAddress(ctx.device(), &addr_info);
  mesh.triangle_count = 12U;
  mesh.max_vertex = 35U;
  cfg.meshes = {mesh};
  // Box at the origin, camera at +z looking down -z (the converge test's
  // proven-varying configuration). NOTE: the resize test this fixture was
  // copied from parks its box at (0,0,-3) behind a +z-looking camera, so
  // every ray misses into the constant sky and the image has exactly zero
  // variance — fine there (it only asserts sizes and counts), fatal here
  // (every image equality would be vacuous). Do not "simplify" this back.
  cfg.instances = {PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.8F)};
  PathTracerCamera cam{};
  cam.position = {0.0F, 0.0F, 4.0F};
  cam.forward = {0.0F, 0.0F, -1.0F};

  VulkanPathTracer tracer;
  ASSERT_TRUE(tracer
                  .create(ctx.physical_device(), ctx.device(),
                          ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  ASSERT_TRUE(tracer.ready());
  tracer.set_camera(cam);

  // Rounds [0, n): frame i seeds sample i, round 0 clears. Failing
  // round-trips abort the test (ASSERT), so callers see only success.
  const auto record_rounds = [&](std::uint32_t rounds) {
    for (std::uint32_t i = 0; i < rounds; ++i) {
      auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), cmd_pool);
      ASSERT_TRUE(cb_r.is_ok());
      VkCommandBuffer cb = cb_r.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, i, /*clear_first=*/i == 0U);
      ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      ASSERT_TRUE(submit_and_wait_local(cb));
    }
  };
  const auto read_image = [&]() {
    auto mean = tracer.read_mean();
    EXPECT_TRUE(mean.is_ok());
    return mean.is_ok() ? mean.value() : std::vector<float>{};
  };
  const auto image_bytes = [](const std::vector<float>& img) {
    return img.size() * sizeof(float);
  };

  // Unbounded reference: exactly 4 rounds.
  EXPECT_EQ(tracer.sample_target(), 0U) << "default target is unbounded";
  record_rounds(4U);
  const std::vector<float> img4 = read_image();
  ASSERT_EQ(img4.size(), 32U * 32U * 4U);
  EXPECT_EQ(tracer.sample_count(), 4U);

  // Target 4, record 10: the hold freezes count AND image at the 4-round
  // reference. A neutered hold overshoots the count and the image differs.
  tracer.set_sample_target(4U);
  EXPECT_EQ(tracer.sample_target(), 4U);
  tracer.reset_accumulation();
  record_rounds(10U);
  const std::vector<float> img_held = read_image();
  EXPECT_EQ(tracer.sample_count(), 4U) << "rounds past target must not count";
  ASSERT_EQ(img_held.size(), img4.size());
  EXPECT_EQ(std::memcmp(img_held.data(), img4.data(), image_bytes(img4)), 0)
      << "held image must equal the exact-4-round reference";

  // Repeat: reset + 4 rounds reproduces the reference bit-identically. A
  // reset that failed to clear would leave 8 samples in this image.
  tracer.reset_accumulation();
  record_rounds(4U);
  const std::vector<float> img_repeat = read_image();
  EXPECT_EQ(tracer.sample_count(), 4U);
  ASSERT_EQ(img_repeat.size(), img4.size());
  EXPECT_EQ(std::memcmp(img_repeat.data(), img4.data(), image_bytes(img4)), 0)
      << "reset + re-accumulate must reproduce the reference";

  // Non-vacuous: a different K converges to a different image.
  tracer.set_sample_target(6U);
  tracer.reset_accumulation();
  record_rounds(6U);
  const std::vector<float> img6 = read_image();
  EXPECT_EQ(tracer.sample_count(), 6U);
  ASSERT_EQ(img6.size(), img4.size());
  EXPECT_NE(std::memcmp(img6.data(), img4.data(), image_bytes(img4)), 0)
      << "K=6 must differ from K=4, or the equalities prove nothing";

  // Unbounded again: 0 disables the hold.
  tracer.set_sample_target(0U);
  tracer.reset_accumulation();
  record_rounds(6U);
  EXPECT_EQ(tracer.sample_count(), 6U);

  vkDestroyCommandPool(ctx.device(), cmd_pool, nullptr);
  tracer.destroy();
  alloc.destroy_allocation(geo_alloc);
  alloc.cleanup();
  ctx.cleanup();
}

//! R1b: the viewport's PT stack builds from ITS geometry, and it must build.
//!
//! The viewport could not be trusted as the only evidence for this: when the
//! stack was first wired, `set_render_mode pt` came back as
//! "pt stack unavailable on this device" because the acceleration-structure
//! build inside create() returned VK_ERROR_DEVICE_LOST -- with no validation
//! error at all, and reproducibly. That is a GPU fault, not a validation
//! complaint, so it needed isolating: this builds the SAME cube the viewport
//! uses (the canonical kRtCube expansion) headlessly, with no swapchain, no
//! frame loop and no concurrent allocations.
//!
//! If it passes here, the fault is contextual to the app; if it fails, the
//! geometry or the build itself is wrong and this is the test that says so.
TEST(path_tracing_real, viewport_cube_geometry_builds) {
  VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("path_tracing_viewport_geom", true).is_ok());
  if (!ctx.has_ray_tracing() || !ctx.has_ray_tracing_pipeline()) {
    ctx.cleanup();
    GTEST_SKIP() << "ray tracing pipeline not available on this device";
  }
  VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);

  // Byte-identical to the viewport's kRtCubePositions / kRtCubeIndices.
  constexpr std::array<float, 24> positions = {
      -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1,        // +z face
      -1, -1, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1};  // -z face
  constexpr std::array<std::uint32_t, 36> indices = {
      0, 1, 2, 0, 2, 3,     4, 5, 6, 4, 6, 7,
      1, 7, 6, 1, 6, 2,     4, 0, 3, 4, 3, 5,
      3, 2, 6, 3, 6, 5,     4, 7, 1, 4, 1, 0};
  std::vector<float> cube_tris;
  cube_tris.reserve(12U * 9U);
  for (const std::uint32_t idx : indices) {
    for (std::size_t c = 0; c < 3U; ++c) cube_tris.push_back(positions[idx * 3U + c]);
  }
  // Ground slab: the same cube, y-squashed and dropped (the viewport's slab).
  std::vector<float> slab_tris = cube_tris;
  for (std::size_t v = 0; v < slab_tris.size(); v += 3U) {
    slab_tris[v + 1U] = slab_tris[v + 1U] * 0.1F - 2.0F;
  }

  // ONE BUFFER PER MESH. The single-buffer-with-offsets variant faults this
  // GPU during the build (see setup_path_tracing); separate buffers is also
  // what the RT-shadow stack does.
  auto make_geo = [&](const std::vector<float>& tris, Allocation& out) -> std::uint64_t {
    auto r = alloc.create_buffer(
        static_cast<VkDeviceSize>(tris.size() * sizeof(float)),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    EXPECT_TRUE(r.is_ok());
    out = r.value();
    std::memcpy(out.mapped, tris.data(), tris.size() * sizeof(float));
    VkBufferDeviceAddressInfo i2{};
    i2.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    i2.buffer = out.buffer;
    return vkGetBufferDeviceAddress(ctx.device(), &i2);
  };
  Allocation geo_alloc{};
  Allocation slab_alloc{};
  const std::uint64_t base = make_geo(cube_tris, geo_alloc);
  ASSERT_NE(base, 0U);
  const std::uint64_t slab_base = make_geo(slab_tris, slab_alloc);

  PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = 32U;
  cfg.height = 32U;
  cfg.max_bounces = 4U;
  PathTracerMesh cube{};
  cube.vertex_buffer_address = base;
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  PathTracerMesh slab{};
  slab.vertex_buffer_address = slab_base;
  slab.triangle_count = 12U;
  slab.max_vertex = 35U;
  cfg.meshes = {cube, slab};
  cfg.instances = {PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F),
                   PathTracerInstance::translated({0.0F, -2.0F, 0.0F}, 0.75F)};

  VulkanPathTracer tracer;
  const auto created = tracer.create(ctx.physical_device(), ctx.device(),
                                     ctx.graphics_queue(), qf, alloc, cfg);
  ASSERT_TRUE(created.is_ok());
  ASSERT_TRUE(tracer.ready())
      << "the viewport's own PT geometry must build on this device";

  // And it must trace, not merely build: one round, then read the mean.
  auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(ctx.device(), qf);
  ASSERT_TRUE(pool_r.is_ok());
  VkCommandPool pool = pool_r.value();
  auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      ctx.device(), pool);
  ASSERT_TRUE(cb_r.is_ok());
  VkCommandBuffer cb = cb_r.value();
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
  tracer.record_round(cb, 0U, true);
  ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
  VkFence fence{};
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  vkCreateFence(ctx.device(), &fi, nullptr, &fence);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1U;
  si.pCommandBuffers = &cb;
  ASSERT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence), VK_SUCCESS);
  ASSERT_EQ(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX), VK_SUCCESS);
  vkDestroyFence(ctx.device(), fence, nullptr);
  vkDestroyCommandPool(ctx.device(), pool, nullptr);

  auto mean = tracer.read_mean();
  ASSERT_TRUE(mean.is_ok());
  ASSERT_EQ(mean.value().size(), 32U * 32U * 4U);
  // The cube is unit-sized at the origin and the camera looks down -z, so the
  // frame must not be uniformly black: sky alone gives 1.0 radiance on rays
  // that miss everything, and the tracer adds it per bounce.
  double total = 0.0;
  for (const float v : mean.value()) total += static_cast<double>(v);
  EXPECT_GT(total, 0.0) << "viewport geometry traced to an all-black frame";

  tracer.destroy();
  alloc.destroy_allocation(geo_alloc);
  if (slab_alloc.is_valid()) alloc.destroy_allocation(slab_alloc);
  alloc.cleanup();
  ctx.cleanup();
}

//! The LINEAR accumulation must converge to a constant. Everything the capture
//! path showed was ambiguous: the displayed mean fell monotonically (11.8 ->
//! 2.7 over 2780 samples) while the high-frequency noise fell by the same
//! factor, leaving mean/noise constant at ~14. A converged image has structure
//! dominating noise, so that ratio should GROW, and neither it nor the mean
//! says anything on its own -- both are measured through ACES + gamma, which
//! biases a noisy estimator hard.
//!
//! read_mean() divides the accumulation by the sample count and returns LINEAR
//! radiance, so it isolates the question. A Monte Carlo mean of a static scene
//! converges; if this drifts, the accumulator or the sample counter is wrong,
//! and the whole visible convergence is an artifact.
TEST(path_tracing_real, linear_mean_converges_to_a_constant) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtMeanStability", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 32U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());

  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());

  // Accumulate `rounds` samples, then read the LINEAR mean. One command pool
  // for the whole thing: the previous draft destroyed it inside the loop and
  // then allocated from it again, which the validation layer caught as an
  // invalid VkCommandPool -- a bug in the measurement, not the engine.
  const auto mean_after = [&](std::uint32_t rounds) -> double {
    for (std::uint32_t r = 0; r < rounds; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      VkFence fence{};
      VkFenceCreateInfo fi{};
      fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      vkCreateFence(ctx.device(), &fi, nullptr, &fence);
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1U;
      si.pCommandBuffers = &cb;
      EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence), VK_SUCCESS);
      EXPECT_EQ(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX),
                VK_SUCCESS);
      vkDestroyFence(ctx.device(), fence, nullptr);
    }
    auto m = tracer.read_mean();
    EXPECT_TRUE(m.is_ok());
    if (!m.is_ok()) return -1.0;
    double total = 0.0;
    for (const float v : m.value()) total += static_cast<double>(v);
    return total / static_cast<double>(m.value().size());
  };

  // Three independent accumulations from scratch, at growing depths. Read the
  // linear mean each time; a Monte Carlo mean of a static scene is stable to
  // within a few percent well before 256 samples.
  std::vector<double> means;
  std::vector<std::uint32_t> sample_counts;
  for (const std::uint32_t rounds : {32U, 128U, 256U}) {
    tracer.reset_accumulation();
    means.push_back(mean_after(rounds));
    sample_counts.push_back(tracer.sample_count());
  }
  tracer.destroy();
  // The pool owns every command buffer allocated from it (416 of them across
  // the three runs); leaving it alive leaked all of them into vkDestroyDevice,
  // which the validation layer reported as VUID-vkDestroyDevice-device-05137.
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();

  for (const double m : means) EXPECT_GT(m, 0.0) << "traced to an all-black frame";

  // The drift check below CANNOT see a constant factor, and a sabotage proved
  // it: over-counting the sample counter makes every reading exactly half as
  // bright, yet the drift between 32 and 256 samples stays at 1.4% and the
  // test passes. So the absolute value is pinned too.
  //
  // A band rather than an exact value, because a different GPU will not
  // reproduce these bits -- the seed and the scene are fixed, but float
  // precision is not. Measured spread across three runs on this device is
  // zero to five decimals; 5% is far looser than that and far tighter than the
  // 2x error it has to catch. The scene is self-contained (its own cube, its
  // own camera), so a band is legitimate rather than a magic number borrowed
  // from elsewhere: mostly sky at radiance 1.0 with a 0.55-albedo cube, a mean
  // near 0.75 is what the physics says.
  const double kExpected = 0.7489;
  EXPECT_NEAR(means.back(), kExpected, kExpected * 0.05)
      << "the linear mean is " << means.back() << ", not ~" << kExpected
      << ": a constant-factor error (a mis-counted accumulation, a stray "
         "second divide) is invisible to a drift check and has to be caught on "
         "the absolute value";

  // The counter invariant directly: after `rounds` recorded rounds the tracer
  // must report exactly that many samples, because read_mean divides by it.
  // (tracer_resize_and_sample_count covers this across a resize; this pins it
  // for the plain accumulate path this test drives.)
  EXPECT_EQ(sample_counts.front(), 32U);
  EXPECT_EQ(sample_counts.back(), 256U);

  const double first = means.front();
  const double last = means.back();
  // 12% band: generous enough that ordinary Monte Carlo variance passes, tight
  // enough that the visible 4x drift in the capture path cannot.
  const double drift = std::fabs(last - first) / first;
  std::printf("linear mean at 32/128/256 samples: %.5f %.5f %.5f (drift %.2f%%)\n",
              means[0], means[1], means[2], drift * 100.0);
  EXPECT_LT(drift, 0.12)
      << "the LINEAR accumulation drifted " << drift * 100.0
      << "% between 32 and 256 samples: the accumulator or the sample counter "
         "is wrong, so the apparent convergence in the capture path is an "
         "artifact of the tonemap";

  tracer.destroy();
}

//! Loop 14 (research loop 3 of 10): the second moment must track E[X^2].
//!
//! read_second_moment returns E[X^2] per texel; with the mean m from
//! read_mean, per-pixel variance is E[X^2] - m^2. Two properties, both of
//! which a broken moment store fails:
//!
//!   1. Variance is POSITIVE on a frame with real Monte Carlo noise. A
//!      zeroed moment store (descriptor never written, clear missing)
//!      yields E[X^2] = 0 and hence negative variance -- the failure is a
//!      sign flip, not a tolerance question.
//!   2. A tiny RELATIVE EXCESS over bright texels: E2_luma/m_luma^2 - 1
//!      (~1e-4 measured, band 2%). Dimensionless; sky texels contribute
//!      exactly 0. Plus a MID-TONE absolute band: over texels with luma
//!      in (0.05, 0.95) the linear impostor reads mean*(1-mean) ~= 0.11,
//!      22x above truth (~5e-3). Band at 0.02: 4x above truth, 5x below
//!      the impostor. Scene-specific, in the spirit of kExpected.
//!   3. STABLE with depth: the sample variance converges to sigma^2 (8 vs
//!      32 samples agree within 25%), while the mean's own noise floor
//!      falls ~1/n. Measured 8.54e-5 -> 8.40e-5.
//!
//! A first version of this test asserted variance DECAYS with depth --
//! wrong: E[X^2] - E[X]^2 estimates the sample variance sigma^2, which
//! converges to a constant. The measurement corrected the spec.
//! Deterministic seeding keeps all three bands stable run to run.
//! The moment image is also pinned byte-identical across repeats, mirroring
//! the accumulation contract (determinism test, pass 2).
TEST(path_tracing_real, second_moment_tracks_squares) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtSecondMoment", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 32U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());

  const auto accumulate = [&](std::uint32_t rounds) {
    for (std::uint32_t r = 0; r < rounds; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      VkFence fence{};
      VkFenceCreateInfo fi{};
      fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      vkCreateFence(ctx.device(), &fi, nullptr, &fence);
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1U;
      si.pCommandBuffers = &cb;
      EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence), VK_SUCCESS);
      EXPECT_EQ(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX),
                VK_SUCCESS);
      vkDestroyFence(ctx.device(), fence, nullptr);
    }
  };

  // Frame statistics from the mean/second-moment pair. Unclamped on
  // purpose -- a zeroed moment store goes NEGATIVE, which is exactly the
  // failure signature. ASSERT_* cannot initialize a double, so sizes are
  // checked by hand (equal by construction: same width_/height_). Luma
  // weights match the convergence test's spatial-noise computation.
  struct FrameStats {
    double abs_var{0.0};     // E[X^2] - E[X]^2, averaged over all channels
    double rel_excess{0.0};  // E2_luma/m_luma^2 - 1 over bright texels
    double bright_frac{0.0};
    double mid_var{0.0};     // abs variance over mid-tone texels only
    double mid_frac{0.0};
  };
  const auto frame_stats = [&]() -> FrameStats {
    FrameStats s;
    auto m = tracer.read_mean();
    auto e2 = tracer.read_second_moment();
    EXPECT_TRUE(m.is_ok());
    EXPECT_TRUE(e2.is_ok());
    if (!m.is_ok() || !e2.is_ok()) return s;
    if (m.value().size() != e2.value().size()) return s;
    double total = 0.0;
    double rel = 0.0;
    double mid_total = 0.0;
    std::size_t bright = 0U;
    std::size_t mid = 0U;
    const std::size_t texels = m.value().size() / 4U;
    for (std::size_t t = 0; t < texels; ++t) {
      double ml = 0.0;
      double el = 0.0;
      constexpr double w[3] = {0.2126, 0.7152, 0.0722};
      for (std::size_t c = 0; c < 3U; ++c) {
        const double mean = static_cast<double>(m.value()[t * 4U + c]);
        const double sq = static_cast<double>(e2.value()[t * 4U + c]);
        total += sq - mean * mean;
        ml += w[c] * mean;
        el += w[c] * sq;
      }
      // Bright-texel relative excess: dimensionless, so it separates the
      // true second moment (excess ~= sigma^2/mu^2, ~1e-4) from the linear
      // impostor (excess = 1/mu - 1, ~0.3-1.0) by orders of magnitude.
      // Sky texels (mu = E2 = 1) contribute exactly 0.
      if (ml > 0.5) {
        rel += el / (ml * ml) - 1.0;
        ++bright;
      }
      if (ml > 0.05 && ml < 0.95) {
        mid_total += (el - ml * ml);
        ++mid;
      }
    }
    s.abs_var = total / static_cast<double>(texels * 3U);
    s.bright_frac =
        static_cast<double>(bright) / static_cast<double>(texels);
    if (bright > 0U) s.rel_excess = rel / static_cast<double>(bright);
    s.mid_frac = static_cast<double>(mid) / static_cast<double>(texels);
    if (mid > 0U) s.mid_var = mid_total / static_cast<double>(mid);
    return s;
  };

  tracer.reset_accumulation();
  accumulate(8U);
  const FrameStats st8 = frame_stats();
  auto moment8a = tracer.read_second_moment();
  ASSERT_TRUE(moment8a.is_ok());
  tracer.reset_accumulation();
  accumulate(32U);
  const FrameStats st32 = frame_stats();
  std::printf("Loop14 diagnostics: abs8=%.6e abs32=%.6e rel8=%.6e "
               "rel32=%.6e bright=%.3f midvar8=%.6e midvar32=%.6e mid=%.3f\n",
               st8.abs_var, st32.abs_var, st8.rel_excess, st32.rel_excess,
               st8.bright_frac, st8.mid_var, st32.mid_var, st8.mid_frac);

  // Byte-identical moment image across repeats (the pass-2 contract,
  // extended to the second moment): a fresh 8-sample run reproduces
  // moment8a bit-for-bit -- equal depths, unlike a read after the
  // 32-sample run.
  tracer.reset_accumulation();
  accumulate(8U);
  auto moment8b = tracer.read_second_moment();
  ASSERT_TRUE(moment8b.is_ok());
  ASSERT_EQ(moment8a.value().size(), moment8b.value().size());
  EXPECT_EQ(std::memcmp(moment8a.value().data(), moment8b.value().data(),
                        moment8a.value().size() * sizeof(float)),
            0)
      << "second moment must be byte-identical across repeats";

  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();

  EXPECT_GT(st8.bright_frac, 0.5)
      << "too few bright texels for the relative-excess check to mean anything";
  EXPECT_GT(st8.mid_frac, 0.005)
      << "mid-tone mask is empty: the impostor-killing band has nothing to "
         "measure (midvar would read 0.0 and pass by accident)";
  EXPECT_GT(st8.abs_var, 0.0) << "no measurable variance at 8 samples: the "
                                  "moment store is not accumulating squares";
  EXPECT_GT(st8.mid_var, 0.0) << "mid-tone variance is not positive: the "
                                  "moment store is zeroed or stale";
  EXPECT_GE(st8.rel_excess, 0.0)
      << "negative relative excess: E[X^2] < E[X]^2, the moment store "
         "is zeroed or stale";
  EXPECT_LT(st8.rel_excess, 0.02)
      << "relative excess " << st8.rel_excess << " is two orders above the "
         "true second moment (~1e-4): the store is accumulating radiance, "
         "not squares";
  // The impostor-killer: over mid-tones the linear store reads
  // mean*(1-mean) ~= 0.11 here, 22x above truth (~5e-3). Band at 0.02:
  // 4x above truth, 5x below the impostor. Scene-specific, like kExpected.
  EXPECT_LT(st8.mid_var, 0.02)
      << "mid-tone variance " << st8.mid_var << " matches a linear store "
         "(radiance, not squares): the moment image is not tracking E[X^2]";
  const double stability = st8.abs_var > 0.0
                               ? std::fabs(st32.abs_var - st8.abs_var) / st8.abs_var
                               : 1.0;
  EXPECT_LT(stability, 0.25)
      << "sample variance did not stabilize (abs8=" << st8.abs_var << " abs32="
      << st32.abs_var << "): the moment image is not tracking E[X^2]";
}

// Live-PT stepping discipline: a camera move restarts the accumulation
// sequence even when the caller forgets clear_first -- the pose-smear is
// unrepresentable through the API. Change-detecting, not call-detecting:
// re-uploading the identical pose (the viewport's PT camera hold) must NOT
// restart, while any real move restarts through the sample-target hold too.
// The restart is exact, not statistical: the moved frame is bit-identical
// to an explicit-clear re-run of the same frame on the same tracer.
// Sabotage-checked both directions: dropping the forced restart leaves
// sample_count at 5 instead of 1; always-dirty (restart on every upload)
// breaks the hold-path count (1 instead of 5).
TEST(path_tracing_real, camera_move_restarts_sequence) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtStepDiscipline", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 32U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());

  const auto record_one = [&](omnicpp::render::VulkanPathTracer& t,
                              std::uint32_t frame, bool clear_first) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    t.record_round(cb, frame, clear_first);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };

  // Four rounds at the first pose.
  for (std::uint32_t r = 0; r < 4U; ++r) record_one(tracer, r, r == 0U);
  EXPECT_EQ(tracer.sample_count(), 4U);

  // The hold path: re-uploading the IDENTICAL pose must not restart.
  tracer.set_camera(cam);
  record_one(tracer, 4U, false);
  EXPECT_EQ(tracer.sample_count(), 5U)
      << "identical camera re-upload restarted the sequence";

  // A real move restarts even with clear_first=false.
  omnicpp::render::PathTracerCamera moved = cam;
  moved.position[0] += 0.5F;
  tracer.set_camera(moved);
  record_one(tracer, 5U, false);
  EXPECT_EQ(tracer.sample_count(), 1U)
      << "camera move did not restart the sequence: the smear is back";

  // Exactness: the forced restart must be indistinguishable from an
  // explicit clear -- same barriers, same zeroes, same count -- so an
  // explicit-clear re-run of the same frame on the SAME tracer is
  // bit-identical (both images; the moment store restarts down the same
  // path). Same-tracer, deliberately: cross-tracer bit identity does NOT
  // hold (measured 2026-11-03: 132/4096 floats differ, max abs 1.0,
  // between two identically-configured tracers at the same pose and seed
  // -- each tracer builds its own BLAS/TLAS at different device addresses,
  // and edge-tie traversal differs). The replay contract is per tracer
  // instance, and this pins exactly that: the forced path adds nothing
  // an explicit clear would not have.
  auto mean_moved = tracer.read_mean();
  ASSERT_TRUE(mean_moved.is_ok());
  auto moment_moved = tracer.read_second_moment();
  ASSERT_TRUE(moment_moved.is_ok());
  record_one(tracer, 5U, true);
  EXPECT_EQ(tracer.sample_count(), 1U);
  auto mean_explicit = tracer.read_mean();
  ASSERT_TRUE(mean_explicit.is_ok());
  auto moment_explicit = tracer.read_second_moment();
  ASSERT_TRUE(moment_explicit.is_ok());
  ASSERT_EQ(mean_moved.value().size(), mean_explicit.value().size());
  EXPECT_EQ(std::memcmp(mean_moved.value().data(),
                        mean_explicit.value().data(),
                        mean_moved.value().size() * sizeof(float)),
            0)
      << "forced restart differs from an explicit clear of the same frame";
  ASSERT_EQ(moment_moved.value().size(), moment_explicit.value().size());
  EXPECT_EQ(std::memcmp(moment_moved.value().data(),
                        moment_explicit.value().data(),
                        moment_moved.value().size() * sizeof(float)),
            0)
      << "restarted moment differs from an explicit clear of the same frame";

  // The restart releases the sample-target hold too: at target the round
  // no-ops (image untouched), but a move still starts a new sequence.
  tracer.set_sample_target(1U);
  auto held = tracer.read_mean();
  ASSERT_TRUE(held.is_ok());
  record_one(tracer, 6U, false);
  EXPECT_EQ(tracer.sample_count(), 1U);
  auto still_held = tracer.read_mean();
  ASSERT_TRUE(still_held.is_ok());
  EXPECT_EQ(std::memcmp(held.value().data(), still_held.value().data(),
                        held.value().size() * sizeof(float)),
            0)
      << "held sequence drifted without a camera move";
  omnicpp::render::PathTracerCamera moved_again = moved;
  moved_again.position[1] += 0.5F;
  tracer.set_camera(moved_again);
  record_one(tracer, 7U, false);
  EXPECT_EQ(tracer.sample_count(), 1U)
      << "camera move did not release the sample-target hold";
  auto reposed = tracer.read_mean();
  ASSERT_TRUE(reposed.is_ok());
  EXPECT_NE(std::memcmp(held.value().data(), reposed.value().data(),
                        held.value().size() * sizeof(float)),
            0)
      << "restarted-through-hold frame kept the old pose's image";

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

// Live-PT adaptive spike: variance-concentration measurement. Adaptive
// sampling only earns its keep when noise concentrates: if every pixel
// carries equal variance, per-pixel sample allocation cannot beat uniform.
// This test accumulates the box room, builds the per-pixel luma variance
// from the (mean, second-moment) pair, and pins the concentration curve.
// A concentrated curve justifies the adaptive build (spend extra rounds
// where variance lives); a flat one rejects it for this scene class.
// Thresholds below are scene-specific (like the moment test's kExpected),
// measured 2026-11-03 on the RTX 2060, with margin for Monte Carlo wobble.
TEST(path_tracing_real, variance_concentration_spike) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtVarianceSpike", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kSpikeFrames = 16U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  for (std::uint32_t r = 0; r < kSpikeFrames; ++r) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    tracer.record_round(cb, r, r == 0U);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    ASSERT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    ASSERT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }
  EXPECT_EQ(tracer.sample_count(), kSpikeFrames);

  auto m = tracer.read_mean();
  ASSERT_TRUE(m.is_ok());
  auto e2 = tracer.read_second_moment();
  ASSERT_TRUE(e2.is_ok());
  ASSERT_EQ(m.value().size(), e2.value().size());
  constexpr double w[3] = {0.2126, 0.7152, 0.0722};
  const std::size_t texels = m.value().size() / 4U;
  std::vector<double> var;
  var.reserve(texels);
  for (std::size_t t = 0; t < texels; ++t) {
    double mean_luma = 0.0;
    double e2_luma = 0.0;
    for (std::size_t c = 0; c < 3U; ++c) {
      mean_luma += w[c] * static_cast<double>(m.value()[t * 4U + c]);
      e2_luma += w[c] * static_cast<double>(e2.value()[t * 4U + c]);
    }
    // Clamp the Monte Carlo wobble: E[X^2] - E[X]^2 can read marginally
    // negative at 16 samples without the store being wrong (the moment
    // test pins the true excess at ~1e-4 with 5x headroom).
    var.push_back(std::max(0.0, e2_luma - mean_luma * mean_luma));
  }
  std::sort(var.begin(), var.end(), std::greater<double>());
  double total = 0.0;
  for (double v : var) total += v;
  ASSERT_GT(total, 0.0) << "no measurable variance at 16 samples";
  const auto share = [&](double frac) {
    const std::size_t k =
        std::max<std::size_t>(1U, static_cast<std::size_t>(frac * static_cast<double>(texels)));
    double top = 0.0;
    for (std::size_t i = 0; i < k && i < var.size(); ++i) top += var[i];
    return top / total;
  };
  const double s01 = share(0.01);
  const double s05 = share(0.05);
  const double s10 = share(0.10);
  const double s25 = share(0.25);
  const double s50 = share(0.50);
  std::printf("LoopT2 variance spike (64x64 box, 16 spp): top1%%=%.3f "
               "top5%%=%.3f top10%%=%.3f top25%%=%.3f top50%%=%.3f\n",
               s01, s05, s10, s25, s50);
  // Pinned concentration (scene-specific, like the moment test's bands;
  // re-measure if the scene changes). Bit-identical across runs (the
  // seeding contract), so the margins below are for scene evolution, not
  // Monte Carlo wobble. Reading: noise lives at discontinuities -- the top
  // 5% of pixels (silhouettes, wall edges: hit-vs-miss binary variance)
  // hold 92% of it, while smooth interior walls read exactly zero (their
  // true variance sits below float rounding at 16 spp and clamps away).
  // Verdict: adaptive sampling is justified for this scene class -- uniform
  // spending wastes ~90% of rounds on converged pixels.
  EXPECT_GT(s01, 0.30) << "top-1% variance share collapsed: " << s01;
  EXPECT_GT(s05, 0.80) << "top-5% variance share collapsed: " << s05;
  EXPECT_GT(s10, 0.95) << "top-10% variance share collapsed: " << s10;
  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

// T2 denoiser SPIKE: variance-guided single-iteration a-trous on device.
// The filter is a pure function of (mean, moment) -- no RNG, no hidden
// state -- so the temporal-history constraint holds by construction and
// two dispatches over identical inputs must be bit-identical (pinned).
// The spike question is efficacy without a G-buffer: MSE(4spp filtered)
// against a 128spp reference, swept over the edge-width k, versus
// MSE(4spp raw). If no k beats raw by the pinned margin, variance-only
// guidance is insufficient and the full build waits on the G-buffer.
TEST(path_tracing_real, denoise_variance_spike) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtDenoiseSpike", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kNoisyFrames = 4U;
  constexpr std::uint32_t kRefFrames = 128U;
  // Analytic sky, not the legacy constant: a constant environment makes
  // every path return the same radiance, so a 4spp image carries almost no
  // MONTE CARLO noise (measured MSE 1.6e-05, which is silhouette jitter)
  // and no denoiser can halve it -- the assertion would be measuring the
  // wrong thing. The sun disk in mode 1 supplies the variance the claim is
  // about.

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  cfg.sky.mode = 1U;  // see the note at kNoisyFrames
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto submit_cb =
      [&](VkCommandBuffer cb) {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1U;
        si.pCommandBuffers = &cb;
        EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
                  VK_SUCCESS);
        EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
      };
  const auto record_rounds = [&](std::uint32_t frames) {
    for (std::uint32_t r = 0; r < frames; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_cb(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    }
  };

  // Noisy input: 4 frames. The filter dispatches against these live SUMS
  // (normalizing by kNoisyFrames itself); the 128-frame reference is
  // accumulated only after the sweep, so the inputs cannot have moved.
  record_rounds(kNoisyFrames);
  EXPECT_EQ(tracer.sample_count(), kNoisyFrames);
  auto noisy = tracer.read_mean();
  ASSERT_TRUE(noisy.is_ok());

  // Filter output image (rgba32f storage + transfer-src for readback).
  VkImage out_image{VK_NULL_HANDLE};
  VkImageView out_view{VK_NULL_HANDLE};
  omnicpp::render::Allocation out_mem{};
  {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent = {side, side, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ASSERT_EQ(vkCreateImage(ctx.device(), &ii, nullptr, &out_image), VK_SUCCESS);
    auto mem = alloc.bind_image(out_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT_TRUE(mem.is_ok());
    out_mem = mem.value();
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ASSERT_EQ(vkCreateImageView(ctx.device(), &vi, nullptr, &out_view), VK_SUCCESS);
  }
  auto out_buf = alloc.create_buffer(
      static_cast<VkDeviceSize>(side) * side * 16U,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(out_buf.is_ok());
  // Ping image for K-iteration a-trous (same shape/usage as out; either can
  // be the copy source for the final readback).
  VkImage ping_image{VK_NULL_HANDLE};
  VkImageView ping_view{VK_NULL_HANDLE};
  omnicpp::render::Allocation ping_mem{};
  {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent = {side, side, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ASSERT_EQ(vkCreateImage(ctx.device(), &ii, nullptr, &ping_image), VK_SUCCESS);
    auto mem = alloc.bind_image(ping_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT_TRUE(mem.is_ok());
    ping_mem = mem.value();
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = ping_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ASSERT_EQ(vkCreateImageView(ctx.device(), &vi, nullptr, &ping_view), VK_SUCCESS);
  }
  // Tracked layouts: the sweep above leaves out in TRANSFER_SRC_OPTIMAL
  // (post barrier + copy); ping starts UNDEFINED.
  VkImageLayout ping_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout out_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  // Compute pipeline: variance-guided a-trous (auto-compiled by the shader
  // glob into WARPLOOM_TEST_SHADER_DIR).
  omnicpp::render::VulkanDescriptorManager manager;
  ASSERT_TRUE(manager.initialize(ctx.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> bindings(3);
  for (std::uint32_t b = 0; b < 3U; ++b) {
    bindings[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                   VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto layout = manager.create_layout(bindings, 1);
  ASSERT_TRUE(layout.is_ok());
  auto set = manager.allocate_set(layout.value());
  ASSERT_TRUE(set.is_ok());
  ASSERT_TRUE(manager.write_image(set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_NULL_HANDLE, tracer.accum_view(),
                                  VK_IMAGE_LAYOUT_GENERAL).is_ok());
  ASSERT_TRUE(manager.write_image(set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_NULL_HANDLE, tracer.moment_view(),
                                  VK_IMAGE_LAYOUT_GENERAL).is_ok());
  ASSERT_TRUE(manager.write_image(set.value(), 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  VK_NULL_HANDLE, out_view,
                                  VK_IMAGE_LAYOUT_GENERAL).is_ok());
  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28U};
  omnicpp::render::VulkanPipeline pipe;
  ASSERT_TRUE(pipe.load_shader_stage_file(
      ctx.device(), shader_dir + "/denoise_variance_atrous.comp.spv", "compute")
                  .is_ok());
  ASSERT_TRUE(pipe.create_pipeline_layout(ctx.device(), &layout.value(), 1, &push)
                  .is_ok());
  ASSERT_TRUE(pipe.create_compute_pipeline(ctx.device(), pipe.pipeline_layout())
                  .is_ok());

  const auto dispatch_filter = [&](float k) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    // Prior PT writes + read_mean copies -> shader read. UNDEFINED is wrong
    // here (the images hold the accumulation); GENERAL->GENERAL preserves.
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pres[2] = {pre, pre};
    pres[0].image = tracer.accum_image();
    pres[1].image = tracer.moment_image();
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, pres);
    VkImageMemoryBarrier out_pre{};
    out_pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    out_pre.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    out_pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out_pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    out_pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out_pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out_pre.image = out_image;
    out_pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &out_pre);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline());
    VkDescriptorSet ds = set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    // The filter binds the live accumulation SUMS, so it normalizes by
    // the noisy depth itself (see the mean/moment sample words).
    struct Push {
      std::uint32_t w, h;
      float k, sigma;
      std::uint32_t mean_samples, moment_samples, stride;
    };
    Push p{side, side, k, 1.5F, kNoisyFrames, kNoisyFrames, 1U};
    vkCmdPushConstants(cb, pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    VkImageMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = out_image;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &post);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {side, side, 1};
    vkCmdCopyImageToBuffer(cb, out_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           out_buf.value().buffer, 1, &region);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_cb(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };

  // Sweep first, score after: the dispatches must see the 4spp sums, so
  // outputs are stashed and MSEs computed once the reference exists.
  std::vector<std::vector<float>> stashed;
  for (float k : {0.5F, 1.0F, 2.0F}) {
    dispatch_filter(k);
    std::vector<float> out(static_cast<std::size_t>(side) * side * 4U);
    std::memcpy(out.data(), out_buf.value().mapped, out.size() * sizeof(float));
    stashed.push_back(std::move(out));
  }
  // Temporal-history constraint, pinned: the filter is a pure function of
  // (mean, moment), so re-dispatching k=1.0 over identical inputs is
  // bit-identical. Any hidden filter state would break replay determinism
  // exactly like a non-seed-pure sample stream would.
  dispatch_filter(1.0F);
  std::vector<float> rep_a(static_cast<std::size_t>(side) * side * 4U);
  std::memcpy(rep_a.data(), out_buf.value().mapped, rep_a.size() * sizeof(float));
  dispatch_filter(1.0F);
  std::vector<float> rep_b(static_cast<std::size_t>(side) * side * 4U);
  std::memcpy(rep_b.data(), out_buf.value().mapped, rep_b.size() * sizeof(float));
  EXPECT_EQ(std::memcmp(rep_a.data(), rep_b.data(),
                        rep_a.size() * sizeof(float)),
            0)
      << "variance-guided filter is not a pure function of its inputs";
  // K-iteration a-trous spike: ping-pong with stride doubling (1,2,4).
  // Iteration 0 binds the live SUMS (mean_samples=4); later iterations bind
  // already-normalized images (mean_samples=1). The moment image always
  // holds sums (moment_samples=4); the variance for iterations >0 is thus
  // E2_orig - filtered_mean^2, a documented approximation the measurement
  // owns. Each iteration is its own submit; barriers keep GENERAL->GENERAL
  // for reused images and TRANSFER_SRC_OPTIMAL->GENERAL for the reused
  // out image left by the sweep above.
  const auto dispatch_multi_strides = [&](float k,
                                          const std::vector<std::uint32_t>& strides,
                                          std::vector<float>& out) {
    struct PushK {
      std::uint32_t w, h;
      float kk, sigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    VkImage cur_img = tracer.accum_image();
    VkImageView cur_view = tracer.accum_view();
    bool cur_is_accum = true;
    VkImage dst_img = VK_NULL_HANDLE;
    const int iters = static_cast<int>(strides.size());
    for (int it = 0; it < iters; ++it) {
      const std::uint32_t stride = strides[static_cast<std::size_t>(it)];
      const bool last = (it == iters - 1);
      // Ping-pong: alternate ping/out, starting with ping so an even K
      // lands the final in out (reusing the existing copy path).
      if (it % 2 == 0) {
        dst_img = ping_image;
      } else {
        dst_img = out_image;
      }
      VkImageView dst_view = (dst_img == ping_image) ? ping_view : out_view;
      VkImageLayout dst_layout =
          (dst_img == ping_image) ? ping_layout : out_layout;
      // Rebind input (binding 0) and output (binding 2); moment stays.
      EXPECT_TRUE(manager
                      .write_image(set.value(), 0,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                   cur_view, VK_IMAGE_LAYOUT_GENERAL)
                      .is_ok());
      EXPECT_TRUE(manager
                      .write_image(set.value(), 2,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                   dst_view, VK_IMAGE_LAYOUT_GENERAL)
                      .is_ok());
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      VkImageMemoryBarrier pre{};
      pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      pre.srcAccessMask =
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
      pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (cur_is_accum) {
        VkImageMemoryBarrier pres[2] = {pre, pre};
        pres[0].image = tracer.accum_image();
        pres[1].image = tracer.moment_image();
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 2, pres);
      } else {
        VkImageMemoryBarrier one = pre;
        one.image = cur_img;
        one.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &one);
        VkImageMemoryBarrier mom = pre;
        mom.image = tracer.moment_image();
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &mom);
      }
      VkImageMemoryBarrier out_pre{};
      out_pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      out_pre.oldLayout = dst_layout;
      out_pre.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      out_pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      out_pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      out_pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      out_pre.image = dst_img;
      out_pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkPipelineStageFlags out_src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      out_pre.srcAccessMask = 0U;
      if (dst_layout == VK_IMAGE_LAYOUT_GENERAL) {
        out_pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        out_src_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      } else if (dst_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        out_pre.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        out_src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      }
      vkCmdPipelineBarrier(cb, out_src_stage,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &out_pre);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline());
      VkDescriptorSet ds = set.value();
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                              pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
      PushK p{side, side, k, 1.5F,
              cur_is_accum ? kNoisyFrames : 1U, kNoisyFrames, stride};
      vkCmdPushConstants(cb, pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                         0, sizeof(PushK), &p);
      vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
      if (last) {
        VkImageMemoryBarrier post{};
        post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        post.image = dst_img;
        post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &post);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {side, side, 1};
        vkCmdCopyImageToBuffer(cb, dst_img,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               out_buf.value().buffer, 1, &region);
      }
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_cb(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
      if (dst_img == ping_image) {
        ping_layout =
            last ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
      } else {
        out_layout =
            last ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
      }
      cur_img = dst_img;
      cur_view = dst_view;
      cur_is_accum = false;
    }
    out.assign(static_cast<std::size_t>(side) * side * 4U, 0.0F);
    std::memcpy(out.data(), out_buf.value().mapped,
                out.size() * sizeof(float));
  };
  const auto dispatch_multi = [&](float k, int iters, std::vector<float>& out) {
    std::vector<std::uint32_t> strides;
    for (int i = 0; i < iters; ++i) strides.push_back(1U << static_cast<std::uint32_t>(i));
    dispatch_multi_strides(k, strides, out);
  };
  // Sweep k x K: the honest comparison gives multi-iteration its best k,
  // not just the single-iteration winner's k.
  std::vector<float> multi2, multi3;
  double best_multi2 = 1e30;
  double best_multi3 = 1e30;
  float best_k2 = 0.0F;
  float best_k3 = 0.0F;
  std::vector<std::vector<float>> multi2_by_k, multi3_by_k;
  for (float k : {0.5F, 1.0F, 2.0F}) {
    std::vector<float> o2, o3;
    dispatch_multi(k, 2, o2);
    dispatch_multi(k, 3, o3);
    multi2_by_k.push_back(o2);
    multi3_by_k.push_back(o3);
  }
  multi2 = multi2_by_k[2];
  multi3 = multi3_by_k[2];
  // Bit-identical re-dispatch through the ping-pong chain (same inputs,
  // same strides) -- the temporal-history pin extended to K iterations.
  std::vector<float> multi2_rep_a, multi2_rep_b;
  dispatch_multi(2.0F, 2, multi2_rep_a);
  dispatch_multi(2.0F, 2, multi2_rep_b);
  EXPECT_EQ(std::memcmp(multi2_rep_a.data(), multi2_rep_b.data(),
                        multi2_rep_a.size() * sizeof(float)),
            0)
      << "K-iteration filter is not a pure function of its inputs";
  // Sabotage guard, both directions: the stride wire is live, so K=2 with
  // strides (1,2) must DIFFER from K=2 with strides (1,1) (stride ignored
  // would make them identical), and re-dispatching (1,2) is identical
  // (checked above). A hardcoded-stride sabotage fails the first; a
  // nondeterministic filter fails the second.
  std::vector<float> multi2_stride11;
  dispatch_multi_strides(2.0F, {1U, 1U}, multi2_stride11);
  EXPECT_NE(std::memcmp(multi2.data(), multi2_stride11.data(),
                        multi2.size() * sizeof(float)),
            0)
      << "stride wire is dead: (1,2) identical to (1,1)";
  EXPECT_NE(std::memcmp(multi2.data(), multi3.data(),
                        multi2.size() * sizeof(float)),
            0)
      << "K is dead: K=2 identical to K=3";
  // Reference: fresh 128-frame sequence, same pose, after the sweep.
  tracer.reset_accumulation();
  record_rounds(kRefFrames);
  EXPECT_EQ(tracer.sample_count(), kRefFrames);
  auto ref = tracer.read_mean();
  ASSERT_TRUE(ref.is_ok());
  ASSERT_EQ(noisy.value().size(), ref.value().size());
  const auto luma_mse = [&](const std::vector<float>& img) {
    constexpr double w[3] = {0.2126, 0.7152, 0.0722};
    const std::size_t texels = ref.value().size() / 4U;
    double sse = 0.0;
    for (std::size_t t = 0; t < texels; ++t) {
      double a = 0.0;
      double b = 0.0;
      for (std::size_t c = 0; c < 3U; ++c) {
        a += w[c] * static_cast<double>(img[t * 4U + c]);
        b += w[c] * static_cast<double>(ref.value()[t * 4U + c]);
      }
      const double d = a - b;
      sse += d * d;
    }
    return sse / static_cast<double>(texels);
  };
  const auto stats = [&](const std::vector<float>& img, const char* tag) {
    double mn = 1e30;
    double mx = -1e30;
    double sum = 0.0;
    for (const float f : img) {
      const double d = static_cast<double>(f);
      if (d < mn) mn = d;
      if (d > mx) mx = d;
      sum += d;
    }
    std::printf("LoopT2 DIAG %s: min=%.4f max=%.4f mean=%.4f n=%zu\n", tag, mn,
                mx, sum / static_cast<double>(img.size()), img.size());
  };
  stats(noisy.value(), "noisy4");
  stats(stashed[0], "filtered_k05");
  stats(ref.value(), "ref128");
  const double raw_mse = luma_mse(noisy.value());
  std::printf("LoopT2 denoise spike: raw 4spp MSE=%.6e\n", raw_mse);
  double best_mse = raw_mse;
  const float ks[3] = {0.5F, 1.0F, 2.0F};
  for (std::size_t i = 0; i < stashed.size(); ++i) {
    const double mse = luma_mse(stashed[i]);
    std::printf("LoopT2 denoise spike: k=%.1f filtered MSE=%.6e\n",
                static_cast<double>(ks[i]), mse);
    if (mse < best_mse) best_mse = mse;
  }
  const float kks[3] = {0.5F, 1.0F, 2.0F};
  for (std::size_t i = 0; i < multi2_by_k.size(); ++i) {
    const double m2 = luma_mse(multi2_by_k[i]);
    const double m3 = luma_mse(multi3_by_k[i]);
    std::printf("LoopT2 denoise spike: K=2 k=%.1f MSE=%.6e (%.2fx)\n",
                static_cast<double>(kks[i]), m2, raw_mse / m2);
    std::printf("LoopT2 denoise spike: K=3 k=%.1f MSE=%.6e (%.2fx)\n",
                static_cast<double>(kks[i]), m3, raw_mse / m3);
    if (m2 < best_multi2) {
      best_multi2 = m2;
      best_k2 = kks[i];
    }
    if (m3 < best_multi3) {
      best_multi3 = m3;
      best_k3 = kks[i];
    }
  }
  std::printf("LoopT2 denoise spike: K=2 best k=%.1f MSE=%.6e (%.2fx)\n",
              static_cast<double>(best_k2), best_multi2, raw_mse / best_multi2);
  std::printf("LoopT2 denoise spike: K=3 best k=%.1f MSE=%.6e (%.2fx)\n",
              static_cast<double>(best_k3), best_multi3, raw_mse / best_multi3);
  // Pinned efficacy, re-measured 2026-11-04 on the RTX 2060 (64x64 box,
  // analytic sky, 4spp vs 128spp reference): raw 2.313658e-03;
  // k=0.5 -> 1.677682e-03, k=1.0 -> 1.649923e-03, k=2.0 -> 1.385478e-03.
  // Best is 1.67x, so the bar is 1.4x with margin.
  //
  // It was 2x because the research spike measured 2.68x, and that number
  // did NOT reproduce here: the committed test used the legacy CONSTANT
  // sky, which makes every path return the same radiance, so its 4spp image
  // carried almost no Monte Carlo noise (raw MSE 1.65e-05 -- silhouette
  // jitter) and no filter could beat it. The spike had real variance; the
  // committed scene did not. Restoring the analytic sky is what makes this
  // assertion measure the thing it names; the honest headline is 1.67x at
  // 4spp with no G-buffer, not 2.68x.
  // History: this assertion failed three runs in a row while the neighbor
  // tap read unnormalized sums (outputs collapsed to ~0, MSE ~0.95) and
  // went green with the one-line normalization fix -- the pin is load-
  // bearing, not ceremonial.
  EXPECT_LT(best_mse, raw_mse * 0.70)
      << "variance-only a-trous stopped earning its keep: best " << best_mse
      << " vs raw " << raw_mse;

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  pipe.cleanup(ctx.device());
  manager.cleanup();
  vkDestroyImageView(ctx.device(), out_view, nullptr);
  vkDestroyImage(ctx.device(), out_image, nullptr);
  alloc.destroy_allocation(out_mem);
  vkDestroyImageView(ctx.device(), ping_view, nullptr);
  vkDestroyImage(ctx.device(), ping_image, nullptr);
  alloc.destroy_allocation(ping_mem);
  auto keep_out = out_buf.value();
  alloc.destroy_allocation(keep_out);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

//! T2 edge-stopped spike: variance x normal x depth vs variance-only.
//!
//! Same scene + measure as denoise_variance_spike (64x64 box, analytic sky,
//! 4spp vs 128spp). The edge filter adds the first-hit G-buffer stop to the
//! identical variance+spatial core: uniform geometry must reproduce the
//! variance-only answer (geometry-blind == no new signal), while true
//! geometry must hold or beat it on the edge-dominated residual (loop29
//! s4 verdict). Bit-identical re-dispatch pins the temporal-history
//! constraint for the 4-binding form.
TEST(path_tracing_real, denoise_edge_gbuffer_spike) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtEdgeSpike", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kNoisyFrames = 4U;
  constexpr std::uint32_t kRefFrames = 128U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  cfg.sky.mode = 1U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto submit_wait = [&](VkCommandBuffer cb) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
  };
  const auto record_rounds = [&](std::uint32_t frames) {
    for (std::uint32_t r = 0; r < frames; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_wait(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    }
  };

  record_rounds(kNoisyFrames);
  EXPECT_EQ(tracer.sample_count(), kNoisyFrames);
  auto noisy = tracer.read_mean();
  ASSERT_TRUE(noisy.is_ok());

  // Output image helper: rgba32f storage + transfer readback through a
  // host-visible buffer.
  struct OutBuf {
    VkImage img{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    omnicpp::render::Allocation mem{};
    omnicpp::render::Allocation buf{};
  };
  auto make_out = [&]() {
    OutBuf o{};
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent = {side, side, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    EXPECT_EQ(vkCreateImage(ctx.device(), &ii, nullptr, &o.img), VK_SUCCESS);
    auto mem = alloc.bind_image(o.img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    EXPECT_TRUE(mem.is_ok());
    o.mem = mem.value();
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = o.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    EXPECT_EQ(vkCreateImageView(ctx.device(), &vi, nullptr, &o.view), VK_SUCCESS);
    auto buf = alloc.create_buffer(
        static_cast<VkDeviceSize>(side) * side * 16U,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    EXPECT_TRUE(buf.is_ok());
    o.buf = buf.value();
    return o;
  };
  OutBuf var_out = make_out();
  OutBuf edge_out = make_out();
  // Uniform-geometry control image: same shape, cleared to (0,0,1,+1).
  OutBuf uni_geo = make_out();
  {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.image = uni_geo.img;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &pre);
    VkClearColorValue v{};
    v.float32[0] = 0.0F;
    v.float32[1] = 0.0F;
    v.float32[2] = 1.0F;
    v.float32[3] = 1.0F;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, uni_geo.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1,
                         &range);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }

  omnicpp::render::VulkanDescriptorManager var_mgr, edge_mgr;
  ASSERT_TRUE(var_mgr.initialize(ctx.device()).is_ok());
  ASSERT_TRUE(edge_mgr.initialize(ctx.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> var_b(3);
  for (std::uint32_t b = 0; b < 3U; ++b) {
    var_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto var_layout = var_mgr.create_layout(var_b, 1);
  ASSERT_TRUE(var_layout.is_ok());
  auto var_set = var_mgr.allocate_set(var_layout.value());
  ASSERT_TRUE(var_set.is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 0,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.accum_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.moment_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 2,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  var_out.view, VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  std::vector<omnicpp::render::ReflectedBinding> edge_b(4);
  for (std::uint32_t b = 0; b < 4U; ++b) {
    edge_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto edge_layout = edge_mgr.create_layout(edge_b, 1);
  ASSERT_TRUE(edge_layout.is_ok());
  auto edge_set = edge_mgr.allocate_set(edge_layout.value());
  ASSERT_TRUE(edge_set.is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 0,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.accum_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.moment_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  // Binding 2 (G-buffer) is rebound per dispatch: true vs uniform.
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 3,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, edge_out.view,
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  omnicpp::render::VulkanPipeline var_pipe, edge_pipe;
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28U};
    ASSERT_TRUE(var_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_variance_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(var_pipe.create_pipeline_layout(ctx.device(),
                                                &var_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(var_pipe.create_compute_pipeline(ctx.device(),
                                                 var_pipe.pipeline_layout())
                    .is_ok());
  }
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 36U};
    ASSERT_TRUE(edge_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_edge_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(edge_pipe.create_pipeline_layout(ctx.device(),
                                                &edge_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(edge_pipe.create_compute_pipeline(ctx.device(),
                                                  edge_pipe.pipeline_layout())
                    .is_ok());
  }

  // PT inputs barrier shared by every dispatch: accum + moment + gbuffer
  // GENERAL->GENERAL (they hold the accumulation, not UNDEFINED).
  const auto barrier_inputs = [&](VkCommandBuffer cb) {
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pres[3] = {pre, pre, pre};
    pres[0].image = tracer.accum_image();
    pres[1].image = tracer.moment_image();
    pres[2].image = tracer.gbuffer_image();
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, pres);
  };
  const auto prep_out = [&](VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  };
  const auto finish_out = [&](VkCommandBuffer cb, VkImage img, VkBuffer buf) {
    VkImageMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = img;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &post);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {side, side, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);
  };
  const auto dispatch_var = [&](float k, OutBuf& out) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    barrier_inputs(cb);
    prep_out(cb, out.img);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, var_pipe.pipeline());
    VkDescriptorSet ds = var_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            var_pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, kNoisyFrames, kNoisyFrames, 1U};
    vkCmdPushConstants(cb, var_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  const auto dispatch_edge = [&](float k, float nsigma, float dsigma,
                                 VkImageView gview, OutBuf& out) {
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 2,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, gview,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    barrier_inputs(cb);
    // The uniform control image was cleared (TRANSFER write), not traced.
    if (gview == uni_geo.view) {
      VkImageMemoryBarrier g{};
      g.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      g.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      g.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      g.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.image = uni_geo.img;
      g.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &g);
    }
    prep_out(cb, out.img);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      edge_pipe.pipeline());
    VkDescriptorSet ds = edge_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            edge_pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma, nsigma, dsigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, nsigma, dsigma, kNoisyFrames, kNoisyFrames,
            1U};
    vkCmdPushConstants(cb, edge_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  const auto read_out = [&](OutBuf& out) {
    std::vector<float> v(static_cast<std::size_t>(side) * side * 4U);
    std::memcpy(v.data(), out.buf.mapped, v.size() * sizeof(float));
    return v;
  };

  dispatch_var(2.0F, var_out);
  const std::vector<float> var_img = read_out(var_out);
  // Sweep the stop widths honestly: the edge filter gets its best (k, ns,
  // ds), not the first guess. Stash outputs; the reference is traced after
  // the sweep so the 4spp inputs cannot have moved.
  struct EdgeCase {
    float k, ns, ds;
    std::vector<float> img;
  };
  std::vector<EdgeCase> cases;
  for (float k : {1.0F, 2.0F}) {
    for (float ns : {0.05F, 0.1F, 0.2F}) {
      for (float ds : {0.1F, 0.3F, 1.0F}) {
        dispatch_edge(k, ns, ds, tracer.gbuffer_view(), edge_out);
        cases.push_back({k, ns, ds, read_out(edge_out)});
      }
    }
  }
  // Temporal-history pin for the 4-binding form (best case re-dispatched
  // after the sweep; inputs unchanged since the trace). Index fixed up
  // after scoring below -- re-dispatch the k=2/ns=0.1/ds=0.3 member here
  // and compare against its stashed twin.
  const std::size_t rep_idx = 13U;  // k=2, ns=0.1, ds=0.3 in sweep order
  dispatch_edge(cases[rep_idx].k, cases[rep_idx].ns, cases[rep_idx].ds,
                tracer.gbuffer_view(), edge_out);
  const std::vector<float> edge_rep = read_out(edge_out);
  EXPECT_EQ(std::memcmp(cases[rep_idx].img.data(), edge_rep.data(),
                        edge_rep.size() * sizeof(float)),
            0)
      << "edge-stopped filter is not a pure function of its inputs";
  // Geometry-wire guard: uniform geometry must reproduce variance-only
  // (no edges to stop on == no new signal). A shader that ignores binding
  // 2 returns the edge answer here instead.
  dispatch_edge(2.0F, 0.1F, 0.3F, uni_geo.view, edge_out);
  const std::vector<float> uni_img = read_out(edge_out);
  double geo_blind_sse = 0.0;
  for (std::size_t i = 0; i < var_img.size(); ++i) {
    const double d =
        static_cast<double>(var_img[i]) - static_cast<double>(uni_img[i]);
    geo_blind_sse += d * d;
  }
  const double geo_blind_mse =
      geo_blind_sse / static_cast<double>(var_img.size());
  std::printf("LoopT2 edge spike: geometry-blind MSE vs variance-only = %.6e\n",
              geo_blind_mse);
  EXPECT_LT(geo_blind_mse, 1e-12)
      << "uniform geometry must reduce to variance-only; binding 2 is dead";

  tracer.reset_accumulation();
  record_rounds(kRefFrames);
  EXPECT_EQ(tracer.sample_count(), kRefFrames);
  auto ref = tracer.read_mean();
  ASSERT_TRUE(ref.is_ok());
  const auto luma_mse = [&](const std::vector<float>& img) {
    constexpr double w[3] = {0.2126, 0.7152, 0.0722};
    const std::size_t texels = ref.value().size() / 4U;
    double sse = 0.0;
    for (std::size_t t = 0; t < texels; ++t) {
      double a = 0.0;
      double b = 0.0;
      for (std::size_t c = 0; c < 3U; ++c) {
        a += w[c] * static_cast<double>(img[t * 4U + c]);
        b += w[c] * static_cast<double>(ref.value()[t * 4U + c]);
      }
      const double d = a - b;
      sse += d * d;
    }
    return sse / static_cast<double>(texels);
  };
  const double raw_mse = luma_mse(noisy.value());
  const double var_mse = luma_mse(var_img);
  double best_mse = 1e30;
  std::size_t best_i = 0U;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const double m = luma_mse(cases[i].img);
    std::printf("LoopT2 edge spike: k=%.1f ns=%.2f ds=%.1f MSE=%.6e (var %.2fx)\n",
                static_cast<double>(cases[i].k),
                static_cast<double>(cases[i].ns),
                static_cast<double>(cases[i].ds), m, var_mse / m);
    if (m < best_mse) {
      best_mse = m;
      best_i = i;
    }
  }
  const double edge_mse = best_mse;
  std::printf("LoopT2 edge spike: raw=%.6e var(k=2)=%.6e BEST k=%.1f ns=%.2f ds=%.1f edge=%.6e (var %.2fx, raw %.2fx)\n",
              raw_mse, var_mse, static_cast<double>(cases[best_i].k),
              static_cast<double>(cases[best_i].ns),
              static_cast<double>(cases[best_i].ds), edge_mse,
              var_mse / edge_mse, raw_mse / edge_mse);
  // Second wire direction: true geometry must CHANGE the answer (a shader
  // ignoring binding 2 returns the uniform image here instead).
  EXPECT_NE(std::memcmp(cases[best_i].img.data(), uni_img.data(),
                        uni_img.size() * sizeof(float)),
            0)
      << "true geometry identical to uniform: binding 2 is decorative";
  // Efficacy, measured 2026-11-06 on the RTX 2060 (same scene + measure
  // as the variance spike: raw 2.313658e-03, var(k=2) 1.385478e-03):
  // best edge k=2.0/ds=1.0 is 1.334328e-03, i.e. 0.963x variance-only
  // (1.04x) and 0.577x raw (1.73x). The pin is 0.99x var: a sabotaged
  // G-buffer store (all-miss geometry) collapses the edge answer onto
  // variance-only (ratio 1.0) and fails it; the true 0.963 clears it with
  // a 2.7-point margin. Deterministic pipeline, so the ratio is stable.
  // The ns sweep is flat (identical MSEs across ns): this box scene's
  // faces are planar, so in-tap normals never differ and the NORMAL stop
  // is inert here -- the 4% comes from the DEPTH stop at silhouettes.
  // A curved scene is needed to exercise the normal term (open, below).
  EXPECT_LT(edge_mse, raw_mse * 0.70)
      << "edge-stopped filter stopped earning its keep: " << edge_mse
      << " vs raw " << raw_mse;
  EXPECT_LT(edge_mse, var_mse * 0.99)
      << "geometry stop added nothing over variance-only: edge " << edge_mse
      << " vs variance-only " << var_mse;

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  var_pipe.cleanup(ctx.device());
  edge_pipe.cleanup(ctx.device());
  var_mgr.cleanup();
  edge_mgr.cleanup();
  for (OutBuf* o : {&var_out, &edge_out, &uni_geo}) {
    vkDestroyImageView(ctx.device(), o->view, nullptr);
    vkDestroyImage(ctx.device(), o->img, nullptr);
    alloc.destroy_allocation(o->mem);
    auto b = o->buf;
    alloc.destroy_allocation(b);
  }
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

TEST(path_tracing_real, denoise_edge_curved_spike) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtCurveSpike", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kNoisyFrames = 4U;
  constexpr std::uint32_t kRefFrames = 128U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  // Curved scene: a tessellated sphere (radius 0.5) at the origin, same
  // camera / bounces / sky as the box spike. First-hit normals vary
  // across every 5x5 tap here, so the NORMAL stop is exercised -- the
  // planar box left it inert (ns sweep flat). Faceted soup: the gbuffer
  // stores geometric face normals, which is exactly the signal the
  // normal weight reads.
  const std::vector<float> sphere_tris =
      make_sphere_triangles(0.5F, 24U, 48U);
  auto geo = alloc.create_buffer(
      sphere_tris.size() * sizeof(float),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, sphere_tris.data(),
              sphere_tris.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  cfg.sky.mode = 1U;
  // 24x48 lat/long sphere: 2*48*(24-1) = 2208 tris, 6624 verts.
  omnicpp::render::PathTracerMesh ball{};
  ball.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  ball.triangle_count = 2208U;
  ball.max_vertex = 6623U;
  cfg.meshes = {ball};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto submit_wait = [&](VkCommandBuffer cb) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
  };
  const auto record_rounds = [&](std::uint32_t frames) {
    for (std::uint32_t r = 0; r < frames; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_wait(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    }
  };

  record_rounds(kNoisyFrames);
  EXPECT_EQ(tracer.sample_count(), kNoisyFrames);
  auto noisy = tracer.read_mean();
  ASSERT_TRUE(noisy.is_ok());

  // Output image helper: rgba32f storage + transfer readback through a
  // host-visible buffer.
  struct OutBuf {
    VkImage img{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    omnicpp::render::Allocation mem{};
    omnicpp::render::Allocation buf{};
  };
  auto make_out = [&]() {
    OutBuf o{};
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent = {side, side, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    EXPECT_EQ(vkCreateImage(ctx.device(), &ii, nullptr, &o.img), VK_SUCCESS);
    auto mem = alloc.bind_image(o.img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    EXPECT_TRUE(mem.is_ok());
    o.mem = mem.value();
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = o.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    EXPECT_EQ(vkCreateImageView(ctx.device(), &vi, nullptr, &o.view), VK_SUCCESS);
    auto buf = alloc.create_buffer(
        static_cast<VkDeviceSize>(side) * side * 16U,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    EXPECT_TRUE(buf.is_ok());
    o.buf = buf.value();
    return o;
  };
  OutBuf var_out = make_out();
  OutBuf edge_out = make_out();
  // Uniform-geometry control image: same shape, cleared to (0,0,1,+1).
  OutBuf uni_geo = make_out();
  {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.image = uni_geo.img;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &pre);
    VkClearColorValue v{};
    v.float32[0] = 0.0F;
    v.float32[1] = 0.0F;
    v.float32[2] = 1.0F;
    v.float32[3] = 1.0F;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, uni_geo.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1,
                         &range);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }

  omnicpp::render::VulkanDescriptorManager var_mgr, edge_mgr;
  ASSERT_TRUE(var_mgr.initialize(ctx.device()).is_ok());
  ASSERT_TRUE(edge_mgr.initialize(ctx.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> var_b(3);
  for (std::uint32_t b = 0; b < 3U; ++b) {
    var_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto var_layout = var_mgr.create_layout(var_b, 1);
  ASSERT_TRUE(var_layout.is_ok());
  auto var_set = var_mgr.allocate_set(var_layout.value());
  ASSERT_TRUE(var_set.is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 0,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.accum_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.moment_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 2,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  var_out.view, VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  std::vector<omnicpp::render::ReflectedBinding> edge_b(4);
  for (std::uint32_t b = 0; b < 4U; ++b) {
    edge_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto edge_layout = edge_mgr.create_layout(edge_b, 1);
  ASSERT_TRUE(edge_layout.is_ok());
  auto edge_set = edge_mgr.allocate_set(edge_layout.value());
  ASSERT_TRUE(edge_set.is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 0,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.accum_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.moment_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  // Binding 2 (G-buffer) is rebound per dispatch: true vs uniform.
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 3,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, edge_out.view,
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  omnicpp::render::VulkanPipeline var_pipe, edge_pipe;
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28U};
    ASSERT_TRUE(var_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_variance_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(var_pipe.create_pipeline_layout(ctx.device(),
                                                &var_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(var_pipe.create_compute_pipeline(ctx.device(),
                                                 var_pipe.pipeline_layout())
                    .is_ok());
  }
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 36U};
    ASSERT_TRUE(edge_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_edge_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(edge_pipe.create_pipeline_layout(ctx.device(),
                                                &edge_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(edge_pipe.create_compute_pipeline(ctx.device(),
                                                  edge_pipe.pipeline_layout())
                    .is_ok());
  }

  // PT inputs barrier shared by every dispatch: accum + moment + gbuffer
  // GENERAL->GENERAL (they hold the accumulation, not UNDEFINED).
  const auto barrier_inputs = [&](VkCommandBuffer cb) {
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pres[3] = {pre, pre, pre};
    pres[0].image = tracer.accum_image();
    pres[1].image = tracer.moment_image();
    pres[2].image = tracer.gbuffer_image();
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, pres);
  };
  const auto prep_out = [&](VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  };
  const auto finish_out = [&](VkCommandBuffer cb, VkImage img, VkBuffer buf) {
    VkImageMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = img;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &post);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {side, side, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);
  };
  const auto dispatch_var = [&](float k, OutBuf& out) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    barrier_inputs(cb);
    prep_out(cb, out.img);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, var_pipe.pipeline());
    VkDescriptorSet ds = var_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            var_pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, kNoisyFrames, kNoisyFrames, 1U};
    vkCmdPushConstants(cb, var_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  const auto dispatch_edge = [&](float k, float nsigma, float dsigma,
                                 VkImageView gview, OutBuf& out) {
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 2,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, gview,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    barrier_inputs(cb);
    // The uniform control image was cleared (TRANSFER write), not traced.
    if (gview == uni_geo.view) {
      VkImageMemoryBarrier g{};
      g.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      g.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      g.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      g.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.image = uni_geo.img;
      g.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &g);
    }
    prep_out(cb, out.img);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      edge_pipe.pipeline());
    VkDescriptorSet ds = edge_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            edge_pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma, nsigma, dsigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, nsigma, dsigma, kNoisyFrames, kNoisyFrames,
            1U};
    vkCmdPushConstants(cb, edge_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  const auto read_out = [&](OutBuf& out) {
    std::vector<float> v(static_cast<std::size_t>(side) * side * 4U);
    std::memcpy(v.data(), out.buf.mapped, v.size() * sizeof(float));
    return v;
  };

  dispatch_var(2.0F, var_out);
  const std::vector<float> var_img = read_out(var_out);
  // Sweep the stop widths honestly: the edge filter gets its best (k, ns,
  // ds), not the first guess. Stash outputs; the reference is traced after
  // the sweep so the 4spp inputs cannot have moved.
  struct EdgeCase {
    float k, ns, ds;
    std::vector<float> img;
  };
  std::vector<EdgeCase> cases;
  for (float k : {1.0F, 2.0F}) {
    for (float ns : {0.05F, 0.1F, 0.2F}) {
      for (float ds : {0.1F, 0.3F, 1.0F}) {
        dispatch_edge(k, ns, ds, tracer.gbuffer_view(), edge_out);
        cases.push_back({k, ns, ds, read_out(edge_out)});
      }
    }
  }
  // Temporal-history pin for the 4-binding form (best case re-dispatched
  // after the sweep; inputs unchanged since the trace). Index fixed up
  // after scoring below -- re-dispatch the k=2/ns=0.1/ds=0.3 member here
  // and compare against its stashed twin.
  const std::size_t rep_idx = 13U;  // k=2, ns=0.1, ds=0.3 in sweep order
  dispatch_edge(cases[rep_idx].k, cases[rep_idx].ns, cases[rep_idx].ds,
                tracer.gbuffer_view(), edge_out);
  const std::vector<float> edge_rep = read_out(edge_out);
  EXPECT_EQ(std::memcmp(cases[rep_idx].img.data(), edge_rep.data(),
                        edge_rep.size() * sizeof(float)),
            0)
      << "edge-stopped filter is not a pure function of its inputs";
  // Geometry-wire guard: uniform geometry must reproduce variance-only
  // (no edges to stop on == no new signal). A shader that ignores binding
  // 2 returns the edge answer here instead.
  dispatch_edge(2.0F, 0.1F, 0.3F, uni_geo.view, edge_out);
  const std::vector<float> uni_img = read_out(edge_out);
  double geo_blind_sse = 0.0;
  for (std::size_t i = 0; i < var_img.size(); ++i) {
    const double d =
        static_cast<double>(var_img[i]) - static_cast<double>(uni_img[i]);
    geo_blind_sse += d * d;
  }
  const double geo_blind_mse =
      geo_blind_sse / static_cast<double>(var_img.size());
  std::printf("LoopT2 curve spike: geometry-blind MSE vs variance-only = %.6e\n",
              geo_blind_mse);
  EXPECT_LT(geo_blind_mse, 1e-12)
      << "uniform geometry must reduce to variance-only; binding 2 is dead";

  tracer.reset_accumulation();
  record_rounds(kRefFrames);
  EXPECT_EQ(tracer.sample_count(), kRefFrames);
  auto ref = tracer.read_mean();
  ASSERT_TRUE(ref.is_ok());
  const auto luma_mse = [&](const std::vector<float>& img) {
    constexpr double w[3] = {0.2126, 0.7152, 0.0722};
    const std::size_t texels = ref.value().size() / 4U;
    double sse = 0.0;
    for (std::size_t t = 0; t < texels; ++t) {
      double a = 0.0;
      double b = 0.0;
      for (std::size_t c = 0; c < 3U; ++c) {
        a += w[c] * static_cast<double>(img[t * 4U + c]);
        b += w[c] * static_cast<double>(ref.value()[t * 4U + c]);
      }
      const double d = a - b;
      sse += d * d;
    }
    return sse / static_cast<double>(texels);
  };
  const double raw_mse = luma_mse(noisy.value());
  const double var_mse = luma_mse(var_img);
  double best_mse = 1e30;
  std::size_t best_i = 0U;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const double m = luma_mse(cases[i].img);
    std::printf("LoopT2 curve spike: k=%.1f ns=%.2f ds=%.1f MSE=%.6e (var %.2fx)\n",
                static_cast<double>(cases[i].k),
                static_cast<double>(cases[i].ns),
                static_cast<double>(cases[i].ds), m, var_mse / m);
    if (m < best_mse) {
      best_mse = m;
      best_i = i;
    }
  }
  const double edge_mse = best_mse;
  std::printf("LoopT2 curve spike: raw=%.6e var(k=2)=%.6e BEST k=%.1f ns=%.2f ds=%.1f edge=%.6e (var %.2fx, raw %.2fx)\n",
              raw_mse, var_mse, static_cast<double>(cases[best_i].k),
              static_cast<double>(cases[best_i].ns),
              static_cast<double>(cases[best_i].ds), edge_mse,
              var_mse / edge_mse, raw_mse / edge_mse);
  // Second wire direction: true geometry must CHANGE the answer (a shader
  // ignoring binding 2 returns the uniform image here instead).
  EXPECT_NE(std::memcmp(cases[best_i].img.data(), uni_img.data(),
                        uni_img.size() * sizeof(float)),
            0)
      << "true geometry identical to uniform: binding 2 is decorative";
  // Efficacy, measured 2026-11-06 on the RTX 2060 (same camera/bounces/
  // sky/measure as the box spike; sphere r=0.5 at the origin: raw
  // 2.894761e-03, var(k=2) 1.479378e-03): best edge k=2.0/ns=0.2/ds=0.1
  // is 1.435363e-03, i.e. 0.970x variance-only (1.03x) and 0.496x raw
  // (2.02x). The pin is 0.99x var: a sabotaged G-buffer store
  // (all-miss geometry) collapses the edge answer onto variance-only
  // (ratio 1.0) and fails it; the true 0.970 clears it with a 2-point
  // margin. Gain composition is INVERTED vs the planar scene: the ds
  // sweep is flat (~0.05% -- small silhouette, interior-dominated) while
  // the ns sweep moves 2% (ns=0.05 over-stops at 1.463669e-03 vs best
  // 1.435363e-03) -- the normal weight is live here, inert there. The
  // ns pin below traps a regression that flattens it (a dead normal
  // weight returns the best value for every ns). A larger sphere
  // (r=1.0, tried 2026-11-06) is a WORSE spike: raw 3.28e-02, the frame
  // is variance-dominated multi-bounce and single-iteration buys 1.02x
  // over var -- scene selection matters, recorded so it is not retried.
  EXPECT_LT(edge_mse, raw_mse * 0.70)
      << "edge-stopped filter stopped earning its keep: " << edge_mse
      << " vs raw " << raw_mse;
  EXPECT_LT(edge_mse, var_mse * 0.99)
      << "geometry stop added nothing over variance-only: edge " << edge_mse
      << " vs variance-only " << var_mse;
  // Sweep order is k outer / ns middle / ds inner: index 9 is
  // k=2.0/ns=0.05/ds=0.1, the over-stopped corner (measured 1.0197x the
  // best). A normal weight that stops nothing makes every ns identical.
  EXPECT_GT(luma_mse(cases[9].img), edge_mse * 1.01)
      << "ns sweep went flat: the normal stop is inert again";

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  var_pipe.cleanup(ctx.device());
  edge_pipe.cleanup(ctx.device());
  var_mgr.cleanup();
  edge_mgr.cleanup();
  for (OutBuf* o : {&var_out, &edge_out, &uni_geo}) {
    vkDestroyImageView(ctx.device(), o->view, nullptr);
    vkDestroyImage(ctx.device(), o->img, nullptr);
    alloc.destroy_allocation(o->mem);
    auto b = o->buf;
    alloc.destroy_allocation(b);
  }
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

TEST(path_tracing_real, denoise_edge_multi_iteration_spike) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtEdgeIterSpike", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kNoisyFrames = 4U;
  constexpr std::uint32_t kRefFrames = 128U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  cfg.sky.mode = 1U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto submit_wait = [&](VkCommandBuffer cb) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
  };
  const auto record_rounds = [&](std::uint32_t frames) {
    for (std::uint32_t r = 0; r < frames; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_wait(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    }
  };

  record_rounds(kNoisyFrames);
  EXPECT_EQ(tracer.sample_count(), kNoisyFrames);
  auto noisy = tracer.read_mean();
  ASSERT_TRUE(noisy.is_ok());

  // Output image helper: rgba32f storage + transfer readback through a
  // host-visible buffer.
  struct OutBuf {
    VkImage img{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    omnicpp::render::Allocation mem{};
    omnicpp::render::Allocation buf{};
  };
  auto make_out = [&]() {
    OutBuf o{};
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent = {side, side, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    EXPECT_EQ(vkCreateImage(ctx.device(), &ii, nullptr, &o.img), VK_SUCCESS);
    auto mem = alloc.bind_image(o.img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    EXPECT_TRUE(mem.is_ok());
    o.mem = mem.value();
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = o.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    EXPECT_EQ(vkCreateImageView(ctx.device(), &vi, nullptr, &o.view), VK_SUCCESS);
    auto buf = alloc.create_buffer(
        static_cast<VkDeviceSize>(side) * side * 16U,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    EXPECT_TRUE(buf.is_ok());
    o.buf = buf.value();
    return o;
  };
  OutBuf var_out = make_out();
  // Uniform-geometry control image: same shape, cleared to (0,0,1,+1).
  OutBuf uni_geo = make_out();
  {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.image = uni_geo.img;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &pre);
    VkClearColorValue v{};
    v.float32[0] = 0.0F;
    v.float32[1] = 0.0F;
    v.float32[2] = 1.0F;
    v.float32[3] = 1.0F;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, uni_geo.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1,
                         &range);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }

  OutBuf ping = make_out();
  OutBuf pong = make_out();
  // First-touch: both ping-pong images enter GENERAL once and never leave
  // it outside finish_out's internal round-trip (which returns to GENERAL),
  // so every iteration dispatch sees GENERAL on both sides. All dispatches
  // are idle-separated submits, so no inter-dispatch barriers are needed
  // beyond the layout contract.
  {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier first[2]{};
    VkImage outs[2] = {ping.img, pong.img};
    for (int f = 0; f < 2; ++f) {
      first[f].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      first[f].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      first[f].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      first[f].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      first[f].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      first[f].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      first[f].image = outs[f];
      first[f].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 2, first);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }

  omnicpp::render::VulkanDescriptorManager var_mgr, edge_mgr;
  ASSERT_TRUE(var_mgr.initialize(ctx.device()).is_ok());
  ASSERT_TRUE(edge_mgr.initialize(ctx.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> var_b(3);
  for (std::uint32_t b = 0; b < 3U; ++b) {
    var_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto var_layout = var_mgr.create_layout(var_b, 1);
  ASSERT_TRUE(var_layout.is_ok());
  auto var_set = var_mgr.allocate_set(var_layout.value());
  ASSERT_TRUE(var_set.is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 0,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.accum_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.moment_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 2,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  var_out.view, VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  std::vector<omnicpp::render::ReflectedBinding> edge_b(4);
  for (std::uint32_t b = 0; b < 4U; ++b) {
    edge_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto edge_layout = edge_mgr.create_layout(edge_b, 1);
  ASSERT_TRUE(edge_layout.is_ok());
  auto edge_set = edge_mgr.allocate_set(edge_layout.value());
  ASSERT_TRUE(edge_set.is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 0,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.accum_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.moment_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  // Bindings 0/2/3 (mean input, G-buffer, output) are rebound per
  // iteration: mean alternates sums/ping/pong, G-buffer true vs uniform.
  // Binding 3 (output) placeholder -- rebound by every iteration.
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 3,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, ping.view,
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  omnicpp::render::VulkanPipeline var_pipe, edge_pipe;
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28U};
    ASSERT_TRUE(var_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_variance_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(var_pipe.create_pipeline_layout(ctx.device(),
                                                &var_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(var_pipe.create_compute_pipeline(ctx.device(),
                                                 var_pipe.pipeline_layout())
                    .is_ok());
  }
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 36U};
    ASSERT_TRUE(edge_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_edge_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(edge_pipe.create_pipeline_layout(ctx.device(),
                                                &edge_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(edge_pipe.create_compute_pipeline(ctx.device(),
                                                  edge_pipe.pipeline_layout())
                    .is_ok());
  }

  // PT inputs barrier shared by every dispatch: accum + moment + gbuffer
  // GENERAL->GENERAL (they hold the accumulation, not UNDEFINED).
  const auto barrier_inputs = [&](VkCommandBuffer cb) {
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pres[3] = {pre, pre, pre};
    pres[0].image = tracer.accum_image();
    pres[1].image = tracer.moment_image();
    pres[2].image = tracer.gbuffer_image();
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, pres);
  };
  const auto prep_out = [&](VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  };
  const auto finish_out = [&](VkCommandBuffer cb, VkImage img, VkBuffer buf) {
    VkImageMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = img;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &post);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {side, side, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);
    // Return to GENERAL: ping-pong images live in GENERAL permanently,
    // so the next iteration (or chain) needs no layout tracking.
    VkImageMemoryBarrier ret{};
    ret.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ret.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ret.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    ret.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ret.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ret.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ret.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ret.image = img;
    ret.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &ret);
  };
  const auto dispatch_var = [&](float k, OutBuf& out) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    barrier_inputs(cb);
    prep_out(cb, out.img);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, var_pipe.pipeline());
    VkDescriptorSet ds = var_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            var_pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, kNoisyFrames, kNoisyFrames, 1U};
    vkCmdPushConstants(cb, var_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  // One edge-stopped iteration. Iteration 0 binds the live SUMS (mean_n
  // = 4); later iterations bind a normalized ping-pong image (mean_n =
  // 1). The moment image always binds sums (moment_n = 4) -- iterations
  // >0 guide on E2_orig - filtered_mean^2, the documented loop29-s4
  // approximation this measurement owns. Binding 0 (mean input) and
  // binding 3 (output) are rebound every call; binding 2 (geometry) is
  // rebound too, so the uniform control runs the identical path.
  const auto dispatch_edge_iter = [&](VkImage in_img, VkImageView in_view,
                                      std::uint32_t mean_n, float k,
                                      float nsigma, float dsigma,
                                      std::uint32_t stride, VkImageView gview,
                                      OutBuf& out, bool first, bool last) {
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 0,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, in_view,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 2,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, gview,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 3,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, out.view,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    if (first) {
      barrier_inputs(cb);
    } else {
      // Ping-pong input: GENERAL already (first-touch + finish_out's
      // return trip); declare the compute read. The uniform control
      // image takes this path too (TRANSFER-written, GENERAL layout).
      VkImageMemoryBarrier g{};
      g.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      g.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      g.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      g.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.image = in_img;
      g.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &g);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      edge_pipe.pipeline());
    VkDescriptorSet eds = edge_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            edge_pipe.pipeline_layout(), 0, 1, &eds, 0,
                            nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma, nsigma, dsigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, nsigma, dsigma, mean_n, kNoisyFrames, stride};
    vkCmdPushConstants(cb, edge_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    if (last) finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  const auto read_out = [&](OutBuf& out) {
    std::vector<float> v(static_cast<std::size_t>(side) * side * 4U);
    std::memcpy(v.data(), out.buf.mapped, v.size() * sizeof(float));
    return v;
  };

  dispatch_var(2.0F, var_out);
  const std::vector<float> var_img = read_out(var_out);
  // K-iteration ping-pong with stride doubling (loop29 s4 shape, edge
  // weights on). Iteration 0 binds the live SUMS (divisor 4); later
  // iterations bind the normalized ping-pong (divisor 1). The moment image
  // always binds sums (divisor 4) -- iterations >0 guide on E2_orig minus
  // filtered_mean^2, the documented s4 approximation this measurement
  // owns. The G-buffer is CONSTANT across iterations (first-hit geometry
  // does not move), so unlike the variance width the edge stops never go
  // stale: the question this spike answers is whether that saves iteration
  // from the s4 divergence (K=2 0.94x, K=3 0.48x -- do NOT build on the
  // variance signal alone). ns/ds fixed at the loop30 planar best
  // (0.1/1.0); k swept per K, s4-style. Stash outputs; the reference is
  // traced after the sweep so the 4spp inputs cannot have moved.
  struct IterCase {
    std::uint32_t rounds;
    float k;
    std::vector<float> img;
  };
  std::vector<IterCase> cases;
  const auto run_chain = [&](std::uint32_t rounds, float k, float ns,
                             float ds, VkImageView gview) {
    for (std::uint32_t it = 0U; it < rounds; ++it) {
      const bool first = it == 0U;
      VkImage in_img = tracer.accum_image();
      VkImageView in_view = tracer.accum_view();
      std::uint32_t mean_n = kNoisyFrames;
      if (!first) {
        const bool odd = it % 2U == 1U;
        in_img = odd ? ping.img : pong.img;
        in_view = odd ? ping.view : pong.view;
        mean_n = 1U;
      }
      OutBuf& out = (it % 2U == 0U) ? ping : pong;
      const bool last = it + 1U == rounds;
      const std::uint32_t stride = 1U << it;
      dispatch_edge_iter(in_img, in_view, mean_n, k, ns, ds, stride, gview,
                         out, first, last);
    }
    OutBuf& fin = (rounds % 2U == 1U) ? ping : pong;
    return read_out(fin);
  };
  for (std::uint32_t rounds : {1U, 2U, 3U}) {
    for (float k : {0.5F, 1.0F, 2.0F}) {
      cases.push_back(
          {rounds, k, run_chain(rounds, k, 0.1F, 1.0F,
                                tracer.gbuffer_view())});
    }
  }
  // Temporal-history pin for the chain form: K=2/k=2.0 re-dispatched
  // after the sweep (inputs unchanged since the trace) must be
  // bit-identical -- the chain is a pure function of its inputs.
  const std::vector<float> chain_rep =
      run_chain(2U, 2.0F, 0.1F, 1.0F, tracer.gbuffer_view());
  EXPECT_EQ(std::memcmp(cases[5].img.data(), chain_rep.data(),
                        chain_rep.size() * sizeof(float)),
            0)
      << "iterated edge-stopped filter is not a pure function of its inputs";
  // K-wire guard: K=2 and K=3 (k=2.0) must DIFFER -- an iteration that
  // changes nothing is a no-op chain, and the verdict below would be
  // evidence of nothing. (Cases order: rounds outer, k inner:
  // index 5 = K=2/k=2.0, index 8 = K=3/k=2.0.)
  EXPECT_NE(std::memcmp(cases[5].img.data(), cases[8].img.data(),
                        cases[8].img.size() * sizeof(float)),
            0)
      << "K=2 and K=3 agree bit-exactly: iteration 2 is a no-op";
  // Stride-wire guard (s4 shape): K=2 with strides (1,2) must differ from
  // K=2 with strides (1,1) -- stride doubling must be live in the chain.
  {
    dispatch_edge_iter(tracer.accum_image(), tracer.accum_view(),
                       kNoisyFrames, 2.0F, 0.1F, 1.0F, 1U,
                       tracer.gbuffer_view(), ping, true, false);
    dispatch_edge_iter(ping.img, ping.view, 1U, 2.0F, 0.1F, 1.0F, 1U,
                       tracer.gbuffer_view(), pong, false, true);
    const std::vector<float> stride11 = read_out(pong);
    EXPECT_NE(std::memcmp(cases[5].img.data(), stride11.data(),
                          stride11.size() * sizeof(float)),
              0)
        << "stride doubling is dead: (1,2) matches (1,1)";
  }
  // Geometry-wire guards for the chain, both legs K-matched (a K=2
  // uniform chain is a different algorithm than K=1 variance-only, so the
  // loop30 single-dispatch guard does not transfer -- first version of
  // this test compared them and failed on its own guard, correctly).
  // (a) K=1 with uniform geometry reproduces variance-only bit-nearly
  // (no edges to stop on == no new signal in EITHER iteration slot).
  const std::vector<float> uni_k1 =
      run_chain(1U, 2.0F, 0.1F, 1.0F, uni_geo.view);
  double geo_blind_sse = 0.0;
  for (std::size_t i = 0; i < var_img.size(); ++i) {
    const double d =
        static_cast<double>(var_img[i]) - static_cast<double>(uni_k1[i]);
    geo_blind_sse += d * d;
  }
  const double geo_blind_mse =
      geo_blind_sse / static_cast<double>(var_img.size());
  std::printf("LoopT2 edge-iter spike: geometry-blind MSE vs variance-only = %.6e\n",
              geo_blind_mse);
  EXPECT_LT(geo_blind_mse, 1e-12)
      << "uniform geometry must reduce to variance-only; binding 2 is dead";
  // (b) K=2 with uniform geometry through both iterations must DIFFER
  // from K=2 with true geometry -- binding 2 rebound every iteration, so
  // a rebind that drops it in later rounds returns the K=1 answer here
  // instead of the chain answer.
  const std::vector<float> uni_img =
      run_chain(2U, 2.0F, 0.1F, 1.0F, uni_geo.view);
  EXPECT_NE(std::memcmp(cases[5].img.data(), uni_img.data(),
                        uni_img.size() * sizeof(float)),
            0)
      << "uniform K=2 chain matches true K=2 chain: binding 2 dropped "
         "after iteration 0";

  tracer.reset_accumulation();
  record_rounds(kRefFrames);
  EXPECT_EQ(tracer.sample_count(), kRefFrames);
  auto ref = tracer.read_mean();
  ASSERT_TRUE(ref.is_ok());
  const auto luma_mse = [&](const std::vector<float>& img) {
    constexpr double w[3] = {0.2126, 0.7152, 0.0722};
    const std::size_t texels = ref.value().size() / 4U;
    double sse = 0.0;
    for (std::size_t t = 0; t < texels; ++t) {
      double a = 0.0;
      double b = 0.0;
      for (std::size_t c = 0; c < 3U; ++c) {
        a += w[c] * static_cast<double>(img[t * 4U + c]);
        b += w[c] * static_cast<double>(ref.value()[t * 4U + c]);
      }
      const double d = a - b;
      sse += d * d;
    }
    return sse / static_cast<double>(texels);
  };
  const double raw_mse = luma_mse(noisy.value());
  const double var_mse = luma_mse(var_img);
  // Per-K best (k swept per K, s4-style) + the verdict print. Whether
  // geometry guidance saves iteration from the s4 divergence is a
  // MEASUREMENT this run reports, not a pin this run asserts -- the
  // structural pins above (chain purity, K-wire, stride-wire,
  // geometry-wire) are what stop a dead chain from passing silently.
  // The one efficacy pin: K=1 IS the loop30 single-iteration filter, so
  // it reproduces the loop30 bars in-harness (0.963x var, 0.577x raw).
  for (std::uint32_t rounds : {1U, 2U, 3U}) {
    double best = 1e30;
    float best_k = 0.0F;
    for (const IterCase& c : cases) {
      if (c.rounds != rounds) continue;
      const double m = luma_mse(c.img);
      std::printf("LoopT2 edge-iter spike: K=%u k=%.1f MSE=%.6e (var %.2fx)\n",
                  rounds, static_cast<double>(c.k), m, var_mse / m);
      if (m < best) {
        best = m;
        best_k = c.k;
      }
    }
    const double k1 = luma_mse(cases[2].img);  // K=1/k=2.0 reference leg
    std::printf("LoopT2 edge-iter spike: K=%u best k=%.1f MSE=%.6e "
                "(K=1/k=2 %.2fx, raw %.2fx)\n",
                rounds, static_cast<double>(best_k), best, k1 / best,
                raw_mse / best);
  }
  // Second wire direction: true geometry must CHANGE the answer (a shader
  // ignoring binding 2 returns the uniform image here instead). K=2/k=2.0
  // is the chain representative (index 5).
  EXPECT_NE(std::memcmp(cases[5].img.data(), uni_img.data(),
                        uni_img.size() * sizeof(float)),
            0)
      << "true geometry identical to uniform: binding 2 is decorative";
  // K=1/k=2.0 reproduces the loop30 single-iteration bars in-harness.
  const double k1_mse = luma_mse(cases[2].img);
  EXPECT_LT(k1_mse, raw_mse * 0.70)
      << "K=1 stopped reproducing the single-iteration bar: " << k1_mse
      << " vs raw " << raw_mse;
  EXPECT_LT(k1_mse, var_mse * 0.99)
      << "K=1 stopped reproducing the single-iteration bar: K=1 " << k1_mse
      << " vs variance-only " << var_mse;

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  var_pipe.cleanup(ctx.device());
  edge_pipe.cleanup(ctx.device());
  var_mgr.cleanup();
  edge_mgr.cleanup();
  for (OutBuf* o : {&var_out, &ping, &pong, &uni_geo}) {
    vkDestroyImageView(ctx.device(), o->view, nullptr);
    vkDestroyImage(ctx.device(), o->img, nullptr);
    alloc.destroy_allocation(o->mem);
    auto b = o->buf;
    alloc.destroy_allocation(b);
  }
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

//! Loop34: per-iteration variance update -- the open alternative the loop33
//! verdict leaves standing. Loop33 showed edge-stopped K=2/K=3 diverge
//! exactly like variance-only (K=2/k=2 edge == s4 to 2%): the mechanism is
//! the STALE variance guide (E2_orig minus an increasingly smooth mean
//! keeps every width wide while the footprint grows). The obvious rescue is
//! to recompute the guide per iteration: smooth E2 with the same
//! spatial x geometry kernel (variance-blind -- the E2 image's own
//! pseudo-variance clamps to zero wherever luma(E2) >= 1, i.e. the sky, so
//! reusing the edge shader with a huge k would silently no-op there; that
//! reuse was considered and rejected, and denoise_moment_smooth.comp
//! exists because of it), then run iteration 1 on v = E2smooth - M1^2.
//! Scene + measure are identical to loop29 s4 / loop30 / loop33 (box,
//! 64x64, 4spp vs 128spp, analytic sky -- K=1/k=2.0 must reproduce the
//! loop30 bar, which validates the chain harness end to end). it0 is fixed
//! at the loop30 best (k=2.0); it1 sweeps k s4-style on BOTH legs (stale
//! E2orig vs updated E2smooth), so the verdict is a same-k comparison.
//! Whether the update saves iteration 2 is a MEASUREMENT this run reports,
//! not a pin this run asserts -- the structural pins below (chain purity,
//! update-live, guide-live, smoother-geometry, smoother DC + noise,
//! K-wire, stride-wire, geometry-wire, all K-matched where the loop33
//! trap applies) are what stop a dead chain from passing silently.
TEST(path_tracing_real, denoise_edge_variance_update_spike) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtVarUpdateSpike", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kNoisyFrames = 4U;
  constexpr std::uint32_t kRefFrames = 128U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  cfg.sky.mode = 1U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto submit_wait = [&](VkCommandBuffer cb) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
  };
  const auto record_rounds = [&](std::uint32_t frames) {
    for (std::uint32_t r = 0; r < frames; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_wait(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    }
  };

  record_rounds(kNoisyFrames);
  EXPECT_EQ(tracer.sample_count(), kNoisyFrames);
  auto noisy = tracer.read_mean();
  ASSERT_TRUE(noisy.is_ok());

  // Output image helper: rgba32f storage + transfer readback through a
  // host-visible buffer.
  struct OutBuf {
    VkImage img{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    omnicpp::render::Allocation mem{};
    omnicpp::render::Allocation buf{};
  };
  auto make_out = [&]() {
    OutBuf o{};
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent = {side, side, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    EXPECT_EQ(vkCreateImage(ctx.device(), &ii, nullptr, &o.img), VK_SUCCESS);
    auto mem = alloc.bind_image(o.img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    EXPECT_TRUE(mem.is_ok());
    o.mem = mem.value();
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = o.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    EXPECT_EQ(vkCreateImageView(ctx.device(), &vi, nullptr, &o.view), VK_SUCCESS);
    auto buf = alloc.create_buffer(
        static_cast<VkDeviceSize>(side) * side * 16U,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    EXPECT_TRUE(buf.is_ok());
    o.buf = buf.value();
    return o;
  };
  OutBuf var_out = make_out();
  // Uniform-geometry control: (0,0,1,+1). Zeroed guide: all zeros, so the
  // guided iteration sees vc = 0 everywhere (edge2 = 1e-8 -- output is the
  // center texel, no smoothing) and must differ from the E2s-guided chain.
  OutBuf uni_geo = make_out();
  OutBuf zero_geo = make_out();
  {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    for (OutBuf* o : {&uni_geo, &zero_geo}) {
      VkImageMemoryBarrier pre{};
      pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre.image = o->img;
      pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &pre);
    }
    VkClearColorValue v{};
    v.float32[0] = 0.0F;
    v.float32[1] = 0.0F;
    v.float32[2] = 1.0F;
    v.float32[3] = 1.0F;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, uni_geo.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1,
                         &range);
    VkClearColorValue z{};
    z.float32[0] = 0.0F;
    z.float32[1] = 0.0F;
    z.float32[2] = 0.0F;
    z.float32[3] = 0.0F;
    vkCmdClearColorImage(cb, zero_geo.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1,
                         &range);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }

  OutBuf ping = make_out();
  OutBuf pong = make_out();
  OutBuf e2sm = make_out();
  // First-touch: ping/pong/e2sm enter GENERAL once and never leave it
  // outside finish_out's internal round-trip (which returns to GENERAL),
  // so every iteration dispatch sees GENERAL on all three sides. All
  // dispatches are idle-separated submits, so no inter-dispatch barriers
  // are needed beyond the layout contract.
  {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier first[3]{};
    VkImage outs[3] = {ping.img, pong.img, e2sm.img};
    for (int f = 0; f < 3; ++f) {
      first[f].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      first[f].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      first[f].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      first[f].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      first[f].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      first[f].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      first[f].image = outs[f];
      first[f].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, first);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }

  // Staging buffer for the host-side copy of the moment (E2 sums) image.
  auto mbuf_r = alloc.create_buffer(
      static_cast<VkDeviceSize>(side) * side * 16U,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(mbuf_r.is_ok());
  auto mbuf = mbuf_r.value();
  const auto copy_moment = [&]() {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = tracer.moment_image();
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_src);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {side, side, 1};
    vkCmdCopyImageToBuffer(cb, tracer.moment_image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mbuf.buffer, 1,
                           &region);
    VkImageMemoryBarrier ret{};
    ret.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ret.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ret.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    ret.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ret.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ret.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ret.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ret.image = tracer.moment_image();
    ret.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &ret);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  copy_moment();
  std::vector<float> e2_sums(static_cast<std::size_t>(side) * side * 4U);
  std::memcpy(e2_sums.data(), mbuf.mapped, e2_sums.size() * sizeof(float));
  // Normalized E2 (sums depth 4) + its host-side characterization: the
  // smoother must preserve DC while strictly reducing neighbor energy.
  const auto luma3 = [](float r, float g, float b) {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) +
           0.0722 * static_cast<double>(b);
  };
  std::vector<float> e2_norm(e2_sums.size());
  for (std::size_t i = 0; i < e2_sums.size(); ++i)
    e2_norm[i] = e2_sums[i] / static_cast<float>(kNoisyFrames);
  const auto mean_luma = [&](const std::vector<float>& img) {
    double s = 0.0;
    const std::size_t texels = img.size() / 4U;
    for (std::size_t t = 0; t < texels; ++t)
      s += luma3(img[t * 4U], img[t * 4U + 1U], img[t * 4U + 2U]);
    return s / static_cast<double>(texels);
  };
  const auto neighbor_energy = [&](const std::vector<float>& img) {
    double e = 0.0;
    for (std::uint32_t y = 0; y < side; ++y) {
      for (std::uint32_t x = 0; x < side; ++x) {
        const std::size_t t = static_cast<std::size_t>(y) * side + x;
        const double lc =
            luma3(img[t * 4U], img[t * 4U + 1U], img[t * 4U + 2U]);
        if (x + 1U < side) {
          const std::size_t r = t + 1U;
          const double lr =
              luma3(img[r * 4U], img[r * 4U + 1U], img[r * 4U + 2U]);
          const double d = lc - lr;
          e += d * d;
        }
        if (y + 1U < side) {
          const std::size_t dwn = t + side;
          const double ld = luma3(img[dwn * 4U], img[dwn * 4U + 1U],
                                  img[dwn * 4U + 2U]);
          const double d = lc - ld;
          e += d * d;
        }
      }
    }
    return e;
  };
  const double e2_mean = mean_luma(e2_norm);
  const double e2_energy = neighbor_energy(e2_norm);
  std::printf("LoopT2 var-update spike: E2 mean %.6e neighbor-energy %.6e\n",
              e2_mean, e2_energy);

  omnicpp::render::VulkanDescriptorManager var_mgr, edge_mgr, smooth_mgr;
  ASSERT_TRUE(var_mgr.initialize(ctx.device()).is_ok());
  ASSERT_TRUE(edge_mgr.initialize(ctx.device()).is_ok());
  ASSERT_TRUE(smooth_mgr.initialize(ctx.device()).is_ok());
  std::vector<omnicpp::render::ReflectedBinding> var_b(3);
  for (std::uint32_t b = 0; b < 3U; ++b) {
    var_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto var_layout = var_mgr.create_layout(var_b, 1);
  ASSERT_TRUE(var_layout.is_ok());
  auto var_set = var_mgr.allocate_set(var_layout.value());
  ASSERT_TRUE(var_set.is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 0,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.accum_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  tracer.moment_view(), VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(var_mgr.write_image(var_set.value(), 2,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                                  var_out.view, VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  std::vector<omnicpp::render::ReflectedBinding> edge_b(4);
  for (std::uint32_t b = 0; b < 4U; ++b) {
    edge_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto edge_layout = edge_mgr.create_layout(edge_b, 1);
  ASSERT_TRUE(edge_layout.is_ok());
  auto edge_set = edge_mgr.allocate_set(edge_layout.value());
  ASSERT_TRUE(edge_set.is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 0,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.accum_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, tracer.moment_view(),
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  // Bindings 0/1/2/3 (mean input, variance guide, G-buffer, output) are
  // rebound every iteration: mean alternates sums/ping, the guide
  // alternates E2-sums/E2-smooth/zero, G-buffer true vs uniform.
  ASSERT_TRUE(edge_mgr.write_image(edge_set.value(), 3,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                   VK_NULL_HANDLE, ping.view,
                                   VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  std::vector<omnicpp::render::ReflectedBinding> smooth_b(3);
  for (std::uint32_t b = 0; b < 3U; ++b) {
    smooth_b[b] = {0, b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                   VK_SHADER_STAGE_COMPUTE_BIT};
  }
  auto smooth_layout = smooth_mgr.create_layout(smooth_b, 1);
  ASSERT_TRUE(smooth_layout.is_ok());
  auto smooth_set = smooth_mgr.allocate_set(smooth_layout.value());
  ASSERT_TRUE(smooth_set.is_ok());
  ASSERT_TRUE(smooth_mgr.write_image(smooth_set.value(), 0,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, tracer.moment_view(),
                                     VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());
  ASSERT_TRUE(smooth_mgr.write_image(smooth_set.value(), 2,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, e2sm.view,
                                     VK_IMAGE_LAYOUT_GENERAL)
                  .is_ok());

  const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
  omnicpp::render::VulkanPipeline var_pipe, edge_pipe, smooth_pipe;
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28U};
    ASSERT_TRUE(var_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_variance_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(var_pipe.create_pipeline_layout(ctx.device(),
                                                &var_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(var_pipe.create_compute_pipeline(ctx.device(),
                                                 var_pipe.pipeline_layout())
                    .is_ok());
  }
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 36U};
    ASSERT_TRUE(edge_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_edge_atrous.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(edge_pipe.create_pipeline_layout(ctx.device(),
                                                &edge_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(edge_pipe.create_compute_pipeline(ctx.device(),
                                                  edge_pipe.pipeline_layout())
                    .is_ok());
  }
  {
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 28U};
    ASSERT_TRUE(smooth_pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_moment_smooth.comp.spv",
        "compute").is_ok());
    ASSERT_TRUE(smooth_pipe.create_pipeline_layout(
        ctx.device(), &smooth_layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(smooth_pipe.create_compute_pipeline(
        ctx.device(), smooth_pipe.pipeline_layout())
                    .is_ok());
  }

  // PT inputs barrier shared by every dispatch: accum + moment + gbuffer
  // GENERAL->GENERAL (they hold the accumulation, not UNDEFINED).
  const auto barrier_inputs = [&](VkCommandBuffer cb) {
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pres[3] = {pre, pre, pre};
    pres[0].image = tracer.accum_image();
    pres[1].image = tracer.moment_image();
    pres[2].image = tracer.gbuffer_image();
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, pres);
  };
  const auto prep_out = [&](VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  };
  const auto finish_out = [&](VkCommandBuffer cb, VkImage img, VkBuffer buf) {
    VkImageMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = img;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &post);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {side, side, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);
    // Return to GENERAL: ping-pong/smooth images live in GENERAL
    // permanently, so the next iteration (or chain) needs no layout
    // tracking.
    VkImageMemoryBarrier ret{};
    ret.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ret.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ret.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    ret.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ret.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ret.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ret.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ret.image = img;
    ret.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &ret);
  };
  const auto dispatch_var = [&](float k, OutBuf& out) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    barrier_inputs(cb);
    prep_out(cb, out.img);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, var_pipe.pipeline());
    VkDescriptorSet ds = var_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            var_pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, kNoisyFrames, kNoisyFrames, 1U};
    vkCmdPushConstants(cb, var_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  // One edge-stopped iteration with a rebindable variance guide. it0 binds
  // the live SUMS (mean_n = 4) and the E2 SUMS (guide_n = 4); later
  // iterations bind a normalized mean (mean_n = 1) and either the E2 SUMS
  // (guide_n = 4 -- the loop33 stale form), the smoothed E2 (guide_n = 1
  // -- the loop34 update), or the zeroed image (guide_n = 1, the
  // guide-live control). Binding 2 (geometry) is rebound too, so the
  // uniform control runs the identical path.
  const auto dispatch_iter = [&](VkImage in_img, VkImageView in_view,
                                 std::uint32_t mean_n, VkImage guide_img,
                                 VkImageView guide_view, std::uint32_t guide_n,
                                 float k, float nsigma, float dsigma,
                                 std::uint32_t stride, VkImageView gview,
                                 OutBuf& out, bool first, bool last) {
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 0,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, in_view,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 1,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, guide_view,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 2,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, gview,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    EXPECT_TRUE(edge_mgr.write_image(edge_set.value(), 3,
                                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_NULL_HANDLE, out.view,
                                     VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    if (first) {
      barrier_inputs(cb);
    } else {
      // Ping-pong / smoothed inputs: GENERAL already (first-touch +
      // finish_out's return trip); declare the compute read on BOTH the
      // mean input and the guide image (the smoother wrote E2s in an
      // earlier idle-separated submit, the clear wrote the zero guide).
      VkImageMemoryBarrier g{};
      g.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      g.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      g.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      g.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageMemoryBarrier gs[2] = {g, g};
      gs[0].image = in_img;
      gs[1].image = guide_img;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 2, gs);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      edge_pipe.pipeline());
    VkDescriptorSet eds = edge_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            edge_pipe.pipeline_layout(), 0, 1, &eds, 0,
                            nullptr);
    struct Push {
      std::uint32_t w, h;
      float kk, sigma, nsigma, dsigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    Push p{side, side, k, 1.5F, nsigma, dsigma, mean_n, guide_n, stride};
    vkCmdPushConstants(cb, edge_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    if (last) finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  // One variance-blind E2 smoothing pass (the update instrument). The
  // moment image always binds sums (moment_n = 4); the output holds the
  // NORMALIZED smoothed E2, so the guided iteration binds it with
  // guide_n = 1. Binding 1 (geometry) is rebound every call, so the
  // uniform control runs the identical path.
  const auto dispatch_smooth = [&](VkImageView gview, OutBuf& out) {
    EXPECT_TRUE(smooth_mgr.write_image(smooth_set.value(), 1,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_NULL_HANDLE, gview,
                                       VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    EXPECT_TRUE(smooth_mgr.write_image(smooth_set.value(), 2,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_NULL_HANDLE, out.view,
                                       VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    barrier_inputs(cb);
    {
      // e2sm output: GENERAL already (first-touch / return trip); declare
      // the compute write over the previous chain's contents.
      VkImageMemoryBarrier g{};
      g.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      g.srcAccessMask =
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
      g.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      g.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      g.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      g.image = out.img;
      g.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &g);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      smooth_pipe.pipeline());
    VkDescriptorSet ds = smooth_set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            smooth_pipe.pipeline_layout(), 0, 1, &ds, 0,
                            nullptr);
    struct Push {
      std::uint32_t w, h;
      float sigma, nsigma, dsigma;
      std::uint32_t moment_n, stride;
    };
    Push p{side, side, 1.5F, 0.1F, 1.0F, kNoisyFrames, 1U};
    vkCmdPushConstants(cb, smooth_pipe.pipeline_layout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    finish_out(cb, out.img, out.buf.buffer);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };
  const auto read_out = [&](OutBuf& out) {
    std::vector<float> v(static_cast<std::size_t>(side) * side * 4U);
    std::memcpy(v.data(), out.buf.mapped, v.size() * sizeof(float));
    return v;
  };

  dispatch_var(2.0F, var_out);
  const std::vector<float> var_img = read_out(var_out);
  // K=1 (the loop30 single-iteration filter) in-harness, then the it0 leg
  // both chains share: k=2.0/ns=0.1/ds=1.0 over the live sums.
  dispatch_iter(tracer.accum_image(), tracer.accum_view(), kNoisyFrames,
                tracer.moment_image(), tracer.moment_view(), kNoisyFrames,
                2.0F, 0.1F, 1.0F, 1U, tracer.gbuffer_view(), ping, true, true);
  const std::vector<float> k1_img = read_out(ping);
  dispatch_iter(tracer.accum_image(), tracer.accum_view(), kNoisyFrames,
                tracer.moment_image(), tracer.moment_view(), kNoisyFrames,
                2.0F, 0.1F, 1.0F, 1U, tracer.gbuffer_view(), ping, true, false);
  // it1 legs. Stale binds the E2 SUMS (guide_n = 4 -- the loop33 form);
  // updated binds the smoothed E2 (guide_n = 1 -- the loop34 question).
  // it0's M1 sits in ping; it1 outputs land in pong. Smoothed E2 legs are
  // stashed to host vectors as they run (e2sm is reused across legs).
  dispatch_smooth(tracer.gbuffer_view(), e2sm);
  const std::vector<float> e2s_true = read_out(e2sm);
  struct ChainCase {
    const char* leg;
    float k;
    std::vector<float> img;
  };
  std::vector<ChainCase> cases;
  for (float k : {0.5F, 1.0F, 2.0F}) {
    dispatch_iter(ping.img, ping.view, 1U, e2sm.img, e2sm.view, 1U, k, 0.1F,
                  1.0F, 2U, tracer.gbuffer_view(), pong, false, true);
    cases.push_back({"updated", k, read_out(pong)});
  }
  for (float k : {0.5F, 1.0F, 2.0F}) {
    dispatch_iter(ping.img, ping.view, 1U, tracer.moment_image(),
                  tracer.moment_view(), kNoisyFrames, k, 0.1F, 1.0F, 2U,
                  tracer.gbuffer_view(), pong, false, true);
    cases.push_back({"stale", k, read_out(pong)});
  }
  // Stride-wire for the updated chain: strides (1,2) must differ from
  // (1,1) -- stride doubling must be live in the updated form too.
  dispatch_iter(ping.img, ping.view, 1U, e2sm.img, e2sm.view, 1U, 2.0F, 0.1F,
                1.0F, 1U, tracer.gbuffer_view(), pong, false, true);
  const std::vector<float> upd_stride11 = read_out(pong);
  // Guide-live control: the zero guide (vc = 0 everywhere) must change
  // the answer -- a guided iteration ignoring binding 1 returns the
  // updated image here instead.
  dispatch_iter(ping.img, ping.view, 1U, zero_geo.img, zero_geo.view, 1U,
                2.0F, 0.1F, 1.0F, 2U, tracer.gbuffer_view(), pong, false,
                true);
  const std::vector<float> zero_guide_img = read_out(pong);
  // Smoother-geometry leg: uniform geometry through the smoother must
  // DIFFER -- a smoother ignoring its gbuffer returns the true answer.
  dispatch_smooth(uni_geo.view, e2sm);
  const std::vector<float> e2s_uni = read_out(e2sm);
  // Chain geometry-wire legs, K-matched (the loop33 trap: a K=2 uniform
  // chain is a different algorithm than K=1 variance-only, so both legs
  // run the full updated K=2 chain).
  dispatch_iter(tracer.accum_image(), tracer.accum_view(), kNoisyFrames,
                tracer.moment_image(), tracer.moment_view(), kNoisyFrames,
                2.0F, 0.1F, 1.0F, 1U, uni_geo.view, ping, true, false);
  dispatch_iter(ping.img, ping.view, 1U, e2sm.img, e2sm.view, 1U, 2.0F, 0.1F,
                1.0F, 2U, uni_geo.view, pong, false, true);
  const std::vector<float> uni_upd = read_out(pong);
  // Temporal-history pin for the updated chain: it0 + smooth + it1(k=2.0)
  // re-dispatched after the sweep (inputs unchanged since the trace) must
  // be bit-identical -- the updated chain is a pure function of inputs.
  dispatch_iter(tracer.accum_image(), tracer.accum_view(), kNoisyFrames,
                tracer.moment_image(), tracer.moment_view(), kNoisyFrames,
                2.0F, 0.1F, 1.0F, 1U, tracer.gbuffer_view(), ping, true, false);
  dispatch_smooth(tracer.gbuffer_view(), e2sm);
  dispatch_iter(ping.img, ping.view, 1U, e2sm.img, e2sm.view, 1U, 2.0F, 0.1F,
                1.0F, 2U, tracer.gbuffer_view(), pong, false, true);
  const std::vector<float> upd_rep = read_out(pong);
  // Smoother characterization (host-side, on the normalized E2): DC
  // preservation + strict noise-energy reduction. A copy instead of a
  // smoother fails the energy pin; zeros fail the DC pin.
  const double e2s_mean = mean_luma(e2s_true);
  const double e2s_energy = neighbor_energy(e2s_true);
  std::printf("LoopT2 var-update spike: E2s mean %.6e (E2 %.6e, ratio %.4f) "
              "energy %.6e (E2 %.6e, ratio %.4f)\n",
              e2s_mean, e2_mean, e2s_mean / std::max(e2_mean, 1e-30),
              e2s_energy, e2_energy, e2s_energy / std::max(e2_energy, 1e-30));
  EXPECT_LT(std::fabs(e2s_mean - e2_mean) / std::max(e2_mean, 1e-30), 0.02)
      << "smoother does not preserve E2 DC: smoothed " << e2s_mean
      << " vs original " << e2_mean;
  EXPECT_LT(e2s_energy, e2_energy * 0.99)
      << "smoother does not reduce E2 neighbor energy: " << e2s_energy
      << " vs " << e2_energy;
  // Wire pins, all structural.
  EXPECT_EQ(std::memcmp(cases[2].img.data(), upd_rep.data(),
                        upd_rep.size() * sizeof(float)),
            0)
      << "updated chain is not a pure function of its inputs";
  EXPECT_NE(std::memcmp(cases[2].img.data(), cases[5].img.data(),
                        cases[5].img.size() * sizeof(float)),
            0)
      << "updated K=2 matches stale K=2: the update changes nothing";
  EXPECT_NE(std::memcmp(cases[2].img.data(), zero_guide_img.data(),
                        zero_guide_img.size() * sizeof(float)),
            0)
      << "zero guide matches E2s guide: binding 1 is decorative";
  EXPECT_NE(std::memcmp(e2s_true.data(), e2s_uni.data(),
                        e2s_uni.size() * sizeof(float)),
            0)
      << "uniform-geometry E2s matches true-geometry E2s: the smoother "
         "ignores its gbuffer";
  EXPECT_NE(std::memcmp(cases[2].img.data(), k1_img.data(),
                        k1_img.size() * sizeof(float)),
            0)
      << "K=2 updated matches K=1: iteration 1 is a no-op";
  EXPECT_NE(std::memcmp(cases[2].img.data(), upd_stride11.data(),
                        upd_stride11.size() * sizeof(float)),
            0)
      << "stride doubling is dead in the updated chain: (1,2) matches (1,1)";
  EXPECT_NE(std::memcmp(cases[2].img.data(), uni_upd.data(),
                        uni_upd.size() * sizeof(float)),
            0)
      << "uniform K=2 updated chain matches true: binding 2 dropped after "
         "iteration 0";

  tracer.reset_accumulation();
  record_rounds(kRefFrames);
  EXPECT_EQ(tracer.sample_count(), kRefFrames);
  auto ref = tracer.read_mean();
  ASSERT_TRUE(ref.is_ok());
  const auto luma_mse = [&](const std::vector<float>& img) {
    constexpr double w[3] = {0.2126, 0.7152, 0.0722};
    const std::size_t texels = ref.value().size() / 4U;
    double sse = 0.0;
    for (std::size_t t = 0; t < texels; ++t) {
      double a = 0.0;
      double b = 0.0;
      for (std::size_t c = 0; c < 3U; ++c) {
        a += w[c] * static_cast<double>(img[t * 4U + c]);
        b += w[c] * static_cast<double>(ref.value()[t * 4U + c]);
      }
      const double d = a - b;
      sse += d * d;
    }
    return sse / static_cast<double>(texels);
  };
  const double raw_mse = luma_mse(noisy.value());
  const double var_mse = luma_mse(var_img);
  // Same-k efficacy table: stale (loop33 form) vs updated (loop34 form).
  // Whether the update saves iteration 2 is a MEASUREMENT this run
  // reports, not a pin this run asserts -- except K=1, which IS the
  // loop30 single-iteration filter and must reproduce its bars in-harness
  // (0.963x var, 0.577x raw).
  for (const ChainCase& c : cases) {
    const double m = luma_mse(c.img);
    std::printf("LoopT2 var-update spike: %s K=2 k=%.1f MSE=%.6e (var %.2fx)\n",
                c.leg, static_cast<double>(c.k), m, var_mse / m);
  }
  double upd_best = 1e30;
  double stale_best = 1e30;
  for (const ChainCase& c : cases) {
    const double m = luma_mse(c.img);
    if (std::strcmp(c.leg, "updated") == 0) upd_best = std::min(upd_best, m);
    if (std::strcmp(c.leg, "stale") == 0) stale_best = std::min(stale_best, m);
  }
  const double k1_mse = luma_mse(k1_img);
  std::printf("LoopT2 var-update spike: raw %.6e var %.6e K=1 %.6e; best "
              "updated %.6e (%.2fx K=1, %.2fx raw) vs best stale %.6e\n",
              raw_mse, var_mse, k1_mse, upd_best, k1_mse / upd_best,
              raw_mse / upd_best, stale_best);
  EXPECT_LT(k1_mse, raw_mse * 0.70)
      << "K=1 stopped reproducing the single-iteration bar: " << k1_mse
      << " vs raw " << raw_mse;
  EXPECT_LT(k1_mse, var_mse * 0.99)
      << "K=1 stopped reproducing the single-iteration bar: K=1 " << k1_mse
      << " vs variance-only " << var_mse;

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  var_pipe.cleanup(ctx.device());
  edge_pipe.cleanup(ctx.device());
  smooth_pipe.cleanup(ctx.device());
  var_mgr.cleanup();
  edge_mgr.cleanup();
  smooth_mgr.cleanup();
  for (OutBuf* o : {&var_out, &ping, &pong, &uni_geo, &zero_geo, &e2sm}) {
    vkDestroyImageView(ctx.device(), o->view, nullptr);
    vkDestroyImage(ctx.device(), o->img, nullptr);
    alloc.destroy_allocation(o->mem);
    auto b = o->buf;
    alloc.destroy_allocation(b);
  }
  alloc.destroy_allocation(mbuf);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

//! T2 render-path consumer: the tracer's record_denoise() (count-sourced
//! edge-stopped a-trous) must reproduce the spike shader bit-exactly on a
//! uniform accumulation -- the new shader differs from
//! denoise_edge_atrous.comp ONLY in the divisor source (count image vs
//! scalar push), so identical inputs must give identical texels. The
//! old-shader dispatch below is the equivalence instrument, not a second
//! pin: it runs the frozen spike shader with the baked widths
//! (k=2.0/sigma=1.5/ns=0.1/ds=1.0, the engine's baked values) over the
//! same 4spp accumulation and the outputs must be bit-identical.
TEST(path_tracing_real, denoise_render_path_uniform_parity) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtDenoisePath", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kNoisyFrames = 4U;
  constexpr std::uint32_t kRefFrames = 128U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  cfg.sky.mode = 1U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto submit_wait = [&](VkCommandBuffer cb) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
  };
  const auto record_rounds = [&](std::uint32_t frames) {
    for (std::uint32_t r = 0; r < frames; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_wait(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    }
  };
  const auto record_denoise_once = [&]() {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    tracer.record_denoise(cb);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };

  record_rounds(kNoisyFrames);
  EXPECT_EQ(tracer.sample_count(), kNoisyFrames);
  // Denoise disabled by default: a fresh tracer tonemaps the raw
  // accumulation, exactly the pre-consumer behaviour.
  EXPECT_FALSE(tracer.denoise_enabled());
  tracer.set_denoise_enabled(true);
  EXPECT_TRUE(tracer.denoise_enabled());
  record_denoise_once();
  auto denoised = tracer.read_denoised();
  ASSERT_TRUE(denoised.is_ok());

  // Old-shader equivalence dispatch: the frozen spike shader with the
  // baked widths over the same accumulation.
  std::vector<float> old_img;
  {
    VkImage out_img{VK_NULL_HANDLE};
    VkImageView out_view{VK_NULL_HANDLE};
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ii.extent = {side, side, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ASSERT_EQ(vkCreateImage(ctx.device(), &ii, nullptr, &out_img), VK_SUCCESS);
    auto mem = alloc.bind_image(out_img, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT_TRUE(mem.is_ok());
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ASSERT_EQ(vkCreateImageView(ctx.device(), &vi, nullptr, &out_view),
              VK_SUCCESS);
    auto buf = alloc.create_buffer(
        static_cast<VkDeviceSize>(side) * side * 16U,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ASSERT_TRUE(buf.is_ok());
    omnicpp::render::VulkanDescriptorManager mgr;
    ASSERT_TRUE(mgr.initialize(ctx.device()).is_ok());
    std::vector<omnicpp::render::ReflectedBinding> b(4);
    for (std::uint32_t k = 0; k < 4U; ++k) {
      b[k] = {0, k, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              VK_SHADER_STAGE_COMPUTE_BIT};
    }
    auto layout = mgr.create_layout(b, 1);
    ASSERT_TRUE(layout.is_ok());
    auto set = mgr.allocate_set(layout.value());
    ASSERT_TRUE(set.is_ok());
    ASSERT_TRUE(mgr.write_image(set.value(), 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                VK_NULL_HANDLE, tracer.accum_view(),
                                VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    ASSERT_TRUE(mgr.write_image(set.value(), 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                VK_NULL_HANDLE, tracer.moment_view(),
                                VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    ASSERT_TRUE(mgr.write_image(set.value(), 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                VK_NULL_HANDLE, tracer.gbuffer_view(),
                                VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    ASSERT_TRUE(mgr.write_image(set.value(), 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL)
                    .is_ok());
    omnicpp::render::VulkanPipeline pipe;
    const std::string shader_dir = WARPLOOM_TEST_SHADER_DIR;
    ASSERT_TRUE(pipe.load_shader_stage_file(
        ctx.device(), shader_dir + "/denoise_edge_atrous.comp.spv", "compute")
                    .is_ok());
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 36U};
    ASSERT_TRUE(pipe.create_pipeline_layout(ctx.device(), &layout.value(), 1, &push)
                    .is_ok());
    ASSERT_TRUE(pipe.create_compute_pipeline(ctx.device(), pipe.pipeline_layout())
                    .is_ok());
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier pres[3] = {pre, pre, pre};
    pres[0].image = tracer.accum_image();
    pres[1].image = tracer.moment_image();
    pres[2].image = tracer.gbuffer_image();
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 3, pres);
    VkImageMemoryBarrier prep{};
    prep.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prep.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    prep.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    prep.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    prep.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prep.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prep.image = out_img;
    prep.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &prep);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline());
    VkDescriptorSet ds = set.value();
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe.pipeline_layout(), 0, 1, &ds, 0, nullptr);
    struct OldPush {
      std::uint32_t w, h;
      float kk, sigma, nsigma, dsigma;
      std::uint32_t mean_n, moment_n, stride;
    };
    OldPush p{side, side, 2.0F, 1.5F, 0.1F, 1.0F, kNoisyFrames, kNoisyFrames,
              1U};
    vkCmdPushConstants(cb, pipe.pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(OldPush), &p);
    vkCmdDispatch(cb, (side + 7U) / 8U, (side + 7U) / 8U, 1);
    VkImageMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = out_img;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &post);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {side, side, 1};
    vkCmdCopyImageToBuffer(cb, out_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf.value().buffer, 1, &region);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    old_img.assign(static_cast<std::size_t>(side) * side * 4U, 0.0F);
    std::memcpy(old_img.data(), buf.value().mapped,
                old_img.size() * sizeof(float));
    pipe.cleanup(ctx.device());
    mgr.cleanup();
    vkDestroyImageView(ctx.device(), out_view, nullptr);
    vkDestroyImage(ctx.device(), out_img, nullptr);
    auto keep_mem = mem.value();
    alloc.destroy_allocation(keep_mem);
    auto bb = buf.value();
    alloc.destroy_allocation(bb);
  }
  // THE parity: count-sourced divisors == scalar divisors on uniform input.
  EXPECT_EQ(std::memcmp(denoised.value().data(), old_img.data(),
                        old_img.size() * sizeof(float)),
            0)
      << "render-path denoiser diverged from the spike shader on uniform input";

  // Purity: re-dispatching over unchanged inputs reproduces bit-exactly.
  record_denoise_once();
  auto denoised2 = tracer.read_denoised();
  ASSERT_TRUE(denoised2.is_ok());
  EXPECT_EQ(std::memcmp(denoised.value().data(), denoised2.value().data(),
                        denoised2.value().size() * sizeof(float)),
            0)
      << "record_denoise is not a pure function of its inputs";

  // Follows inputs: one more round moves the accumulation, so the filter
  // must move too (a frozen/stale output would match the first dispatch).
  record_rounds(1U);
  record_denoise_once();
  auto denoised3 = tracer.read_denoised();
  ASSERT_TRUE(denoised3.is_ok());
  EXPECT_NE(std::memcmp(denoised.value().data(), denoised3.value().data(),
                        denoised3.value().size() * sizeof(float)),
            0)
      << "denoised output did not follow the accumulation";

  // Efficacy: the loop30 pin (< raw*0.70, measured 0.577x) through the
  // engine path, not the spike's inline dispatch.
  tracer.reset_accumulation();
  record_rounds(kRefFrames);
  auto ref = tracer.read_mean();
  ASSERT_TRUE(ref.is_ok());
  tracer.reset_accumulation();
  record_rounds(kNoisyFrames);
  record_denoise_once();
  auto den = tracer.read_denoised();
  ASSERT_TRUE(den.is_ok());
  auto raw4 = tracer.read_mean();
  ASSERT_TRUE(raw4.is_ok());
  constexpr double w[3] = {0.2126, 0.7152, 0.0722};
  const auto luma_mse = [&](const std::vector<float>& img) {
    const std::size_t texels = ref.value().size() / 4U;
    double sse = 0.0;
    for (std::size_t t = 0; t < texels; ++t) {
      double a = 0.0;
      double b = 0.0;
      for (std::size_t c = 0; c < 3U; ++c) {
        a += w[c] * static_cast<double>(img[t * 4U + c]);
        b += w[c] * static_cast<double>(ref.value()[t * 4U + c]);
      }
      const double d = a - b;
      sse += d * d;
    }
    return sse / static_cast<double>(texels);
  };
  const double raw_mse = luma_mse(raw4.value());
  const double den_mse = luma_mse(den.value());
  std::printf("LoopT2 denoise path: raw=%.6e denoised=%.6e (raw %.2fx)\n",
              raw_mse, den_mse, raw_mse / den_mse);
  EXPECT_LT(den_mse, raw_mse * 0.70)
      << "render-path denoiser stopped earning its keep";

  // Re-entrancy: the viewport resize path calls create() on a LIVE tracer.
  // A cleanup that leaves a dangling compute-shader handle behind refuses
  // the rebuild here (has_stage sees the stale handle) -- the exact
  // failure that killed the live e1 resize before the fix. The rebuilt
  // stack must be usable, including the denoiser.
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  EXPECT_TRUE(tracer.ready());
  tracer.set_camera(cam);
  tracer.set_denoise_enabled(true);
  record_rounds(kNoisyFrames);
  record_denoise_once();
  auto den_rebuilt = tracer.read_denoised();
  ASSERT_TRUE(den_rebuilt.is_ok());
  // Same calls, same image: the rebuilt stack reproduces the first
  // dispatch bit-exactly (the seed is a pure function of (pixel, frame)).
  EXPECT_EQ(std::memcmp(den.value().data(), den_rebuilt.value().data(),
                        den_rebuilt.value().size() * sizeof(float)),
            0)
      << "rebuilt tracer diverged from the first build";

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

//! T2 render-path consumer under adaptive masking: the count-sourced
//! divisors must keep held texels exact through the FILTER, not just
//! through the host readback. After a 4-uniform + mask(0.25) + 4-masked
//! sequence, record_denoise() filters the per-texel mean; the denoised
//! MSE must beat the adaptive-raw MSE (the filter earns its keep on the
//! masked stack too), true geometry must beat uniform-overwritten
//! geometry (binding 4's counts and binding 2's edges are both live),
//! and an all-held zero stack must filter to exact zeros (no garbage
//! from the output image's first layout transition).
TEST(path_tracing_real, denoise_render_path_adaptive) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtDenoiseAdapt", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 64U;
  constexpr std::uint32_t kPrefix = 4U;
  constexpr std::uint32_t kMasked = 4U;
  constexpr std::uint32_t kRefFrames = 128U;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  cfg.sky.mode = 1U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto submit_wait = [&](VkCommandBuffer cb) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, VK_NULL_HANDLE),
              VK_SUCCESS);
    EXPECT_EQ(vkQueueWaitIdle(ctx.graphics_queue()), VK_SUCCESS);
  };
  const auto record_rounds = [&](std::uint32_t frames, bool clear_first) {
    for (std::uint32_t r = 0; r < frames; ++r) {
      auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
          ctx.device(), pool.value());
      EXPECT_TRUE(cbr.is_ok());
      if (!cbr.is_ok()) return;
      VkCommandBuffer cb = cbr.value();
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
      tracer.record_round(cb, r, r == 0U && clear_first);
      EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
      submit_wait(cb);
      vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
    }
  };
  const auto record_denoise_once = [&]() {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    tracer.record_denoise(cb);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  };

  record_rounds(kPrefix, true);
  auto mean = tracer.read_mean();
  auto moment = tracer.read_second_moment();
  ASSERT_TRUE(mean.is_ok() && moment.is_ok());
  const auto mask =
      omnicpp::render::adaptive_mask_from_variance(mean.value(),
                                                   moment.value(), 0.25);
  EXPECT_TRUE(tracer.upload_adaptive_mask(mask.mask));
  tracer.set_adaptive_enabled(true);
  record_rounds(kMasked, false);
  auto adapt_raw = tracer.read_adaptive_mean();
  ASSERT_TRUE(adapt_raw.is_ok());

  tracer.set_denoise_enabled(true);
  record_denoise_once();
  auto den_true = tracer.read_denoised();
  ASSERT_TRUE(den_true.is_ok());

  // Geometry-live sabotage: overwrite the G-buffer with uniform geometry
  // (no edges to stop on), over the SAME adaptive stack the true-geometry
  // dispatch just filtered. A dead geometry binding returns the true-geo
  // answer here instead; live geometry must change it AND cost MSE.
  // (The overwrite runs before the reference below because the reference
  // re-traces and would replace the stack under test.)
  {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    ASSERT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = tracer.gbuffer_image();
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_transfer);
    VkClearColorValue uni{};
    uni.float32[0] = 0.0F;
    uni.float32[1] = 0.0F;
    uni.float32[2] = 1.0F;
    uni.float32[3] = 1.0F;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, tracer.gbuffer_image(), VK_IMAGE_LAYOUT_GENERAL,
                         &uni, 1, &range);
    VkImageMemoryBarrier to_shader = to_transfer;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_shader);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    submit_wait(cb);
    vkFreeCommandBuffers(ctx.device(), pool.value(), 1U, &cb);
  }
  record_denoise_once();
  auto den_uni = tracer.read_denoised();
  ASSERT_TRUE(den_uni.is_ok());
  EXPECT_NE(std::memcmp(den_true.value().data(), den_uni.value().data(),
                        den_uni.value().size() * sizeof(float)),
            0)
      << "uniform geometry identical to true: geometry binding is decorative";

  // Reference for the MSEs (re-traced after the sabotage, so the stack
  // under test above is untouched by it).
  tracer.set_adaptive_enabled(false);
  tracer.reset_accumulation();
  record_rounds(kRefFrames, true);
  auto ref = tracer.read_mean();
  ASSERT_TRUE(ref.is_ok());
  constexpr double w[3] = {0.2126, 0.7152, 0.0722};
  const auto luma_mse = [&](const std::vector<float>& img) {
    const std::size_t texels = ref.value().size() / 4U;
    double sse = 0.0;
    for (std::size_t t = 0; t < texels; ++t) {
      double a = 0.0;
      double b = 0.0;
      for (std::size_t c = 0; c < 3U; ++c) {
        a += w[c] * static_cast<double>(img[t * 4U + c]);
        b += w[c] * static_cast<double>(ref.value()[t * 4U + c]);
      }
      const double d = a - b;
      sse += d * d;
    }
    return sse / static_cast<double>(texels);
  };
  const double adapt_raw_mse = luma_mse(adapt_raw.value());
  const double den_true_mse = luma_mse(den_true.value());
  const double den_uni_mse = luma_mse(den_uni.value());
  std::printf("LoopT2 denoise adaptive: raw-adaptive=%.6e denoised=%.6e "
              "uniform-geometry=%.6e\n",
              adapt_raw_mse, den_true_mse, den_uni_mse);
  // Measured 2026-11-06 on the RTX 2060 (64x64 box, 4-uniform + mask(0.25)
  // + 4-masked): raw-adaptive 2.313659e-03 (held texels dominate), denoised
  // 1.334325e-03 (0.577x raw -- the same 1.73x the uniform spike measured,
  // to 3e-9), uniform-geometry 1.385475e-03 (collapses onto variance-only
  // 1.385478e-03, the loop30 geometry-blind equivalence through the new
  // shader). Pins mirror loop30's margins: 0.70 on raw, 0.99 on the
  // geometry-blind answer (measured 0.963, the same 4% depth-stop gain).
  EXPECT_LT(den_true_mse, adapt_raw_mse * 0.70)
      << "denoiser stopped earning its keep on the masked stack";
  EXPECT_LT(den_true_mse, den_uni_mse * 0.99)
      << "edge stops earned nothing over geometry-blind filtering";

  // Zero-state: an all-held zero stack filters to exact zeros -- the
  // output image's first layout transition leaves no garbage texels, and
  // max(count, 1) keeps the 0/0 texels defined.
  tracer.reset_accumulation();
  EXPECT_TRUE(tracer.upload_adaptive_mask(
      std::vector<std::uint32_t>(static_cast<std::size_t>(side) * side, 0U)));
  tracer.set_adaptive_enabled(true);
  record_rounds(2U, true);
  record_denoise_once();
  auto den_zero = tracer.read_denoised();
  ASSERT_TRUE(den_zero.is_ok());
  for (std::size_t i = 0; i < den_zero.value().size(); ++i) {
    EXPECT_EQ(den_zero.value()[i], 0.0F) << "nonzero denoised texel " << i;
  }

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

//! T2 adaptive build: masked re-trace with per-pixel counts.
//!
//! After a uniform prefix, a variance mask (top 25%) selects texels for
//! refinement. Masked rounds must (a) leave held texels bit-identical to
//! the prefix mean, (b) carry per-pixel counts 4 vs 8, and (c) keep the
//! uniform path's counts at sample_count() exactly. Sabotage-checked both
//! directions: a wrong-size upload fails, and disabling the flag refines
//! everything (held counts advance to 8).
TEST(path_tracing_real, adaptive_masked_rounds_hold_unmasked_bit_exact) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtAdaptive", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 32U;
  const std::size_t texels = static_cast<std::size_t>(side) * side;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto run_round = [&](std::uint32_t frame, bool clear) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    tracer.record_round(cb, frame, clear);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    VkFence fence{};
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    EXPECT_EQ(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX),
              VK_SUCCESS);
    vkDestroyFence(ctx.device(), fence, nullptr);
  };

  tracer.reset_accumulation();
  for (std::uint32_t r = 0; r < 4U; ++r) run_round(r, r == 0U);

  // Uniform prefix carries per-pixel counts == sample_count() exactly.
  auto counts4 = tracer.read_per_pixel_counts();
  ASSERT_TRUE(counts4.is_ok());
  ASSERT_EQ(counts4.value().size(), texels);
  for (std::uint32_t c : counts4.value()) EXPECT_EQ(c, 4U);

  auto m4 = tracer.read_mean();
  auto e2_4 = tracer.read_second_moment();
  ASSERT_TRUE(m4.is_ok());
  ASSERT_TRUE(e2_4.is_ok());
  const auto mask =
      omnicpp::render::adaptive_mask_from_variance(m4.value(), e2_4.value(), 0.25);
  ASSERT_EQ(mask.mask.size(), texels);
  EXPECT_GT(mask.selected, 0U);
  EXPECT_LT(mask.selected, texels)
      << "variance did not concentrate on the 32x32 box scene";
  EXPECT_GE(mask.threshold, 0.0F);
  // Sabotage direction 1: a wrong-size upload must fail and leave the
  // device mask untouched (all zeros from create).
  EXPECT_FALSE(tracer.upload_adaptive_mask(std::vector<std::uint32_t>(7U, 1U)));
  EXPECT_TRUE(tracer.upload_adaptive_mask(mask.mask));
  tracer.set_adaptive_enabled(true);

  for (std::uint32_t r = 4U; r < 8U; ++r) run_round(r, false);

  auto counts8 = tracer.read_per_pixel_counts();
  ASSERT_TRUE(counts8.is_ok());
  auto adapt = tracer.read_adaptive_mean();
  ASSERT_TRUE(adapt.is_ok());
  std::size_t held = 0U;
  std::size_t refined = 0U;
  for (std::size_t t = 0; t < texels; ++t) {
    if (mask.mask[t] == 0U) {
      EXPECT_EQ(counts8.value()[t], 4U) << "held texel " << t;
      for (std::size_t c = 0; c < 3U; ++c)
        EXPECT_FLOAT_EQ(adapt.value()[t * 4U + c], m4.value()[t * 4U + c])
            << "held texel " << t << " channel " << c;
      ++held;
    } else {
      EXPECT_EQ(counts8.value()[t], 8U) << "refined texel " << t;
      ++refined;
    }
  }
  EXPECT_EQ(held + refined, texels);
  EXPECT_EQ(refined, mask.selected);
  std::printf("LoopT2 adaptive: texels=%zu selected=%zu threshold=%.6e\n",
              texels, mask.selected, static_cast<double>(mask.threshold));

  // Sabotage direction 2: with the flag off, extra rounds refine everything
  // (held counts advance), so masking is load-bearing, not decorative.
  tracer.set_adaptive_enabled(false);
  run_round(8U, false);
  auto counts9 = tracer.read_per_pixel_counts();
  ASSERT_TRUE(counts9.is_ok());
  for (std::size_t t = 0; t < texels; ++t) {
    const std::uint32_t want = mask.mask[t] == 0U ? 5U : 9U;
    EXPECT_EQ(counts9.value()[t], want) << "texel " << t;
  }

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

//! T2 adaptive build, second generation: iterative re-masking.
//!
//! One-shot masking (previous test) freezes the FIRST ranking: texels that
//! are quiet at 4spp but noisy at 8spp never get selected. Re-masking from
//! the refined state re-ranks: the second mask follows the CURRENT noise.
//! This needs no engine change (upload + enable are re-callable; the mask
//! buffer is host-coherent) -- it is a host-side loop over existing
//! primitives, proven here headlessly.
//!
//! Two generations on the 32x32 box scene: 4 uniform + mask1(25%) + 4
//! masked + re-mask from the refined state + mask2(25%) + 4 masked.
//! Per-texel history classes pin the counts (HH=4, HR/RH=8, RR=12);
//! double-held texels keep the prefix mean bit-exactly and
//! refined-then-held texels keep the gen-1 mean bit-exactly (same hold
//! contract as the one-shot test, extended across a re-rank). A same-tracer
//! reset + full repeat must reproduce means AND counts bit-exactly (replay
//! is a pure function of the call sequence). Sabotage-checked: an
//! all-zeros re-upload freezes every count and an all-ones re-upload
//! advances every count, so the device mask tracks the LATEST upload in
//! both directions rather than sticking on the first.
TEST(path_tracing_real, adaptive_iterative_remask_refines_new_texels) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtReMask", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 32U;
  const std::size_t texels = static_cast<std::size_t>(side) * side;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  const std::vector<float> box = {
      -0.5F, -0.5F, -0.5F,  -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,  -0.5F, -0.5F, -0.5F,
      -0.5F, -0.5F, 0.5F,  -0.5F, 0.5F, 0.5F,   -0.5F, 0.5F, -0.5F,
      -0.5F, 0.5F, 0.5F,   -0.5F, -0.5F, 0.5F,  -0.5F, -0.5F, -0.5F};
  auto geo = alloc.create_buffer(box.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, box.data(), box.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  omnicpp::render::PathTracerMesh cube{};
  cube.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  cube.triangle_count = 12U;
  cube.max_vertex = 35U;
  cfg.meshes = {cube};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 0.55F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto run_round = [&](std::uint32_t frame, bool clear) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    tracer.record_round(cb, frame, clear);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    VkFence fence{};
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    EXPECT_EQ(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX),
              VK_SUCCESS);
    vkDestroyFence(ctx.device(), fence, nullptr);
  };
  const auto current_mask = [&](double frac) {
    auto m = tracer.read_mean();
    auto e = tracer.read_second_moment();
    EXPECT_TRUE(m.is_ok());
    EXPECT_TRUE(e.is_ok());
    return omnicpp::render::adaptive_mask_from_variance(m.value(), e.value(),
                                                        frac);
  };

  // Generation 0: uniform prefix.
  tracer.reset_accumulation();
  for (std::uint32_t r = 0; r < 4U; ++r) run_round(r, r == 0U);
  auto m4 = tracer.read_mean();
  ASSERT_TRUE(m4.is_ok());

  // Generation 1: mask from the prefix, 4 masked rounds.
  const auto mask1 =
      omnicpp::render::adaptive_mask_from_variance(m4.value(),
                                                   tracer.read_second_moment().value(),
                                                   0.25);
  ASSERT_EQ(mask1.mask.size(), texels);
  EXPECT_EQ(mask1.selected, 256U)
      << "rank budget must be exact: ceil(0.25 * 1024)";
  ASSERT_TRUE(tracer.upload_adaptive_mask(mask1.mask));
  tracer.set_adaptive_enabled(true);
  for (std::uint32_t r = 4U; r < 8U; ++r) run_round(r, false);
  auto a8 = tracer.read_adaptive_mean();
  ASSERT_TRUE(a8.is_ok());

  // Generation 2: re-mask from the REFINED state, 4 more masked rounds.
  const auto mask2 = current_mask(0.25);
  ASSERT_EQ(mask2.mask.size(), texels);
  EXPECT_EQ(mask2.selected, 256U) << "re-ranked budget must stay exact";
  std::size_t overlap = 0U;
  for (std::size_t t = 0; t < texels; ++t)
    if (mask1.mask[t] != 0U && mask2.mask[t] != 0U) ++overlap;
  std::printf("LoopT2 remask: gen1=%zu gen2=%zu overlap=%zu new=%zu\n",
              mask1.selected, mask2.selected, overlap,
              mask2.selected - overlap);
  // Iteration must re-rank onto NEW texels: measured overlap 0 (gen-2
  // picks the complementary noisy half). A stale re-upload of mask1 would
  // read overlap 256 here... but overlap is computed from the masks, so
  // this pin guards the RANKING; the history-class counts below guard the
  // upload (stale mask1 fails the RH count 12 != 8 -- sabotage-checked).
  EXPECT_LT(overlap, mask2.selected)
      << "re-masking re-selected the same set: iteration adds nothing";
  // Non-vacuity: HR and RH must both occur, or their bit-exact loops
  // check nothing. (RR is empty on this scene -- overlap 0 -- so the RR
  // branch documents the 4+4+4 contract without executing; the +1
  // instruction it relies on is proven by the all-ones round below and
  // HR/RH prove two selections accumulate.)
  ASSERT_TRUE(tracer.upload_adaptive_mask(mask2.mask));
  for (std::uint32_t r = 8U; r < 12U; ++r) run_round(r, false);
  auto counts12 = tracer.read_per_pixel_counts();
  auto a12 = tracer.read_adaptive_mean();
  ASSERT_TRUE(counts12.is_ok());
  ASSERT_TRUE(a12.is_ok());

  // History classes: HH=4, HR/RH=8, RR=12. Double-held keeps the prefix
  // mean; refined-then-held keeps the gen-1 mean -- both bit-exact.
  std::size_t n_hh = 0U, n_hr = 0U, n_rh = 0U, n_rr = 0U;
  for (std::size_t t = 0; t < texels; ++t) {
    const bool in1 = mask1.mask[t] != 0U;
    const bool in2 = mask2.mask[t] != 0U;
    if (!in1 && !in2) {
      EXPECT_EQ(counts12.value()[t], 4U) << "HH texel " << t;
      for (std::size_t c = 0; c < 3U; ++c)
        EXPECT_FLOAT_EQ(a12.value()[t * 4U + c], m4.value()[t * 4U + c])
            << "HH texel " << t << " channel " << c;
      ++n_hh;
    } else if (!in1 && in2) {
      EXPECT_EQ(counts12.value()[t], 8U) << "HR texel " << t;
      ++n_hr;
    } else if (in1 && !in2) {
      EXPECT_EQ(counts12.value()[t], 8U) << "RH texel " << t;
      for (std::size_t c = 0; c < 3U; ++c)
        EXPECT_FLOAT_EQ(a12.value()[t * 4U + c], a8.value()[t * 4U + c])
            << "RH texel " << t << " channel " << c;
      ++n_rh;
    } else {
      EXPECT_EQ(counts12.value()[t], 12U) << "RR texel " << t;
      ++n_rr;
    }
  }
  EXPECT_EQ(n_hh + n_hr + n_rh + n_rr, texels);
  EXPECT_GT(n_hr, 0U) << "HR class empty: held-then-refined unproven";
  EXPECT_GT(n_rh, 0U) << "RH class empty: refined-then-held unproven";
  std::printf("LoopT2 remask: HH=%zu HR=%zu RH=%zu RR=%zu\n", n_hh, n_hr,
              n_rh, n_rr);

  // Sabotage, both directions of the upload wire: all-zeros freezes every
  // count, all-ones advances every count -- the device mask tracks the
  // LATEST upload, not the first.
  ASSERT_TRUE(tracer.upload_adaptive_mask(std::vector<std::uint32_t>(texels, 0U)));
  run_round(12U, false);
  auto counts_frozen = tracer.read_per_pixel_counts();
  ASSERT_TRUE(counts_frozen.is_ok());
  EXPECT_EQ(std::memcmp(counts_frozen.value().data(),
                        counts12.value().data(),
                        texels * sizeof(std::uint32_t)),
            0)
      << "all-zeros upload must freeze every count";
  ASSERT_TRUE(tracer.upload_adaptive_mask(std::vector<std::uint32_t>(texels, 1U)));
  run_round(13U, false);
  auto counts_all = tracer.read_per_pixel_counts();
  ASSERT_TRUE(counts_all.is_ok());
  for (std::size_t t = 0; t < texels; ++t)
    EXPECT_EQ(counts_all.value()[t], counts_frozen.value()[t] + 1U)
        << "texel " << t;

  // Determinism: same tracer, reset, full two-generation repeat -- means
  // AND counts bit-identical (replay is a pure function of the calls).
  tracer.set_adaptive_enabled(false);
  tracer.reset_accumulation();
  for (std::uint32_t r = 0; r < 4U; ++r) run_round(r, r == 0U);
  const auto rep1 = current_mask(0.25);
  EXPECT_EQ(rep1.mask, mask1.mask) << "gen-1 mask must re-rank identically";
  ASSERT_TRUE(tracer.upload_adaptive_mask(rep1.mask));
  tracer.set_adaptive_enabled(true);
  for (std::uint32_t r = 4U; r < 8U; ++r) run_round(r, false);
  const auto rep2 = current_mask(0.25);
  EXPECT_EQ(rep2.mask, mask2.mask) << "gen-2 mask must re-rank identically";
  ASSERT_TRUE(tracer.upload_adaptive_mask(rep2.mask));
  for (std::uint32_t r = 8U; r < 12U; ++r) run_round(r, false);
  auto rep_counts = tracer.read_per_pixel_counts();
  auto rep_mean = tracer.read_adaptive_mean();
  ASSERT_TRUE(rep_counts.is_ok());
  ASSERT_TRUE(rep_mean.is_ok());
  EXPECT_EQ(std::memcmp(rep_counts.value().data(), counts12.value().data(),
                        texels * sizeof(std::uint32_t)),
            0)
      << "iterative counts are not a pure function of the call sequence";
  EXPECT_EQ(std::memcmp(rep_mean.value().data(), a12.value().data(),
                        a12.value().size() * sizeof(float)),
            0)
      << "iterative means are not a pure function of the call sequence";

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

//! T2 G-buffer emission: first-hit normal + depth overwritten per round.
//!
//! One isolated triangle in the void: hit texels must carry a unit
//! faceforward normal (here exactly +Z) and a positive t, miss texels the
//! exact clear encoding (0,0,0,-1) -- cross-checked against the independent
//! radiance signal (miss == white sky, hit <= grey albedo). A second
//! identical frame-0 round must reproduce the image bit-exactly (the
//! G-buffer is a pure function of pose + frame seed, which replay needs).
//! Sabotage-checked: deleting the rgen store leaves the clear value
//! everywhere, so no hit texel can pass.
TEST(path_tracing_real, gbuffer_first_hit_normal_depth) {
  omnicpp::render::VulkanContext ctx;
  ASSERT_TRUE(ctx.initialize("OmniCppPtGbuffer", true).is_ok());
  if (!ctx.has_ray_tracing()) GTEST_SKIP() << "device lacks ray tracing";
  const std::uint32_t qf =
      static_cast<std::uint32_t>(ctx.queue_families().graphics_family);
  const std::uint32_t side = 32U;
  const std::size_t texels = static_cast<std::size_t>(side) * side;

  omnicpp::render::VulkanMemoryAllocator alloc;
  ASSERT_TRUE(alloc.initialize(ctx.device(), ctx.physical_device()).is_ok());

  // Single triangle at z=0 facing +Z (soup: 3 verts, 1 tri).
  const std::vector<float> tri = {-2.0F, -2.0F, 0.0F, 2.0F, -2.0F, 0.0F,
                                  0.0F, 2.0F, 0.0F};
  auto geo = alloc.create_buffer(tri.size() * sizeof(float),
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(geo.is_ok());
  std::memcpy(geo.value().mapped, tri.data(), tri.size() * sizeof(float));
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = geo.value().buffer;

  omnicpp::render::PathTracerConfig cfg{};
  cfg.shader_dir = WARPLOOM_TEST_SHADER_DIR;
  cfg.width = side;
  cfg.height = side;
  cfg.max_bounces = 4U;
  omnicpp::render::PathTracerMesh mesh{};
  mesh.vertex_buffer_address = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(ctx.device(), &ai));
  mesh.triangle_count = 1U;
  mesh.max_vertex = 2U;
  cfg.meshes = {mesh};
  cfg.instances = {
      omnicpp::render::PathTracerInstance::translated({0.0F, 0.0F, 0.0F}, 1.0F)};

  omnicpp::render::VulkanPathTracer tracer;
  ASSERT_TRUE(tracer.create(ctx.physical_device(), ctx.device(),
                            ctx.graphics_queue(), qf, alloc, cfg)
                  .is_ok());
  omnicpp::render::PathTracerCamera cam{};
  cam.position = {0.0F, 0.6F, 2.6F};
  cam.forward = {0.0F, -0.2F, -1.0F};
  cam.right = {1.0F, 0.0F, 0.0F};
  cam.up = {0.0F, 1.0F, 0.0F};
  tracer.set_camera(cam);

  auto pool = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool.is_ok());
  const auto run_round = [&](std::uint32_t frame, bool clear) {
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), pool.value());
    EXPECT_TRUE(cbr.is_ok());
    if (!cbr.is_ok()) return;
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &bi), VK_SUCCESS);
    tracer.record_round(cb, frame, clear);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    VkFence fence{};
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx.device(), &fi, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1U;
    si.pCommandBuffers = &cb;
    EXPECT_EQ(vkQueueSubmit(ctx.graphics_queue(), 1, &si, fence), VK_SUCCESS);
    EXPECT_EQ(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX),
              VK_SUCCESS);
    vkDestroyFence(ctx.device(), fence, nullptr);
  };

  tracer.reset_accumulation();
  run_round(0U, true);
  auto mean = tracer.read_mean();
  auto gbuf = tracer.read_gbuffer();
  ASSERT_TRUE(mean.is_ok());
  ASSERT_TRUE(gbuf.is_ok());
  ASSERT_EQ(gbuf.value().size(), texels * 4U);

  std::size_t hits = 0U;
  std::size_t misses = 0U;
  for (std::size_t t = 0; t < texels; ++t) {
    const float nx = gbuf.value()[t * 4U + 0U];
    const float ny = gbuf.value()[t * 4U + 1U];
    const float nz = gbuf.value()[t * 4U + 2U];
    const float tt = gbuf.value()[t * 4U + 3U];
    const float mr = mean.value()[t * 4U + 0U];
    if (tt >= 0.0F) {
      ++hits;
      // Unit faceforward normal, here exactly +Z (flat triangle), positive t.
      const double len = std::sqrt(static_cast<double>(nx * nx + ny * ny + nz * nz));
      EXPECT_NEAR(len, 1.0, 1e-5) << "texel " << t;
      EXPECT_GT(nz, 0.9F) << "texel " << t;
      EXPECT_GT(tt, 0.0F) << "texel " << t;
      EXPECT_LT(tt, 100.0F) << "texel " << t;
      // Independent signal: a hit attenuates by the grey albedo at least once.
      EXPECT_LT(mr, 0.9F) << "texel " << t;
      EXPECT_GT(mr, 0.0F) << "texel " << t;
    } else {
      ++misses;
      EXPECT_FLOAT_EQ(nx, 0.0F) << "texel " << t;
      EXPECT_FLOAT_EQ(ny, 0.0F) << "texel " << t;
      EXPECT_FLOAT_EQ(nz, 0.0F) << "texel " << t;
      EXPECT_FLOAT_EQ(tt, -1.0F) << "texel " << t;
      // Independent signal: a miss is exactly the constant white sky.
      EXPECT_FLOAT_EQ(mr, 1.0F) << "texel " << t;
      EXPECT_FLOAT_EQ(mean.value()[t * 4U + 1U], 1.0F) << "texel " << t;
      EXPECT_FLOAT_EQ(mean.value()[t * 4U + 2U], 1.0F) << "texel " << t;
    }
  }
  EXPECT_GT(hits, 0U) << "triangle covers nothing: test is vacuous";
  EXPECT_GT(misses, 0U) << "triangle covers everything: miss path untested";
  std::printf("LoopT2 gbuffer: texels=%zu hits=%zu misses=%zu\n", texels, hits,
              misses);

  // Determinism: the same frame-0 round reproduces the image bit-exactly.
  tracer.reset_accumulation();
  run_round(0U, true);
  auto gbuf2 = tracer.read_gbuffer();
  ASSERT_TRUE(gbuf2.is_ok());
  EXPECT_EQ(std::memcmp(gbuf.value().data(), gbuf2.value().data(),
                        gbuf.value().size() * sizeof(float)),
            0)
      << "G-buffer is not a pure function of (pose, frame)";

  EXPECT_EQ(ctx.validation_error_count(), 0U);
  EXPECT_EQ(ctx.validation_warning_count(), 0U);
  tracer.destroy();
  vkDestroyCommandPool(ctx.device(), pool.value(), nullptr);
  auto keep_geo = geo.value();
  alloc.destroy_allocation(keep_geo);
  alloc.cleanup();
  ctx.cleanup();
}

#else  // !WARPLOOM_HAS_VULKAN

TEST(path_tracing_real, disabled_without_vulkan) {
  GTEST_SKIP() << "Vulkan not available";
}

#endif  // WARPLOOM_HAS_VULKAN

// CPU-only: the adaptive mask is a pure function of (mean, second moment).
// No device needed, so this runs everywhere Vulkan does not.
TEST(adaptive_mask, selects_top_fraction_deterministically) {
  // 8 texels: luma variance concentrated in texel 0 (var 1.0), texel 1
  // (var 0.5), rest 0. Top 25% must select exactly {0, 1}.
  std::vector<float> mean(8U * 4U, 0.0F);
  std::vector<float> moment(8U * 4U, 0.0F);
  for (std::size_t c = 0; c < 3U; ++c) {
    mean[c] = 1.0F;
    moment[c] = 2.0F;  // luma var = 2 - 1 = 1
    mean[4U + c] = 0.5F;
    moment[4U + c] = 0.75F;  // luma var = 0.75 - 0.25 = 0.5
  }
  const auto mask =
      omnicpp::render::adaptive_mask_from_variance(mean, moment, 0.25);
  ASSERT_EQ(mask.mask.size(), 8U);
  EXPECT_EQ(mask.selected, 2U);
  EXPECT_EQ(mask.mask[0], 1U);
  EXPECT_EQ(mask.mask[1], 1U);
  for (std::size_t t = 2; t < 8U; ++t) EXPECT_EQ(mask.mask[t], 0U);
  EXPECT_FLOAT_EQ(mask.threshold, 0.5F);
  // Deterministic: same inputs, same mask bit-for-bit.
  const auto mask2 =
      omnicpp::render::adaptive_mask_from_variance(mean, moment, 0.25);
  EXPECT_EQ(mask2.mask, mask.mask);
  // Degenerate inputs select nothing (no crash, no selection).
  EXPECT_EQ(omnicpp::render::adaptive_mask_from_variance({}, {}, 0.25)
                .selected,
            0U);
  EXPECT_EQ(omnicpp::render::adaptive_mask_from_variance(mean, moment, 0.0)
                .selected,
            0U);
  // All-equal variance selects exactly the top-25% budget by rank (ties
  // broken by texel index) -- the budget is exact even when the signal
  // carries no information.
  std::vector<float> flat_mean(4U * 4U, 0.5F);
  std::vector<float> flat_moment(4U * 4U, 0.5F);
  const auto flat =
      omnicpp::render::adaptive_mask_from_variance(flat_mean, flat_moment, 0.25);
  EXPECT_EQ(flat.selected, 1U);
  EXPECT_EQ(flat.mask[0], 1U);
}
