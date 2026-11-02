//! @file test_multiscale_topology.cpp
//! Trial #602 spike: homological multi-scale slicing prerequisite --
//! topology preservation across decimation scales.
//!
//! The full 602 claim (no LOD pop-in via continuous slicing) needs render
//! integration and stays staged. The homological prerequisite is CPU sized:
//! a multi-scale chain must preserve the mesh's topology (Betti numbers;
//! here Euler characteristic chi = V-E+F on the live index soup) while a
//! curvature-blind collapse is free to pinch handles and change it.
//! Probe: closed torus (chi = 0, no boundary so every edge is eligible),
//! 768 tris -> 192, QEM vs fixed-seed blind. Both meet the identical
//! budget; only the topology outcome differs -- if it doesn't, this probe
//! is rejected as non-discriminating (recorded, not forced).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "warploom/core/mesh_topology.hpp"
#include "warploom/core/quadric_decimation.hpp"

namespace {

using warploom::core::DecimationMesh;
using warploom::core::decimate_blind;
using warploom::core::decimate_quadric;
using warploom::core::euler_chi;

//! Closed torus: major radius 1, minor 0.35, 24 major x 16 minor quads
//! -> 384 verts, 768 tris. chi = 0 (one component, one handle).
DecimationMesh make_torus(int major = 24, int minor = 16) {
  DecimationMesh m;
  constexpr float kR = 1.0f;
  constexpr float kr = 0.35f;
  for (int j = 0; j < minor; ++j) {
    for (int i = 0; i < major; ++i) {
      const float u = 2.0f * 3.14159265358979323846f *
                      static_cast<float>(i) / static_cast<float>(major);
      const float v = 2.0f * 3.14159265358979323846f *
                      static_cast<float>(j) / static_cast<float>(minor);
      const float x = (kR + kr * std::cos(v)) * std::cos(u);
      const float y = (kR + kr * std::cos(v)) * std::sin(u);
      const float z = kr * std::sin(v);
      m.positions.insert(m.positions.end(), {x, y, z});
    }
  }
  for (int j = 0; j < minor; ++j) {
    for (int i = 0; i < major; ++i) {
      const std::uint32_t a = static_cast<std::uint32_t>(j * major + i);
      const std::uint32_t b =
          static_cast<std::uint32_t>(j * major + (i + 1) % major);
      const std::uint32_t c =
          static_cast<std::uint32_t>(((j + 1) % minor) * major + i);
      const std::uint32_t d =
          static_cast<std::uint32_t>(((j + 1) % minor) * major +
                                     (i + 1) % major);
      m.indices.insert(m.indices.end(), {a, b, d, a, d, c});
    }
  }
  return m;
}

std::size_t live_tris(const DecimationMesh& m) {
  std::size_t n = 0;
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    if (m.indices[t] != m.indices[t + 1] &&
        m.indices[t + 1] != m.indices[t + 2] &&
        m.indices[t + 2] != m.indices[t])
      ++n;
  }
  return n;
}

//! chi of a decimated mesh = chi of its live index soup (dead verts and
//! degenerate tris excluded by construction in mesh_topology.hpp).
long mesh_chi(const DecimationMesh& m) { return euler_chi(m.indices); }

TEST(MultiscaleTopologyTrial, SourceTorusHasChiZero) {
  const DecimationMesh src = make_torus();
  ASSERT_EQ(live_tris(src), 768U);
  EXPECT_EQ(mesh_chi(src), 0L);
}

TEST(MultiscaleTopologyTrial, QemPreservesChiBlindPinchesHandle) {
  const DecimationMesh src = make_torus();
  constexpr std::size_t kTarget = 192;
  const DecimationMesh qem = decimate_quadric(src, kTarget);
  const DecimationMesh blind = decimate_blind(src, kTarget, 0x602602ULL);
  const long qem_chi = mesh_chi(qem);
  const long blind_chi = mesh_chi(blind);
  std::printf("[mslice] tris qem=%zu blind=%zu chi qem=%ld blind=%ld\n",
              live_tris(qem), live_tris(blind), qem_chi, blind_chi);
  EXPECT_EQ(live_tris(qem), kTarget);
  EXPECT_EQ(live_tris(blind), kTarget);
  // QEM must keep the handle (chi 0); the blind baseline is expected to
  // pinch it (chi != 0). If blind also keeps chi 0, this probe does not
  // discriminate and must be reworked, not forced -- hence the explicit
  // inequality assertion on the baseline side too.
  EXPECT_EQ(qem_chi, 0L);
  EXPECT_NE(blind_chi, 0L);
}

}  // namespace
