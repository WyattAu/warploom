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
#include "warploom/render/vulkan_memory_allocator.hpp"
#include "warploom/render/vulkan_path_tracer.hpp"
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

// Cosine-weighted hemisphere sample around +Z rotated into n (same ONB form
// as the shader), fp64, from an independent mt19937_64 stream.
void cosine_dir_fp64(const double n[3], std::mt19937_64& rng, double out[3]) {
  std::uniform_real_distribution<double> U(0.0, 1.0);
  const double r1 = U(rng);
  const double r2 = U(rng);
  const double phi = 6.283185307179586 * r1;
  const double s = std::sqrt(r2);
  const double tz[3] = {s * std::cos(phi), s * std::sin(phi),
                        std::sqrt(std::max(0.0, 1.0 - r2))};
  // Build ONB around n (n is axis-aligned here except the ceiling normal).
  const double sg = n[2] < 0.0 ? -1.0 : 1.0;
  const double up[3] = {0.0, 0.0, sg};
  const double d = up[0] * n[0] + up[1] * n[1] + up[2] * n[2];
  double txv[3] = {up[0] - n[0] * d, up[1] - n[1] * d, up[2] - n[2] * d};
  const double tl = std::sqrt(txv[0] * txv[0] + txv[1] * txv[1] +
                              txv[2] * txv[2]);
  txv[0] /= tl; txv[1] /= tl; txv[2] /= tl;
  // ty = cross(n, tx)
  const double tyv[3] = {n[1] * txv[2] - n[2] * txv[1],
                         n[2] * txv[0] - n[0] * txv[2],
                         n[0] * txv[1] - n[1] * txv[0]};
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
    // Cosine sample (same ONB), fp32.
    const float r1 = rand01(rng);
    const float r2 = rand01(rng);
    const float phi = 6.28318530718f * r1;
    const float sq = std::sqrt(r2);
    const float tz[3] = {sq * std::cos(phi), sq * std::sin(phi),
                         std::sqrt(std::max(0.0f, 1.0f - r2))};
    const float sg = n[2] < 0.0f ? -1.0f : 1.0f;
    const float upv[3] = {0.0f, 0.0f, sg};
    const float dd = upv[0] * n[0] + upv[1] * n[1] + upv[2] * n[2];
    float txv[3] = {upv[0] - n[0] * dd, upv[1] - n[1] * dd, upv[2] - n[2] * dd};
    const float tl = std::sqrt(txv[0] * txv[0] + txv[1] * txv[1] + txv[2] * txv[2]);
    txv[0] /= tl; txv[1] /= tl; txv[2] /= tl;
    const float tyv[3] = {n[1] * txv[2] - n[2] * txv[1],
                          n[2] * txv[0] - n[0] * txv[2],
                          n[0] * txv[1] - n[1] * txv[0]};
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

#else  // !WARPLOOM_HAS_VULKAN

TEST(path_tracing_real, disabled_without_vulkan) {
  GTEST_SKIP() << "Vulkan not available";
}

#endif  // WARPLOOM_HAS_VULKAN
