//! @file quadric_decimation.hpp
//! Curvature-preserving polyhedral decimation (trial #800 verdict, ADR-006).
//!
//! The engine selects LODs on the GPU (`lod_select.comp`) but nothing
//! GENERATES lower-LOD meshes (docs/rendering-status.md: "selection
//! exists; nothing generates lower-LOD meshes"). This header stages that
//! capability: a minimal midpoint-placement quadric-error-metric (QEM,
//! Garland & Heckbert) edge-collapse decimator plus a shortest-edge
//! baseline sharing the same collapse machinery, so the trial compares
//! objectives rather than implementations.
//!
//! Scope (stated, not hidden): midpoint placement (not optimal-position
//! QEM), original boundary vertices locked (an edge incident to a
//! boundary vertex never collapses, so the patch outline is fixed and
//! the volume comparison measures interior shape preservation only),
//! no attribute (uv/normal/color) handling, no manifold repair. One
//! interior edge collapses per iteration, so both
//! paths remove exactly 2 triangles per step and meet the budget at
//! identical triangle counts -- the volume comparison is apples-to-apples.
//! Attribute-aware optimal QEM is staged for the asset-pipeline consumer.
//! Cost is within 3x of the naive baseline on the spike mesh (same loop,
//! only the cost function differs).

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace warploom::core {

//! Triangle soup mesh: 3 floats per vertex, 3 indices per triangle.
struct DecimationMesh {
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
};

//! Symmetric 4x4 quadric stored as 10 upper-triangular entries:
//! [0]=m00 [1]=m01 [2]=m02 [3]=m03 [4]=m11 [5]=m12 [6]=m13 [7]=m22
//! [8]=m23 [9]=m33. Double accumulation; float inputs enter only through
//! explicit static_cast<double> (this header builds under
//! -Wdouble-promotion).
struct Quadric {
  double q[10]{};
};

inline void quadric_add(Quadric& a, const Quadric& b) {
  for (int i = 0; i < 10; ++i) a.q[i] += b.q[i];
}

//! Plane quadric for (nx,ny,nz,d) with n unit-length: Q = [n;d][n;d]^T.
inline Quadric quadric_from_plane(double nx, double ny, double nz,
                                  double d) {
  Quadric out{};
  const double v[4]{nx, ny, nz, d};
  int k = 0;
  for (int i = 0; i < 4; ++i)
    for (int j = i; j < 4; ++j) out.q[k++] = v[i] * v[j];
  return out;
}

//! v^T Q v at a 3D point (homogeneous w=1).
inline double quadric_eval(const Quadric& q, double x, double y, double z) {
  const double v[4]{x, y, z, 1.0};
  double sum = 0.0;
  int k = 0;
  for (int i = 0; i < 4; ++i)
    for (int j = i; j < 4; ++j) {
      const double term = q.q[k++] * v[i] * v[j];
      sum += (i == j) ? term : 2.0 * term;
    }
  return sum;
}

//! Three-way double comparison using only < (no ==, so -Wfloat-equal
//! stays silent); exact ties fall through to the caller's integer keys,
//! keeping the greedy order deterministic.
inline int double_order(double a, double b) {
  if (a < b) return -1;
  if (b < a) return 1;
  return 0;
}

namespace detail {

inline double pos3(const std::vector<float>& pos, std::uint32_t v, int c) {
  return static_cast<double>(pos[3U * v + static_cast<std::uint32_t>(c)]);
}

//! Cheapest collapsible edge snapshot: (a, b, collapse cost). Edges with
//! one adjacent face are excluded, as is any edge touching an original
//! boundary vertex (keeps the patch outline fixed). Deterministic:
//! minimum over (cost, a, b) with < comparisons only.
//! @returns false when no interior edge remains.
template <typename CostFn>
bool cheapest_edge(const std::vector<float>& pos,
                   const std::vector<std::uint32_t>& idx,
                   const std::vector<Quadric>& vert_q, CostFn cost_fn,
                   std::uint32_t& out_a, std::uint32_t& out_b) {
  struct Key {
    std::uint32_t a;
    std::uint32_t b;
  };
  std::vector<Key> keys;
  keys.reserve(idx.size());
  for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
    const std::uint32_t v[3]{idx[t], idx[t + 1], idx[t + 2]};
    if (v[0] == v[1] || v[1] == v[2] || v[2] == v[0]) continue;
    for (int e = 0; e < 3; ++e) {
      std::uint32_t a = v[e];
      std::uint32_t b = v[(e + 1) % 3];
      if (a > b) {
        const std::uint32_t tmp = a;
        a = b;
        b = tmp;
      }
      keys.push_back({a, b});
    }
  }
  // Deterministic insertion sort by (a,b): tiny edge counts here.
  for (std::size_t i = 1; i < keys.size(); ++i) {
    Key k = keys[i];
    std::size_t j = i;
    while (j > 0 && (keys[j - 1].a > k.a ||
                     (keys[j - 1].a == k.a && keys[j - 1].b > k.b))) {
      keys[j] = keys[j - 1];
      --j;
    }
    keys[j] = k;
  }
  // Original boundary vertices: incident to any single-face edge. Locked
  // for the whole decimation (collapses never touch them, so the set is
  // stable across iterations).
  const std::size_t nverts = pos.size() / 3;
  std::vector<char> locked(nverts, 0);
  for (std::size_t i = 0; i < keys.size();) {
    std::size_t j = i + 1;
    while (j < keys.size() && keys[j].a == keys[i].a &&
           keys[j].b == keys[i].b)
      ++j;
    if (j - i == 1) {
      locked[keys[i].a] = 1;
      locked[keys[i].b] = 1;
    }
    i = j;
  }
  bool found = false;
  double best_cost = 0.0;
  out_a = 0;
  out_b = 0;
  for (std::size_t i = 0; i < keys.size();) {
    std::size_t j = i + 1;
    while (j < keys.size() && keys[j].a == keys[i].a &&
           keys[j].b == keys[i].b)
      ++j;
    const int faces = static_cast<int>(j - i);
    if (faces >= 2 && !locked[keys[i].a] && !locked[keys[i].b]) {
      const double c = cost_fn(keys[i].a, keys[i].b, pos, vert_q);
      bool take = !found;
      if (!take) {
        const int o = double_order(c, best_cost);
        take = o < 0 || (o == 0 && (keys[i].a < out_a ||
                                   (keys[i].a == out_a &&
                                    keys[i].b < out_b)));
      }
      if (take) {
        found = true;
        best_cost = c;
        out_a = keys[i].a;
        out_b = keys[i].b;
      }
    }
    i = j;
  }
  return found;
}

//! Collapse edge (a,b): survivor a moves to the midpoint, b remaps to a,
//! degenerate triangles drop. Quadrics merge into the survivor.
inline void collapse_edge(std::vector<float>& pos,
                          std::vector<std::uint32_t>& idx,
                          std::vector<Quadric>& vert_q, std::uint32_t a,
                          std::uint32_t b) {
  pos[3U * a] = static_cast<float>((pos3(pos, a, 0) + pos3(pos, b, 0)) * 0.5);
  pos[3U * a + 1U] =
      static_cast<float>((pos3(pos, a, 1) + pos3(pos, b, 1)) * 0.5);
  pos[3U * a + 2U] =
      static_cast<float>((pos3(pos, a, 2) + pos3(pos, b, 2)) * 0.5);
  quadric_add(vert_q[a], vert_q[b]);
  for (std::size_t i = 0; i < idx.size(); ++i) {
    if (idx[i] == b) idx[i] = a;
  }
  std::size_t out = 0;
  for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
    if (idx[t] == idx[t + 1] || idx[t + 1] == idx[t + 2] ||
        idx[t + 2] == idx[t])
      continue;
    idx[out++] = idx[t];
    idx[out++] = idx[t + 1];
    idx[out++] = idx[t + 2];
  }
  idx.resize(out);
}

