//! Trial #608: golden-ratio multiplicative spatial hashing vs the incumbent
//! uniform-grid broadphase (unordered_map keyed on int64 cell triples).
//! Both build the same cell occupancy and walk the same i-ascending
//! 27-neighbourhood; parity is the EXACT same candidate pair set (sorted
//! comparison) across scenes and scales. The question is build+query cost
//! and probe degradation, measured here, not in the proposal text.
//!
//! Determinism: xorshift64* with a fixed seed; no wall-clock input to the
//! data, only to the reported timings.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Cell = std::array<std::int64_t, 3>;
using Pair = std::pair<std::uint32_t, std::uint32_t>;

struct Body {
  float x, y, z;
  float bound;
};

std::uint64_t rng_state = 0x243F6A8885A308D3ULL;
std::uint64_t next_u64() {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1DULL;
}
float next_unit() {  // [0, 1)
  return static_cast<float>(static_cast<double>(next_u64() >> 11) *
                            (1.0 / 9007199254740992.0));
}

// --- Scenes (deterministic) -----------------------------------------------

std::vector<Body> make_uniform(std::size_t n, float span) {
  std::vector<Body> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back({next_unit() * span, next_unit() * span, next_unit() * span,
                   0.5f + next_unit()});
  }
  return out;
}

std::vector<Body> make_cluster(std::size_t n, float spread) {
  std::vector<Body> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    // Sum-of-uniforms ~ gaussian core with heavy tails (pile-up stress).
    const float cx = (next_unit() + next_unit() + next_unit()) / 3.0f;
    const float cy = (next_unit() + next_unit() + next_unit()) / 3.0f;
    const float cz = (next_unit() + next_unit() + next_unit()) / 3.0f;
    out.push_back({cx * spread, cy * spread, cz * spread, 0.5f + next_unit()});
  }
  return out;
}

std::vector<Body> make_lattice(std::size_t n, float spacing) {
  std::vector<Body> out;
  out.reserve(n);
  const std::size_t side =
      static_cast<std::size_t>(std::ceil(std::cbrt(static_cast<double>(n))));
  for (std::size_t ix = 0; ix < side && out.size() < n; ++ix) {
    for (std::size_t iy = 0; iy < side && out.size() < n; ++iy) {
      for (std::size_t iz = 0; iz < side && out.size() < n; ++iz) {
        out.push_back({static_cast<float>(ix) * spacing,
                       static_cast<float>(iy) * spacing,
                       static_cast<float>(iz) * spacing, 1.0f});
      }
    }
  }
  return out;
}

// --- Incumbent: unordered_map on cell triples (mirrors production) ---------

struct TripleHash {
  std::size_t operator()(const Cell& c) const noexcept {
    std::size_t h = 1469598103934665603ULL;
    for (const std::int64_t w : c) {
      h ^= static_cast<std::uint64_t>(w) + 0x9E3779B97F4A7C15ULL + (h << 6) +
           (h >> 2);
    }
    return h;
  }
};

std::vector<Pair> incumbent_pairs(const std::vector<Body>& bodies, float cell,
                                  double& ms_out) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto cell_of = [cell](float v) {
    return static_cast<std::int64_t>(std::floor(v / cell));
  };
  std::unordered_map<Cell, std::vector<std::uint32_t>, TripleHash> grid;
  grid.reserve(bodies.size() * 2U);
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    grid[{cell_of(bodies[i].x), cell_of(bodies[i].y),
          cell_of(bodies[i].z)}]
        .push_back(static_cast<std::uint32_t>(i));
  }
  static constexpr std::int64_t kOffsets[27][3] = {
      {-1, -1, -1}, {-1, -1, 0}, {-1, -1, 1}, {-1, 0, -1}, {-1, 0, 0},
      {-1, 0, 1},   {-1, 1, -1}, {-1, 1, 0}, {-1, 1, 1},  {0, -1, -1},
      {0, -1, 0},   {0, -1, 1},  {0, 0, -1},  {0, 0, 0},  {0, 0, 1},
      {0, 1, -1},   {0, 1, 0},   {0, 1, 1},   {1, -1, -1}, {1, -1, 0},
      {1, -1, 1},   {1, 0, -1},  {1, 0, 0},   {1, 0, 1},  {1, 1, -1},
      {1, 1, 0},    {1, 1, 1}};
  std::vector<Pair> pairs;
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    const std::int64_t cx = cell_of(bodies[i].x);
    const std::int64_t cy = cell_of(bodies[i].y);
    const std::int64_t cz = cell_of(bodies[i].z);
    for (const auto& off : kOffsets) {
      const auto it =
          grid.find({cx + off[0], cy + off[1], cz + off[2]});
      if (it == grid.end()) continue;
      for (const std::uint32_t other : it->second) {
        if (other <= i) continue;
        pairs.emplace_back(static_cast<std::uint32_t>(i), other);
      }
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  ms_out = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return pairs;
}

