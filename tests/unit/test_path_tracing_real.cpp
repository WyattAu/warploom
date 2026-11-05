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
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20U};
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
    // the noisy depth itself (see the samples word in the shader).
    struct Push {
      std::uint32_t w, h;
      float k, sigma;
      std::uint32_t samples;
    };
    Push p{side, side, k, 1.5F, kNoisyFrames};
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
  auto keep_out = out_buf.value();
  alloc.destroy_allocation(keep_out);
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
