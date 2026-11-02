//! @file dual_quat_skinning.hpp
//! SE(3) dual-quaternion blend skinning (trial #611 verdict, ADR-005).
//!
//! The incumbent (skinned_scene.vert, pbr_gpu_driven_skinned.vert,
//! shadow_skinned.vert) blends bone matrices linearly (LBS):
//!   M = sum w_i * M_i. Under a ~180-degree twist between two bones the
//! blended matrix collapses toward zero scale (measured radial distance
//! 0.0 vs 0.2 rest radius on the spike's twist probe), the classic
//! candy-wrapper artifact. Dual-quaternion blending (DLB, Kavan et al.)
//! blends rigid bone transforms on SE(3) instead: convert each bone to a
//! unit dual quaternion, weighted-sum with antipodal correction,
//! renormalize, then transform. The spike measures full radius
//! preservation (0.2/0.2) on the same probe where LBS collapses.
//!
//! Contract: bones are rigid (rotation + translation only, which is all
//! the glTF sampler path uploads: global * inverse-bind of rigid joints).
//! Scale/shear bones are out of scope -- LBS handles those, DLB does not
//! claim to. Blend inputs are normalized weights summing to 1.
//! Cost is ~1.3x LBS on the CPU leg (measured, this machine); the GPU
//! migration (bone SSBO as 8 floats vs 16, normalize in-shader) is staged,
//! not landed -- this header stages the capability, shaders still do LBS.

#pragma once

#include <cmath>

namespace warploom::core {

//! Minimal quaternion.
struct DualQuatRotation {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
  float w{1.0f};
};

//! Unit dual quaternion: real part = rotation, dual part = translation.
struct DualQuat {
  DualQuatRotation real{0.0f, 0.0f, 0.0f, 1.0f};
  DualQuatRotation dual{0.0f, 0.0f, 0.0f, 0.0f};
};

//! Column-major rigid 4x4 (rotation 3x3 + translation), as the bone SSBO
//! stores it. m[12..14] = translation.
struct RigidBone {
  float m[16]{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

inline float dual_quat_dot(const DualQuatRotation& a,
                           const DualQuatRotation& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

//! Rotation matrix (upper 3x3 of a rigid bone) -> unit quaternion.
//! Branchless trace path with a trace<=0 fallback; deterministic for a
//! given input (no data-dependent iteration).
inline DualQuatRotation quat_from_mat3(const float m[16]) {
  const float trace = m[0] + m[5] + m[10];
  DualQuatRotation q{};
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m[6] - m[9]) / s;
    q.y = (m[8] - m[2]) / s;
    q.z = (m[1] - m[4]) / s;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const float s = std::sqrt(1.0f + m[0] - m[5] - m[10]) * 2.0f;
    q.w = (m[6] - m[9]) / s;
    q.x = 0.25f * s;
    q.y = (m[1] + m[4]) / s;
    q.z = (m[8] + m[2]) / s;
  } else if (m[5] > m[10]) {
    const float s = std::sqrt(1.0f + m[5] - m[0] - m[10]) * 2.0f;
    q.w = (m[8] - m[2]) / s;
    q.x = (m[1] + m[4]) / s;
    q.y = 0.25f * s;
    q.z = (m[6] + m[9]) / s;
  } else {
    const float s = std::sqrt(1.0f + m[10] - m[0] - m[5]) * 2.0f;
    q.w = (m[1] - m[4]) / s;
    q.x = (m[8] + m[2]) / s;
    q.y = (m[6] + m[9]) / s;
    q.z = 0.25f * s;
  }
  // Normalize defensively (input is orthonormal up to float error).
  const float n =
      std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n > 0.0f) {
    q.x /= n;
    q.y /= n;
    q.z /= n;
    q.w /= n;
  }
  return q;
}

//! Rigid bone -> unit dual quaternion.
//! qd = 0.5 * t_quat * qr, with t = (tx, ty, tz, 0).
inline DualQuat dual_quat_from_rigid(const RigidBone& bone) {
  DualQuat dq;
  dq.real = quat_from_mat3(bone.m);
  const float tx = bone.m[12];
  const float ty = bone.m[13];
  const float tz = bone.m[14];
  const float rx = dq.real.x;
  const float ry = dq.real.y;
  const float rz = dq.real.z;
  const float rw = dq.real.w;
  // t_quat * qr (Hamilton product, t has w=0):
  //   w = -(tx*rx + ty*ry + tz*rz)
  //   x = tx*rw + ty*rz - tz*ry, etc.
  dq.dual.w = -0.5f * (tx * rx + ty * ry + tz * rz);
  dq.dual.x = 0.5f * (tx * rw + ty * rz - tz * ry);
  dq.dual.y = 0.5f * (-tx * rz + ty * rw + tz * rx);
  dq.dual.z = 0.5f * (tx * ry - ty * rx + tz * rw);
  return dq;
}

//! Weighted dual-quaternion blend with antipodal correction + full
//! normalization (scale + orthogonalization). Weights must sum to 1.
inline DualQuat dual_quat_blend(const DualQuat* bones, const float* weights,
                                int count) {
  DualQuat acc{};
  acc.real = {0.0f, 0.0f, 0.0f, 0.0f};
  acc.dual = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int i = 0; i < count; ++i) {
    float s = weights[i];
    // Antipodal correction: dq and -dq name the same transform; flip
    // challengers to the hemisphere of the first bone so they add.
    if (i > 0 &&
        dual_quat_dot(bones[0].real, bones[i].real) < 0.0f) {
      s = -s;
    }
    acc.real.x += s * bones[i].real.x;
    acc.real.y += s * bones[i].real.y;
    acc.real.z += s * bones[i].real.z;
    acc.real.w += s * bones[i].real.w;
    acc.dual.x += s * bones[i].dual.x;
    acc.dual.y += s * bones[i].dual.y;
    acc.dual.z += s * bones[i].dual.z;
    acc.dual.w += s * bones[i].dual.w;
  }
  const float n = std::sqrt(dual_quat_dot(acc.real, acc.real));
  if (n > 0.0f) {
    acc.real.x /= n;
    acc.real.y /= n;
    acc.real.z /= n;
    acc.real.w /= n;
    acc.dual.x /= n;
    acc.dual.y /= n;
    acc.dual.z /= n;
    acc.dual.w /= n;
    // Orthogonalize: qd -= dot(qr, qd) * qr (removes scale contamination).
    const float d = dual_quat_dot(acc.real, acc.dual);
    acc.dual.x -= d * acc.real.x;
    acc.dual.y -= d * acc.real.y;
    acc.dual.z -= d * acc.real.z;
    acc.dual.w -= d * acc.real.w;
  }
  return acc;
}