// --- Challenger: flat open-addressing table, golden-ratio hash -------------
//
// Cell words are mixed with Knuth's MMIX constant 2^64/phi
// (11400714819323198485ULL = 2^64 * (sqrt(5)-1)/2, the quasi-random
// golden-ratio projection of the proposal) and the top bits index a
// power-of-two table. Linear probing; one vector per occupied slot, same
// as the incumbent, so the measured delta is hash + table, not storage.

struct FlatGrid {
  struct Slot {
    Cell key{};
    std::vector<std::uint32_t> members;
    bool occupied = false;
  };
  std::vector<Slot> slots;
  unsigned shift = 0;  // index = hash >> shift
  std::uint64_t probe_total = 0;
  std::uint64_t probe_max = 0;

  static std::uint64_t mix_cell(const Cell& c) noexcept {
    constexpr std::uint64_t kPhi = 11400714819323198485ULL;
    std::uint64_t h = static_cast<std::uint64_t>(c[0]) * kPhi;
    h ^= static_cast<std::uint64_t>(c[1]) * kPhi + 0x9E3779B97F4A7C15ULL +
         (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(c[2]) * kPhi + 0x9E3779B97F4A7C15ULL +
         (h << 6) + (h >> 2);
    h *= kPhi;  // final golden-ratio avalanche
    return h;
  }

  explicit FlatGrid(std::size_t want) {
    std::size_t m = 1;
    while (m < want) m <<= 1;
    slots.resize(m);
    shift = static_cast<unsigned>(64 - __builtin_ctzll(m));
  }

  Slot& at(const Cell& key, bool create) {
    const std::uint64_t h = mix_cell(key);
    std::size_t idx = h >> shift;
    std::uint64_t probes = 0;
    for (;;) {
      ++probes;
      Slot& s = slots[idx];
      if (!s.occupied) {
        if (!create) {
          probe_total += probes;
          if (probes > probe_max) probe_max = probes;
          return s;  // unoccupied sentinel, caller checks
        }
        s.occupied = true;
        s.key = key;
        probe_total += probes;
        if (probes > probe_max) probe_max = probes;
        return s;
      }
      if (s.key == key) {
        probe_total += probes;
        if (probes > probe_max) probe_max = probes;
        return s;
      }
      idx = (idx + 1) & (slots.size() - 1U);
    }
  }

  const Slot* find(const Cell& key) {
    Slot& s = at(key, false);
    return s.occupied ? &s : nullptr;
  }
};

std::vector<Pair> challenger_pairs(const std::vector<Body>& bodies, float cell,
                                   double& ms_out, std::uint64_t& probe_max,
                                   double& mean_probe) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto cell_of = [cell](float v) {
    return static_cast<std::int64_t>(std::floor(v / cell));
  };
  FlatGrid grid(bodies.size() * 4U + 16U);
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    grid.at({cell_of(bodies[i].x), cell_of(bodies[i].y),
             cell_of(bodies[i].z)},
            true)
        .members.push_back(static_cast<std::uint32_t>(i));
  }
  static constexpr std::int64_t kOffsets[27][3] = {
      {-1, -1, -1}, {-1, -1, 0}, {-1, -1, 1}, {-1, 0, -1}, {-1, 0, 0},
      {-1, 0, 1},   {-1, 1, -1}, {-1, 1, 0}, {-1, 1, 1},  {0, -1, -1},
      {0, -1, 0},   {0, -1, 1},  {0, 0, -1},  {0, 0, 0},  {0, 0, 1},
      {0, 1, -1},   {0, 1, 0},   {0, 1, 1},   {1, -1, -1}, {1, -1, 0},
      {1, -1, 1},   {1, 0, -1},  {1, 0, 0},   {1, 0, 1},  {1, 1, -1},
      {1, 1, 0},    {1, 1, 1}};
  std::vector<Pair> pairs;
  std::uint64_t lookups = 0;
  const std::uint64_t probes_before = grid.probe_total;
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    const std::int64_t cx = cell_of(bodies[i].x);
    const std::int64_t cy = cell_of(bodies[i].y);
    const std::int64_t cz = cell_of(bodies[i].z);
    for (const auto& off : kOffsets) {
      ++lookups;
      const FlatGrid::Slot* s =
          grid.find({cx + off[0], cy + off[1], cz + off[2]});
      if (s == nullptr) continue;
      for (const std::uint32_t other : s->members) {
        if (other <= i) continue;
        pairs.emplace_back(static_cast<std::uint32_t>(i), other);
      }
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  ms_out = std::chrono::duration<double, std::milli>(t1 - t0).count();
  probe_max = grid.probe_max;
  mean_probe =
      static_cast<double>(grid.probe_total - probes_before) /
      static_cast<double>(lookups == 0 ? 1 : lookups);
  return pairs;
}

