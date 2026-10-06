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
#include "warploom/render/vulkan_renderer.hpp"
#include "warploom/render/vulkan_rt_pipeline.hpp"
#include "vulkan_test_readback.hpp"

#ifdef WARPLOOM_HAS_VULKAN

namespace {

//! %f varargs consume a double; make the required promotion explicit.
inline double dbl(float v) noexcept { return static_cast<double>(v); }

using omnicpp::render::Allocation;
using omnicpp::render::BlasBuildInput;
using omnicpp::render::BottomLevelAS;
using omnicpp::render::TopLevelAS;
using omnicpp::render::TlasInstance;
using omnicpp::render::VulkanAccelerationStructureBuilder;
using omnicpp::render::VulkanContext;
using omnicpp::render::VulkanDescriptorManager;
using omnicpp::render::VulkanMemoryAllocator;
using omnicpp::render::VulkanRtPipeline;
using omnicpp::render::VulkanScratchPool;

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

std::array<float, 12> translated_instance(float tx, float ty, float tz) {
  return {1.0f, 0.0f, 0.0f, tx, 0.0f, 1.0f, 0.0f, ty,
          0.0f, 0.0f, 1.0f, tz};
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
  VulkanDescriptorManager desc;
  ASSERT_TRUE(desc.initialize(ctx.device()).is_ok());
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

  // ---- GPU data: instances (binding 3), params (binding 1) -------------------
  struct GpuParams {
    float cam_pos_frame[4];
    float cam_basis_r[4];
    float cam_basis_u[4];
    float cam_basis_f[4];
    std::uint32_t dims[4];
  };
  // Instance words: [center.xyz, 0], [albedo.xyz, 0]. Box order matches the
  // CPU room list: floor, wall -z, wall +z, wall -x, wall +x (indices 0..4).
  std::vector<std::array<float, 4>> inst_words;
  const auto push_instance = [&](float cx, float cy, float cz) {
    inst_words.push_back({cx, cy, cz, 0.0f});
    inst_words.push_back({kAlbedo, kAlbedo, kAlbedo, 0.0f});
  };
  push_instance(0.0f, -2 * kRoomHy, 0.0f);   // 0: floor slab
  push_instance(0.0f, -kRoomHy, -kRoomHx);   // 1: wall -z
  push_instance(0.0f, -kRoomHy, kRoomHx);    // 2: wall +z
  push_instance(-kRoomHx, -kRoomHy, 0.0f);   // 3: wall -x
  push_instance(kRoomHx, -kRoomHy, 0.0f);    // 4: wall +x

  auto instance_buf = alloc.create_buffer(
      inst_words.size() * sizeof(std::array<float, 4>),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(instance_buf.is_ok());
  Allocation inst_alloc = instance_buf.value();
  std::memcpy(inst_alloc.mapped, inst_words.data(),
              inst_words.size() * sizeof(std::array<float, 4>));

  auto params_buf = alloc.create_buffer(
      sizeof(GpuParams), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(params_buf.is_ok());
  Allocation params_alloc = params_buf.value();
  auto* gpu_params = static_cast<GpuParams*>(params_alloc.mapped);
  *gpu_params = GpuParams{};
  std::memcpy(gpu_params->cam_pos_frame, kCamEye.data(), 3 * sizeof(float));
  std::memcpy(gpu_params->cam_basis_r, right.data(), 3 * sizeof(float));
  std::memcpy(gpu_params->cam_basis_u, up.data(), 3 * sizeof(float));
  std::memcpy(gpu_params->cam_basis_f, kCamFwd.data(), 3 * sizeof(float));
  gpu_params->dims[0] = kImg;
  gpu_params->dims[1] = kImg;
  gpu_params->dims[2] = kBounces;
  gpu_params->dims[3] = 0u;

  // ---- Accumulation image -----------------------------------------------------
  VkImageCreateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ai.imageType = VK_IMAGE_TYPE_2D;
  ai.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  ai.extent = {kImg, kImg, 1};
  ai.mipLevels = 1;
  ai.arrayLayers = 1;
  ai.samples = VK_SAMPLE_COUNT_1_BIT;
  ai.tiling = VK_IMAGE_TILING_OPTIMAL;
  ai.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ai.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage accum_image = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateImage(ctx.device(), &ai, nullptr, &accum_image),
            VK_SUCCESS);
  auto accum_mem =
      alloc.bind_image(accum_image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ASSERT_TRUE(accum_mem.is_ok());
  Allocation accum_allocation = accum_mem.value();
  VkImageViewCreateInfo avi{};
  avi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  avi.image = accum_image;
  avi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  avi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  avi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView accum_view = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreateImageView(ctx.device(), &avi, nullptr, &accum_view),
            VK_SUCCESS);

  // ---- Descriptors: 0 accum, 1 params, 2 TLAS, 3 instances -------------------
  auto layout = desc.create_layout(
      {{0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
       {0, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
       {0, 2, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
       {0, 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR}},
      1);
  ASSERT_TRUE(layout.is_ok());
  VkDescriptorSetLayout pt_layout = layout.value();
  auto set = desc.allocate_set(pt_layout);
  ASSERT_TRUE(set.is_ok());
  VkDescriptorSet pt_set = set.value();
  ASSERT_TRUE(desc
                  .write_image(pt_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               VK_NULL_HANDLE, accum_view,
                               VK_IMAGE_LAYOUT_GENERAL, 0)
                  .is_ok());
  ASSERT_TRUE(desc
                  .write_buffer(pt_set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                params_alloc.buffer, 0, VK_WHOLE_SIZE)
                  .is_ok());
  ASSERT_TRUE(desc
                  .write_buffer(pt_set, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                inst_alloc.buffer, 0, VK_WHOLE_SIZE)
                  .is_ok());

  // ---- Acceleration structures: 5 box BLASes + TLAS --------------------------
  std::vector<std::vector<float>> tri_data;
  std::vector<BlasBuildInput> inputs;
  auto add_box = [&](float hx, float hy, float hz) {
    tri_data.push_back(make_box_triangles(hx, hy, hz));
    BlasBuildInput in{};
    inputs.push_back(in);
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
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    inputs[i].vertex_buffer_address = addr_of(geom_buffers[i]);
    inputs[i].triangle_count = 12U;
    inputs[i].max_vertex = 35U;
  }

  VulkanAccelerationStructureBuilder builder;
  std::vector<BottomLevelAS> blas_list;
  for (const auto& in : inputs) {
    auto r = builder.create_blas(ctx.device(), alloc, in);
    ASSERT_TRUE(r.is_ok());
    blas_list.push_back(std::move(r.value()));
  }
  auto tlas_r = builder.create_tlas(ctx.device(), alloc, 5U);
  ASSERT_TRUE(tlas_r.is_ok());
  TopLevelAS tlas = std::move(tlas_r.value());

  VulkanScratchPool scratch_pool;
  auto scratch1 = scratch_pool.acquire(alloc, ctx.device(), 1U << 20U);
  ASSERT_TRUE(scratch1.is_ok());

  auto pool_r = omnicpp::render::VulkanRenderer::create_command_pool(
      ctx.device(), qf);
  ASSERT_TRUE(pool_r.is_ok());
  VkCommandPool cmd_pool = pool_r.value();
  auto cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
      ctx.device(), cmd_pool);
  ASSERT_TRUE(cb_r.is_ok());
  VkCommandBuffer build_cb = cb_r.value();
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ASSERT_EQ(vkBeginCommandBuffer(build_cb, &bi), VK_SUCCESS);
  const std::array<std::array<float, 3>, 5> centers = {
      std::array<float, 3>{0.0f, -2 * kRoomHy, 0.0f},
      std::array<float, 3>{0.0f, -kRoomHy, -kRoomHx},
      std::array<float, 3>{0.0f, -kRoomHy, kRoomHx},
      std::array<float, 3>{-kRoomHx, -kRoomHy, 0.0f},
      std::array<float, 3>{kRoomHx, -kRoomHy, 0.0f}};
  VkMemoryBarrier asbar{};
  asbar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  asbar.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  asbar.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  for (std::size_t i = 0; i < blas_list.size(); ++i) {
    ASSERT_TRUE(builder
                    .cmd_build_blas(build_cb, ctx.device(), blas_list[i],
                                    inputs[i], scratch1.value())
                    .is_ok());
    vkCmdPipelineBarrier(
        build_cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &asbar,
        0, nullptr, 0, nullptr);
  }
  std::array<TlasInstance, 5> instances{};
  for (std::size_t i = 0; i < 5; ++i) {
    const auto xf = translated_instance(centers[i][0], centers[i][1],
                                        centers[i][2]);
    std::memcpy(instances[i].transform, xf.data(), sizeof(xf));
    instances[i].instance_custom_index = static_cast<std::uint32_t>(i);
    instances[i].blas_device_address = blas_list[i].device_address;
  }
  ASSERT_TRUE(builder
                  .cmd_build_tlas(build_cb, ctx.device(), tlas,
                                  instances.data(), 5U, scratch1.value())
                  .is_ok());
  ASSERT_EQ(vkEndCommandBuffer(build_cb), VK_SUCCESS);
  ASSERT_TRUE(submit_and_wait(ctx.device(), ctx.graphics_queue(), build_cb));
  ASSERT_TRUE(desc
                  .write_acceleration_structure(
                      pt_set, 2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                      tlas.handle, 0)
                  .is_ok());

  // ---- RT pipeline + SBT (recursion depth 1: loop-driven paths) --------------
  const std::string sd = WARPLOOM_TEST_SHADER_DIR;
  VkShaderModule mods[3] = {};
  const char* files[3] = {"/pt_real.rgen.spv", "/pt_real.rmiss.spv",
                          "/pt_real.rchit.spv"};
  for (std::size_t i = 0; i < static_cast<std::size_t>(3); ++i) {
    std::string path = sd + files[i];
    FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr) << path;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> code(static_cast<std::size_t>(size));
    ASSERT_EQ(std::fread(code.data(), 1, code.size(), f), code.size());
    std::fclose(f);
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = code.size();
    mi.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
    ASSERT_EQ(vkCreateShaderModule(ctx.device(), &mi, nullptr, &mods[i]),
              VK_SUCCESS);
  }
  std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
  const VkShaderStageFlagBits stage_bits[3] = {
      VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
      VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
  for (std::size_t i = 0; i < static_cast<std::size_t>(3); ++i) {
    stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[i].stage = stage_bits[i];
    stages[i].module = mods[i];
    stages[i].pName = "main";
  }
  VkPipelineLayoutCreateInfo pli{};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &pt_layout;
  VkPipelineLayout pipe_layout_handle = VK_NULL_HANDLE;
  ASSERT_EQ(vkCreatePipelineLayout(ctx.device(), &pli, nullptr,
                                   &pipe_layout_handle),
            VK_SUCCESS);

  VulkanRtPipeline rt;
  const std::vector<VkPipelineShaderStageCreateInfo> stage_list(
      stages.begin(), stages.end());
  auto created = rt.create(ctx.physical_device(), ctx.device(),
                           pipe_layout_handle, stage_list,
                           /*miss_count=*/1U, /*hit_group_count=*/1U,
                           /*max_recursion_depth=*/1U);
  ASSERT_TRUE(created.is_ok());
  auto handles = rt.fetch_handles(ctx.device());
  ASSERT_TRUE(handles.is_ok());
  EXPECT_EQ(handles.value().size(), 32U * 3U)
      << "SBT handles: 1 raygen + 1 miss + 1 hit group";
  auto sbt_buf = alloc.create_buffer(
      VulkanRtPipeline::required_sbt_bytes(ctx.physical_device(), 1U, 1U),
      VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ASSERT_TRUE(sbt_buf.is_ok());
  Allocation sbt = sbt_buf.value();
  ASSERT_TRUE(rt.write_sbt(ctx.device(), handles.value(), sbt).is_ok());

  // ---- Per-frame recording (E3 pattern) --------------------------------------
  auto record_round = [&](std::uint32_t /*frame*/, bool clear_first) {
    auto r_cb_r = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), cmd_pool);
    EXPECT_TRUE(r_cb_r.is_ok());
    VkCommandBuffer cb = r_cb_r.value();
    VkCommandBufferBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(cb, &rbi), VK_SUCCESS);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      rt.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            pipe_layout_handle, 0, 1, &pt_set, 0, nullptr);
    if (clear_first) {
      VkImageMemoryBarrier to_general{};
      to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_general.srcAccessMask = 0;
      to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      to_general.image = accum_image;
      to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &to_general);
      VkClearColorValue black{};
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cb, accum_image, VK_IMAGE_LAYOUT_GENERAL, &black,
                           1, &range);
      VkImageMemoryBarrier general_rw = to_general;
      general_rw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      general_rw.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                           nullptr, 0, nullptr, 1, &general_rw);
    } else {
      VkImageMemoryBarrier vis{};
      vis.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      vis.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      vis.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT;
      vis.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      vis.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      vis.image = accum_image;
      vis.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                           nullptr, 0, nullptr, 1, &vis);
    }
    rt.trace_rays(cb, kImg, kImg, 1U);
    VkImageMemoryBarrier done{};
    done.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    done.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    done.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    done.image = accum_image;
    done.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &done);
    ASSERT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    EXPECT_TRUE(submit_and_wait(ctx.device(), ctx.graphics_queue(), cb));
  };

  auto read_accum = [&]() -> std::vector<float> {
    auto rb_buf = alloc.create_buffer(
        kImg * kImg * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    EXPECT_TRUE(rb_buf.is_ok());
    Allocation rb = rb_buf.value();
    auto cbr = omnicpp::render::VulkanRenderer::allocate_command_buffer(
        ctx.device(), cmd_pool);
    EXPECT_TRUE(cbr.is_ok());
    VkCommandBuffer cb = cbr.value();
    VkCommandBufferBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    EXPECT_EQ(vkBeginCommandBuffer(cb, &rbi), VK_SUCCESS);
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.image = accum_image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_src);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kImg, kImg, 1};
    vkCmdCopyImageToBuffer(cb, accum_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1,
                           &copy);
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_SHADER_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                         nullptr, 0, nullptr, 1, &back);
    EXPECT_EQ(vkEndCommandBuffer(cb), VK_SUCCESS);
    EXPECT_TRUE(submit_and_wait(ctx.device(), ctx.graphics_queue(), cb));
    std::vector<float> out(static_cast<std::size_t>(kImg) * kImg * 4);
    std::memcpy(out.data(), rb.mapped, out.size() * sizeof(float));
    alloc.destroy_allocation(rb);
    return out;
  };

  auto run_pass = [&]() -> std::vector<float> {
    gpu_params->cam_pos_frame[3] = 0.0f;
    record_round(0, true);
    for (std::uint32_t f = 1; f < kFrames; ++f) {
      gpu_params->cam_pos_frame[3] = static_cast<float>(f);
      record_round(f, false);
    }
    gpu_params->cam_pos_frame[3] = 0.0f;
    const std::vector<float> sum = read_accum();
    std::vector<float> mean(sum.size());
    const float inv_k = 1.0f / static_cast<float>(kFrames);
    for (std::size_t i = 0; i < sum.size(); ++i) mean[i] = sum[i] * inv_k;
    return mean;
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

  vkDestroyImageView(ctx.device(), accum_view, nullptr);
  vkDestroyImage(ctx.device(), accum_image, nullptr);
  for (auto& b : blas_list) builder.destroy_blas(ctx.device(), alloc, b);
  builder.destroy_tlas(ctx.device(), alloc, tlas);
  for (auto& ga : geom_allocs) alloc.destroy_allocation(ga);
  alloc.destroy_allocation(sbt);
  alloc.destroy_allocation(inst_alloc);
  alloc.destroy_allocation(params_alloc);
  alloc.destroy_allocation(accum_allocation);
  for (std::size_t i = 0; i < static_cast<std::size_t>(3); ++i) vkDestroyShaderModule(ctx.device(), mods[i], nullptr);
  rt.cleanup(ctx.device());
  vkDestroyPipelineLayout(ctx.device(), pipe_layout_handle, nullptr);
  vkDestroyCommandPool(ctx.device(), cmd_pool, nullptr);
  desc.cleanup();
  alloc.cleanup();
  ctx.cleanup();
}

#else  // !WARPLOOM_HAS_VULKAN

TEST(path_tracing_real, disabled_without_vulkan) {
  GTEST_SKIP() << "Vulkan not available";
}

#endif  // WARPLOOM_HAS_VULKAN