//! Dual quaternion -> rigid bone (rotation + translation extraction).
//! t = 2 * (qd * conj(qr)).xyz.
inline RigidBone rigid_from_dual_quat(const DualQuat& dq) {
  RigidBone out{};
  const float x = dq.real.x;
  const float y = dq.real.y;
  const float z = dq.real.z;
  const float w = dq.real.w;
  out.m[0] = 1.0f - 2.0f * (y * y + z * z);
  out.m[1] = 2.0f * (x * y + z * w);
  out.m[2] = 2.0f * (x * z - y * w);
  out.m[3] = 0.0f;
  out.m[4] = 2.0f * (x * y - z * w);
  out.m[5] = 1.0f - 2.0f * (x * x + z * z);
  out.m[6] = 2.0f * (y * z + x * w);
  out.m[7] = 0.0f;
  out.m[8] = 2.0f * (x * z + y * w);
  out.m[9] = 2.0f * (y * z - x * w);
  out.m[10] = 1.0f - 2.0f * (x * x + y * y);
  out.m[11] = 0.0f;
  // Translation: 2 * (qd * conj(qr)).xyz with conj(qr) = (-x,-y,-z,w).
  // (a * b).x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y, etc.
  const float dx = dq.dual.x;
  const float dy = dq.dual.y;
  const float dz = dq.dual.z;
  const float dw = dq.dual.w;
  out.m[12] = 2.0f * (dw * -x + dx * w + dy * -z - dz * -y);
  out.m[13] = 2.0f * (dw * -y + dy * w + dz * -x - dx * -z);
  out.m[14] = 2.0f * (dw * -z + dz * w + dx * -y - dy * -x);
  out.m[15] = 1.0f;
  return out;
}

//! LBS baseline (what the shaders do): linear matrix blend + transform.
inline void lbs_blend_transform(const RigidBone* bones, const float* weights,
                                int count, const float p[3], float out[3]) {
  float m[16]{};
  for (int i = 0; i < count; ++i) {
    for (int k = 0; k < 16; ++k) m[k] += weights[i] * bones[i].m[k];
  }
  out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
  out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
  out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

//! DLB path: blend dual quats, extract rigid, transform.
inline void dlb_blend_transform(const DualQuat* bones, const float* weights,
                                int count, const float p[3], float out[3]) {
  const DualQuat b = dual_quat_blend(bones, weights, count);
  const RigidBone m = rigid_from_dual_quat(b);
  out[0] = m.m[0] * p[0] + m.m[4] * p[1] + m.m[8] * p[2] + m.m[12];
  out[1] = m.m[1] * p[0] + m.m[5] * p[1] + m.m[9] * p[2] + m.m[13];
  out[2] = m.m[2] * p[0] + m.m[6] * p[1] + m.m[10] * p[2] + m.m[14];
}

}  // namespace warploom::core