// --- Trial ---------------------------------------------------------------

void run_case(const char* name, const std::vector<Body>& bodies) {
  float max_bound = 0.0f;
  for (const Body& b : bodies) max_bound = std::max(max_bound, b.bound);
  const float cell = max_bound * 2.0f;

  double ms_inc = 0.0, ms_ch = 0.0;
  std::uint64_t probe_max = 0;
  double mean_probe = 0.0;
  auto inc = incumbent_pairs(bodies, cell, ms_inc);
  auto ch = challenger_pairs(bodies, cell, ms_ch, probe_max, mean_probe);
  // Order parity FIRST: production demands all-pairs visit order (grouped by
  // i), not just the same set. Both walks are textually identical, so the
  // emission order must match exactly -- a reorder here fails the trial.
  EXPECT_EQ(ch, inc) << "emission ORDER differs for " << name;
  std::sort(inc.begin(), inc.end());
  std::sort(ch.begin(), ch.end());

  std::uint64_t checksum = 0;
  for (const auto& p : ch) checksum += p.first * 31ULL + p.second;

  std::printf(
      "[hashgrid] %-14s n=%5zu cell=%.1f pairs=%6zu | "
      "incumbent=%7.2fms challenger=%7.2fms ratio=%.2fx | "
      "probe_max=%llu probe_mean=%.2f (sink %llu)\n",
      name, bodies.size(), static_cast<double>(cell), ch.size(), ms_inc,
      ms_ch,
      ms_inc / (ms_ch > 0.0 ? ms_ch : 1.0),
      static_cast<unsigned long long>(probe_max),
      static_cast<double>(mean_probe),
      static_cast<unsigned long long>(checksum));

  EXPECT_EQ(ch, inc) << "candidate pair set differs for " << name;
  EXPECT_EQ(checksum, checksum);  // anti-DCE anchor, documents not gates
}

TEST(GoldenHashGridTrial, Uniform1k) {
  run_case("uniform", make_uniform(1000, 60.0f));
}

TEST(GoldenHashGridTrial, Uniform4k) {
  run_case("uniform", make_uniform(4000, 95.0f));
}

TEST(GoldenHashGridTrial, Uniform16k) {
  run_case("uniform", make_uniform(16000, 150.0f));
}

TEST(GoldenHashGridTrial, Cluster4k) {
  run_case("cluster", make_cluster(4000, 40.0f));
}

TEST(GoldenHashGridTrial, Lattice4k) {
  run_case("lattice", make_lattice(4096, 1.2f));
}

}  // namespace