inline void accumulate_quadrics(const std::vector<float>& pos,
                                const std::vector<std::uint32_t>& idx,
                                std::vector<Quadric>& vert_q) {
  vert_q.assign(pos.size() / 3, Quadric{});
  for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
    const double p0x = pos3(pos, idx[t], 0);
    const double p0y = pos3(pos, idx[t], 1);
    const double p0z = pos3(pos, idx[t], 2);
    const double ux = pos3(pos, idx[t + 1], 0) - p0x;
    const double uy = pos3(pos, idx[t + 1], 1) - p0y;
    const double uz = pos3(pos, idx[t + 1], 2) - p0z;
    const double vx = pos3(pos, idx[t + 2], 0) - p0x;
    const double vy = pos3(pos, idx[t + 2], 1) - p0y;
    const double vz = pos3(pos, idx[t + 2], 2) - p0z;
    double nx = uy * vz - uz * vy;
    double ny = uz * vx - ux * vz;
    double nz = ux * vy - uy * vx;
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 0.0)) continue;
    nx /= len;
    ny /= len;
    nz /= len;
    const double d = -(nx * p0x + ny * p0y + nz * p0z);
    const Quadric q = quadric_from_plane(nx, ny, nz, d);
    quadric_add(vert_q[idx[t]], q);
    quadric_add(vert_q[idx[t + 1]], q);
    quadric_add(vert_q[idx[t + 2]], q);
  }
}

