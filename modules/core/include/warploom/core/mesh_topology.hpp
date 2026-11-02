//! @file mesh_topology.hpp
//! Homological LOD guard (trial #602 verdict, ADR-007).
//!
//! The full 602 claim (no LOD pop-in via continuous multi-scale slicing)
//! needs render integration and stays staged. The homological
//! prerequisite is landed here: a decimation chain must preserve the
//! mesh's topology across scales, and the Euler characteristic
//! chi = V-E+F on the live index soup is the deterministic check for it
//! (torus chi 0; a pinched handle reads chi != 0). The staged LOD
//! generator (trial #800's consumer) uses this as its acceptance guard:
//! any scale whose chi differs from the source is rejected, not shipped.
//!
//! Soup-based by construction: dead vertices and degenerate triangles
//! (which the decimator leaves behind by design) are excluded, so the
//! number measures the mesh the renderer would actually draw.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace warploom::core {

//! Euler characteristic of the live index soup: V = referenced verts,
//! E = unique edges of live (non-degenerate) triangles, F = live count.
//! Deterministic for a given input (ordered sets, no hashing).
inline long euler_chi(const std::vector<std::uint32_t>& indices) {
  std::set<std::uint32_t> verts;
  std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
  long faces = 0;
  for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
    const std::uint32_t v[3]{indices[t], indices[t + 1], indices[t + 2]};
    if (v[0] == v[1] || v[1] == v[2] || v[2] == v[0]) continue;
    ++faces;
    for (int e = 0; e < 3; ++e) {
      verts.insert(v[e]);
      std::uint32_t a = v[e];
      std::uint32_t b = v[static_cast<std::size_t>(e + 1) % 3];
      if (a > b) {
        const std::uint32_t tmp = a;
        a = b;
        b = tmp;
      }
      edges.insert({a, b});
    }
  }
  return static_cast<long>(verts.size()) -
         static_cast<long>(edges.size()) + faces;
}

}  // namespace warploom::core
