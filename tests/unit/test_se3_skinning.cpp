//! @file test_se3_skinning.cpp
//! Trial #611 spike: LBS (incumbent shader math) vs dual-quaternion
//! SE(3) blend on the candy-wrapper probe.
//!
//! Probe: two bones, bone0 = identity, bone1 = 180-degree twist about Y
//! (the bar's long axis). A mid-weight vertex (0.5/0.5) at radius 0.2 from
//! the axis. LBS averages the matrices to a rank-deficient scale (~zero
//! radius); DLB blends on SE(3) and preserves the radius (rotated 90 deg).
//! A second probe (90-degree bend) shows both paths sane, so the test
//! characterizes the incumbent rather than straw-manning it.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include <gtest/gtest.h>

#include "warploom/core/dual_quat_skinning.hpp"

namespace {

using warploom::core::dlb_blend_transform;
using warploom::core::dual_quat_from_rigid;
using warploom::core::DualQuat;
using warploom::core::lbs_blend_transform;
using warploom::core::RigidBone;

RigidBone make_identity() { return RigidBone{}; }

RigidBone make_rot_y(float radians) {
  RigidBone b{};
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  // Column-major rotation about Y: col0=(c,0,-s), col1=(0,1,0), col2=(s,0,c).
  b.m[0] = c;
  b.m[2] = -s;
  b.m[5] = 1.0f;
  b.m[8] = s;
  b.m[10] = c;
  return b;
}

float radial(const float p[3]) {
  return std::sqrt(p[0] * p[0] + p[2] * p[2]);
}

TEST(Se3SkinningTrial, TwistProbeDlbPreservesRadiusLbsCollapses) {
  const RigidBone bones[2]{make_identity(),
                           make_rot_y(3.14159265358979323846f)};
  DualQuat dq[2]{dual_quat_from_rigid(bones[0]),
                 dual_quat_from_rigid(bones[1])};
  const float w[2]{0.5f, 0.5f};
  const float p[3]{0.2f, 0.0f, 0.0f};

  float lbs[3]{};
  float dlb[3]{};
  lbs_blend_transform(bones, w, 2, p, lbs);
  dlb_blend_transform(dq, w, 2, p, dlb);

  const float lbs_r = radial(lbs);
  const float dlb_r = radial(dlb);
  std::printf("[se3] twist180 lbs_r=%.6f dlb_r=%.6f\n",
              static_cast<double>(lbs_r), static_cast<double>(dlb_r));

  // Incumbent characterization: LBS collapses to the axis.
  EXPECT_LT(lbs_r, 0.01f);
  // Challenger requirement: DLB preserves the rest radius.
  EXPECT_NEAR(dlb_r, 0.2f, 0.01f);
  // DLB rotates halfway (90 deg): (0.2,0,0) -> ~(0,0,-0.2) up to sign.
  EXPECT_LT(std::abs(dlb[0]), 0.02f);
  EXPECT_NEAR(std::abs(dlb[2]), 0.2f, 0.02f);
}

TEST(Se3SkinningTrial, BendProbeBothSane) {
  const RigidBone bones[2]{make_identity(),
                           make_rot_y(3.14159265358979323846f * 0.5f)};
  DualQuat dq[2]{dual_quat_from_rigid(bones[0]),
                 dual_quat_from_rigid(bones[1])};
  const float w[2]{0.5f, 0.5f};
  const float p[3]{0.2f, 0.0f, 0.0f};
  float lbs[3]{};
  float dlb[3]{};
  lbs_blend_transform(bones, w, 2, p, lbs);
  dlb_blend_transform(dq, w, 2, p, dlb);
  const float lbs_r = radial(lbs);
  const float dlb_r = radial(dlb);
  std::printf("[se3] bend90 lbs_r=%.6f dlb_r=%.6f\n",
              static_cast<double>(lbs_r), static_cast<double>(dlb_r));
  // LBS already shrinks 29% at 90 deg (0.2 -> 0.1414 = cos(45deg)*0.2:
  // the linear average of two 90-deg-apart frames shortens); DLB keeps
  // the exact radius. Neither collapses, but only DLB is rigid here.
  EXPECT_NEAR(lbs_r, 0.141421f, 0.01f);
  EXPECT_NEAR(dlb_r, 0.2f, 0.01f);
}

TEST(Se3SkinningTrial, TimingDlbVsLbs) {
  const RigidBone bones[2]{make_identity(), make_rot_y(1.0f)};
  DualQuat dq[2]{dual_quat_from_rigid(bones[0]),
                 dual_quat_from_rigid(bones[1])};
  const float w[2]{0.5f, 0.5f};
  const float p[3]{0.2f, 0.35f, -0.1f};
  float out[3]{};
  constexpr int kIters = 200000;
  volatile float sink = 0.0f;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; ++i) {
    lbs_blend_transform(bones, w, 2, p, out);
    sink += out[0] + out[1] + out[2];
  }
  auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIters; ++i) {
    dlb_blend_transform(dq, w, 2, p, out);
    sink += out[0] + out[1] + out[2];
  }
  auto t2 = std::chrono::steady_clock::now();
  (void)sink;
  const double lbs_ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
  const double dlb_ns =
      std::chrono::duration<double, std::nano>(t2 - t1).count() / kIters;
  std::printf("[se3] timing lbs=%.1fns dlb=%.1fns ratio=%.2fx\n", lbs_ns,
              dlb_ns, dlb_ns / lbs_ns);
  // DLB must stay within 3x LBS on the CPU leg (it is ~1.3x here); a
  // blown-up blend (e.g. per-vertex heap work) fails this.
  EXPECT_LT(dlb_ns, lbs_ns * 3.0);
}

}  // namespace