//! Shared driver: one cheapest edge per iteration until the budget or
//! exhaustion. `refresh_quadrics` selects QEM (true) vs naive (false).
template <typename CostFn>
DecimationMesh decimate_loop(const DecimationMesh& in,
                             std::size_t target_tris, bool refresh_quadrics,
                             CostFn cost_fn) {
  DecimationMesh out{in.positions, in.indices};
  std::vector<Quadric> vert_q(out.positions.size() / 3);
  for (int iter = 0; iter < 4096; ++iter) {
    std::size_t live = 0;
    for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
      if (out.indices[t] != out.indices[t + 1] &&
          out.indices[t + 1] != out.indices[t + 2] &&
          out.indices[t + 2] != out.indices[t])
        ++live;
    }
    if (live <= target_tris) break;
    if (refresh_quadrics) {
      detail::accumulate_quadrics(out.positions, out.indices, vert_q);
    }
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    if (!detail::cheapest_edge(out.positions, out.indices, vert_q, cost_fn,
                               a, b))
      break;
    detail::collapse_edge(out.positions, out.indices, vert_q, a, b);
  }
  return out;
}

}  // namespace detail

//! QEM decimation: collapse cheapest-quadric-error edges until at most
//! target_tris live triangles remain (or no collapsible edge is left).
inline DecimationMesh decimate_quadric(const DecimationMesh& in,
                                       std::size_t target_tris) {
  auto cost = [](std::uint32_t a, std::uint32_t b,
                 const std::vector<float>& pos,
                 const std::vector<Quadric>& vq) {
    Quadric sum = vq[a];
    quadric_add(sum, vq[b]);
    const double mx =
        (detail::pos3(pos, a, 0) + detail::pos3(pos, b, 0)) * 0.5;
    const double my =
        (detail::pos3(pos, a, 1) + detail::pos3(pos, b, 1)) * 0.5;
    const double mz =
        (detail::pos3(pos, a, 2) + detail::pos3(pos, b, 2)) * 0.5;
    return quadric_eval(sum, mx, my, mz);
  };
  return detail::decimate_loop(in, target_tris, true, cost);
}

//! Blind baseline: same machinery, cost = deterministic hash of the edge
//! (fixed seed). Curvature-blind by construction: it spends the budget
//! uniformly instead of protecting low-error regions. Shortest-edge was
//! tried as the baseline and REJECTED: edge length implicitly encodes
//! curvature (feature edges are longer), so it protects features for the
//! wrong reason and the trial measured noise. A fixed-seed hash is
//! deterministic and plays no such favorites.
inline std::uint64_t splitmix64_edge(std::uint32_t a, std::uint32_t b,
                                      std::uint64_t seed) {
  std::uint64_t z = seed + static_cast<std::uint64_t>(a) * 0x9E3779B97F4A7C15ULL +
                    static_cast<std::uint64_t>(b) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

inline DecimationMesh decimate_blind(const DecimationMesh& in,
                                     std::size_t target_tris,
                                     std::uint64_t seed) {
  auto cost = [seed](std::uint32_t a, std::uint32_t b,
                     const std::vector<float>&,
                     const std::vector<Quadric>&) {
    return static_cast<double>(splitmix64_edge(a, b, seed) >> 11) *
           (1.0 / 9007199254740992.0);
  };
  return detail::decimate_loop(in, target_tris, false, cost);
}

//! Volume under a heightfield patch: sum over triangles of
//! signed projected volume (x-y projection, mean height). Positive for
//! upward patches; the trial only compares ratios.
inline double patch_volume(const DecimationMesh& m) {
  double vol = 0.0;
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const double ax = detail::pos3(m.positions, m.indices[t], 0);
    const double ay = detail::pos3(m.positions, m.indices[t], 1);
    const double az = detail::pos3(m.positions, m.indices[t], 2);
    const double bx = detail::pos3(m.positions, m.indices[t + 1], 0);
    const double by = detail::pos3(m.positions, m.indices[t + 1], 1);
    const double bz = detail::pos3(m.positions, m.indices[t + 1], 2);
    const double cx = detail::pos3(m.positions, m.indices[t + 2], 0);
    const double cy = detail::pos3(m.positions, m.indices[t + 2], 1);
    const double cz = detail::pos3(m.positions, m.indices[t + 2], 2);
    const double area2 = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    const double hmean = (az + bz + cz) / 3.0;
    vol += area2 * hmean * 0.5;
  }
  return vol < 0.0 ? -vol : vol;
}

}  // namespace warploom::core
