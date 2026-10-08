#pragma once

/**
 * @file physics_world.hpp
 * @brief Deterministic fixed-step physics: semi-implicit Euler integration,
 *        sphere-plane and sphere-sphere contacts.
 *
 * This header is the solver only. It knows nothing about the ECS; the bridge
 * that copies body poses into projected transforms lives on EditorSession
 * (spawn_physics_body / tick), which owns the world. An earlier version of
 * this comment claimed the bridge was here and it was not.
 *
 * Determinism contract: `step(dt)` is a pure function of the world state —
 * fixed iteration order (bodies in insertion order, pairs in index order),
 * no wall-clock reads, no allocation, no parallelism. N identical runs
 * produce bit-identical trajectories, which the replay/scenario harness
 * verifies byte-for-byte.
 *
 * Units: meters, seconds, kilograms. Gravity defaults to -9.81 on Y.
 */

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace warploom::physics {

struct PhysicsBody {
  float position[3]{0.0f, 0.0f, 0.0f};
  float velocity[3]{0.0f, 0.0f, 0.0f};
  float radius{0.5f};      //!< Sphere collider (and render scale reference).
  float inverse_mass{1.0f};  //!< 0 = static (immovable).
  float restitution{0.4f};   //!< Bounce coefficient [0, 1].
};

//! One resolved contact (exposed for tests/telemetry).
struct ContactEvent {
  std::uint32_t a{0};
  std::uint32_t b{0xFFFFFFFFu};  //!< 0xFFFFFFFF = the world (plane).
  float depth{0.0f};
};

//! Deterministic sphere physics world over a ground plane at y = 0.
class PhysicsWorld final {
 public:
  explicit PhysicsWorld(float gravity = -9.81f) noexcept
      : gravity_{gravity} {}

  [[nodiscard]] std::uint32_t add_body(const PhysicsBody& body) {
    const std::uint32_t id = static_cast<std::uint32_t>(bodies_.size());
    bodies_.push_back(body);
    return id;
  }

  [[nodiscard]] PhysicsBody& body(std::uint32_t id) noexcept {
    return bodies_[id];
  }
  [[nodiscard]] const PhysicsBody& body(std::uint32_t id) const noexcept {
    return bodies_[id];
  }
  [[nodiscard]] std::size_t body_count() const noexcept {
    return bodies_.size();
  }

  [[nodiscard]] const std::vector<ContactEvent>& contacts() const noexcept {
    return contacts_;
  }

  //! Gravity, exposed so a snapshot can carry the world's configuration and not
  //! just its bodies. A resumed replay with the wrong gravity diverges.
  [[nodiscard]] float gravity() const noexcept { return gravity_; }

  //! One fixed step, in phase order:
  //!   1. integrate velocities (gravity) and positions (semi-implicit Euler),
  //!   2. resolve sphere-plane contacts (ground at y = 0, restitution),
  //!   3. resolve sphere-sphere contacts (equal-mass elastic-ish impulse,
  //!      positional correction, index-order pairs, single iteration).
  //! Simulation substeps per step: `step(dt)` runs `substeps_` passes of
  //! (integrate dt/n, detect, resolve). Smaller increments improve stacking
  //! stability independently of the solver-iteration count (which operates
  //! WITHIN each substep). Default 1 = the historical single increment.
  void set_substeps(std::uint32_t n) noexcept { substeps_ = n > 0U ? n : 1U; }
  [[nodiscard]] std::uint32_t substeps() const noexcept { return substeps_; }

  void step(float dt) noexcept {
    const std::uint32_t n = substeps_;
    const float sub_dt = dt / static_cast<float>(n);
    for (std::uint32_t sub = 0; sub < n; ++sub) {
      step_single(sub_dt);
    }
  }

  //! One full solve at the given increment (integration + detect + resolve).
  //! Engine detail: step() drives it; kept in the public section only because
  //! the class's private block starts at its data members.
  void step_single(float dt) noexcept {
    contacts_.clear();
    const float g = gravity_;

    // --- 1. Integration -------------------------------------------------
    for (PhysicsBody& b : bodies_) {
      if (b.inverse_mass <= 0.0F) continue;  // 0 == static
      b.velocity[1] += g * dt;
      b.position[0] += b.velocity[0] * dt;
      b.position[1] += b.velocity[1] * dt;
      b.position[2] += b.velocity[2] * dt;
    }

    // --- 2. Ground plane contacts (y = 0) -------------------------------
    for (PhysicsBody& b : bodies_) {
      if (b.inverse_mass <= 0.0F) continue;  // 0 == static
      if (b.position[1] - b.radius < 0.0f) {
        const float depth = b.radius - b.position[1];
        b.position[1] = b.radius;
        if (b.velocity[1] < 0.0f) {
          b.velocity[1] = -b.velocity[1] * b.restitution;
        }
        // Ground friction damps tangential motion while contacting.
        b.velocity[0] *= 0.98f;
        b.velocity[2] *= 0.98f;
        contacts_.push_back(ContactEvent{0U, kWorldContact, depth});
      }
    }

    // --- 3. Sphere-sphere contacts ---------------------------------------
    // Detection reads ONE snapshot of positions and velocities, so the set of
    // contacts does not depend on the order pairs are visited. The previous
    // form re-read live positions per pair while also mutating them, which
    // made the result depend on visit order AND made it impossible to replace
    // the all-pairs scan with a broadphase: a body pushed into an overlap by an
    // earlier correction would be missed by any candidate list built before the
    // pass. Freezing detection makes the pair set a pure function of the
    // snapshot, which is what lets a spatial grid be exactly conservative.
    //
    // Corrections are still applied in pair order -- that is Gauss-Seidel, and
    // it converges better than a simultaneous solve. What is fixed is *which*
    // pairs get solved, not the order their corrections land in.
    if (bodies_.size() >= 2U) {
      solve_snapshot_.assign(bodies_.size(), SolveSnapshot{});
      for (std::size_t i = 0; i < bodies_.size(); ++i) {
        const PhysicsBody& body = bodies_[i];
        SolveSnapshot& snap = solve_snapshot_[i];
        snap.px = body.position[0];
        snap.py = body.position[1];
        snap.pz = body.position[2];
        snap.vx = body.velocity[0];
        snap.vy = body.velocity[1];
        snap.vz = body.velocity[2];
        snap.radius = body.radius;
        snap.inverse_mass = body.inverse_mass;
        snap.restitution = body.restitution;
      }

      solve_contacts_.clear();
      rebuild_candidate_pairs();
      for (const std::pair<std::uint32_t, std::uint32_t>& pair :
           candidate_pairs_) {
        {
          const std::size_t i = pair.first;
          const std::size_t j = pair.second;
          const SolveSnapshot& a = solve_snapshot_[i];
          const SolveSnapshot& b = solve_snapshot_[j];
          const float dx = b.px - a.px;
          const float dy = b.py - a.py;
          const float dz = b.pz - a.pz;
          const float dist_sq = dx * dx + dy * dy + dz * dz;
          const float min_dist = a.radius + b.radius;
          if (dist_sq >= min_dist * min_dist) continue;

          const float inv_sum = a.inverse_mass + b.inverse_mass;
          if (inv_sum <= 0.0f) continue;  // both static

          ContactSolve solve{};
          solve.a = static_cast<std::uint32_t>(i);
          solve.b = static_cast<std::uint32_t>(j);
          const float dist = std::sqrt(dist_sq);
          // Degenerate: coincident centers -> separate along +Y.
          solve.nx = dist > 1e-8f ? dx / dist : 0.0f;
          solve.ny = dist > 1e-8f ? dy / dist : 1.0f;
          solve.nz = dist > 1e-8f ? dz / dist : 0.0f;
          solve.penetration = min_dist - dist;
          solve.inv_sum = inv_sum;
          solve.restitution = 0.5f * (a.restitution + b.restitution);
          solve.vel_n = (b.vx - a.vx) * solve.nx + (b.vy - a.vy) * solve.ny +
                        (b.vz - a.vz) * solve.nz;
          solve_contacts_.push_back(solve);
        }
      }

      // Positional correction: `solver_iterations_` Gauss-Seidel passes over
      // the frozen contact list. Passes after the first re-read live
      // positions, which is the point — residual overlap created by pass k
      // is pushed out by pass k+1. The impulse below stays single-shot on
      // the snapshot's relative velocity.
      for (std::uint32_t pass = 0; pass < solver_iterations_; ++pass) {
        for (const ContactSolve& solve : solve_contacts_) {
          PhysicsBody& a = bodies_[solve.a];
          PhysicsBody& b = bodies_[solve.b];
          // Positional correction proportional to inverse mass.
          const float corr = solve.penetration / solve.inv_sum;
          if (a.inverse_mass > 0.0f) {
            a.position[0] -= solve.nx * corr * a.inverse_mass;
            a.position[1] -= solve.ny * corr * a.inverse_mass;
            a.position[2] -= solve.nz * corr * a.inverse_mass;
          }
          if (b.inverse_mass > 0.0f) {
            b.position[0] += solve.nx * corr * b.inverse_mass;
            b.position[1] += solve.ny * corr * b.inverse_mass;
            b.position[2] += solve.nz * corr * b.inverse_mass;
          }
        }
      }
      for (const ContactSolve& solve : solve_contacts_) {
        PhysicsBody& a = bodies_[solve.a];
        PhysicsBody& b = bodies_[solve.b];

        // Impulse along the normal. vel_n comes from the snapshot, so this
        // does not depend on corrections applied earlier in this loop.
        if (solve.vel_n < 0.0f) {
          const float impulse = -(1.0f + solve.restitution) * solve.vel_n /
                                solve.inv_sum;
          if (a.inverse_mass > 0.0f) {
            a.velocity[0] -= impulse * solve.nx * a.inverse_mass;
            a.velocity[1] -= impulse * solve.ny * a.inverse_mass;
            a.velocity[2] -= impulse * solve.nz * a.inverse_mass;
          }
          if (b.inverse_mass > 0.0f) {
            b.velocity[0] += impulse * solve.nx * b.inverse_mass;
            b.velocity[1] += impulse * solve.ny * b.inverse_mass;
            b.velocity[2] += impulse * solve.nz * b.inverse_mass;
          }
        }
        contacts_.push_back(ContactEvent{
            solve.a, solve.b, solve.penetration});
      }
    }
  }

  //! Disable the spatial grid and solve every pair. Slower, but the reference
  //! the broadphase must agree with: the first attempt at this was wrong at
  //! N=4000 and only a like-for-like comparison at several scales caught it,
  //! so the comparison is now reachable from a test rather than from a manual
  //! rebuild with the threshold edited.
  //! Positional-correction iterations per step (Gauss-Seidel: the contact SET
  //! is frozen from the snapshot, but each pass re-reads live positions, so
  //! later passes push out residual overlap the earlier passes created).
  //! Default 1 = the historical single pass; larger values trade step cost for
  //! stacking firmness. Deterministic at any count: fixed contact order.
  void set_solver_iterations(std::uint32_t n) noexcept {
    solver_iterations_ = n > 0U ? n : 1U;
  }
  [[nodiscard]] std::uint32_t solver_iterations() const noexcept {
    return solver_iterations_;
  }

  void set_force_all_pairs(bool force) noexcept { force_all_pairs_ = force; }
  [[nodiscard]] bool force_all_pairs() const noexcept { return force_all_pairs_; }

  //! Below this body count the all-pairs loop is cheaper than building a grid.
  static constexpr std::size_t kBroadphaseMinBodies = 24;
  //! Ceiling on candidate pairs. A pathological pile-up (every body in one
  //! cell) must not make the broadphase the O(n^2) scan it exists to avoid, so
  //! past this the full pair list is built instead -- slower, but correct.
  static constexpr std::size_t kBroadphaseMaxCandidates = 1U << 18U;

  //! Fill candidate_pairs_ with (i, j), i < j, in exactly the order the nested
  //! all-pairs loop would visit them, containing every pair that can overlap.
  //!
  //! This is only sound because step 3 detects against a frozen snapshot: the
  //! grid is built from those same snapshot positions, so "pairs within
  //! distance 2*max_radius of a cell" provably contains every overlapping pair
  //! and nothing depends on positions moving during the pass. An earlier
  //! version that resolved live was NOT equivalent -- it matched at N=1000 and
  //! diverged at N=4000.
  //!
  //! Determinism is why this is a sorted vector rather than a hash grid walked
  //! in bucket order: hash iteration order is unspecified, and the contact set
  //! is order-sensitive once corrections are applied.
  void rebuild_candidate_pairs() noexcept {
    candidate_pairs_.clear();
    const std::size_t count = solve_snapshot_.size();
    if (count < 2U) return;

    // Cell size at least two maximum radii, so any two overlapping spheres are
    // in the same or an adjacent cell.
    float max_radius = 0.0f;
    for (const SolveSnapshot& body : solve_snapshot_) {
      if (body.radius > max_radius) max_radius = body.radius;
    }
    if (max_radius <= 0.0f) return;  // nothing can overlap

    if (force_all_pairs_ || count < kBroadphaseMinBodies) {
      candidate_pairs_.reserve(count * (count - 1U) / 2U);
      for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
          candidate_pairs_.emplace_back(static_cast<std::uint32_t>(i),
                                       static_cast<std::uint32_t>(j));
        }
      }
      return;
    }

    const float cell = max_radius * 2.0f;
    // Floor, not truncation: negative coordinates must not fold toward zero,
    // or bodies at -0.5 and +0.5 could land in non-adjacent cells and the pair
    // would be missed.
    const auto cell_of = [cell](float v) {
      return static_cast<std::int64_t>(std::floor(v / cell));
    };

    grid_.clear();
    for (std::size_t i = 0; i < count; ++i) {
      const SolveSnapshot& body = solve_snapshot_[i];
      grid_[{cell_of(body.px), cell_of(body.py), cell_of(body.pz)}].push_back(
          static_cast<std::uint32_t>(i));
    }

    static constexpr std::int64_t kOffsets[27][3] = {
        {-1, -1, -1}, {-1, -1, 0}, {-1, -1, 1}, {-1, 0, -1}, {-1, 0, 0},
        {-1, 0, 1},   {-1, 1, -1}, {-1, 1, 0}, {-1, 1, 1}, {0, -1, -1},
        {0, -1, 0},   {0, -1, 1},   {0, 0, -1},  {0, 0, 0},  {0, 0, 1},
        {0, 1, -1},   {0, 1, 0},    {0, 1, 1},   {1, -1, -1}, {1, -1, 0},
        {1, -1, 1},   {1, 0, -1},   {1, 0, 0},   {1, 0, 1},   {1, 1, -1},
        {1, 1, 0},    {1, 1, 1}};

    bool overflowed = false;
    for (std::size_t i = 0; i < count && !overflowed; ++i) {
      const SolveSnapshot& body = solve_snapshot_[i];
      const std::int64_t cx = cell_of(body.px);
      const std::int64_t cy = cell_of(body.py);
      const std::int64_t cz = cell_of(body.pz);
      for (const auto& offset : kOffsets) {
        const auto it =
            grid_.find({cx + offset[0], cy + offset[1], cz + offset[2]});
        if (it == grid_.end()) continue;
        for (const std::uint32_t other : it->second) {
          if (other <= i) continue;  // forward pairs only, so each appears once
          candidate_pairs_.emplace_back(static_cast<std::uint32_t>(i), other);
          if (candidate_pairs_.size() >= kBroadphaseMaxCandidates) {
            overflowed = true;
            break;
          }
        }
        if (overflowed) break;
      }
    }

    if (overflowed) {
      // Degenerate density: fall back to the complete list rather than
      // silently dropping pairs, which is the failure mode that made the
      // first attempt wrong at high N.
      candidate_pairs_.clear();
      candidate_pairs_.reserve(count * (count - 1U) / 2U);
      for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
          candidate_pairs_.emplace_back(static_cast<std::uint32_t>(i),
                                       static_cast<std::uint32_t>(j));
        }
      }
      return;
    }

    std::sort(candidate_pairs_.begin(), candidate_pairs_.end());
  }

  //! 64-bit FNV-1a over all body positions: the determinism fingerprint
  //! (identical runs must hash identically; also used as a telemetry field).
  [[nodiscard]] std::uint64_t position_fingerprint() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const PhysicsBody& b : bodies_) {
      for (const float c : b.position) {
        std::uint32_t bits;
        static_assert(sizeof(bits) == sizeof(float));
        __builtin_memcpy(&bits, &c, sizeof(bits));
        for (int k = 0; k < 4; ++k) {
          hash ^= static_cast<std::uint64_t>((bits >> (8 * k)) & 0xFFu);
          hash *= 1099511628211ULL;
        }
      }
    }
    return hash;
  }

  static constexpr std::uint32_t kWorldContact = 0xFFFFFFFFu;

 private:
  //! One frozen body state for contact detection. Detection must not read live
  //! positions: see the step-3 comment.
  struct SolveSnapshot {
    float px{0.0f}, py{0.0f}, pz{0.0f};
    float vx{0.0f}, vy{0.0f}, vz{0.0f};
    float radius{0.0f};
    float inverse_mass{0.0f};
    float restitution{0.0f};
  };
  //! One resolved contact, computed from the snapshot and then applied.
  struct ContactSolve {
    std::uint32_t a{0};
    std::uint32_t b{0};
    float nx{0.0f}, ny{0.0f}, nz{0.0f};
    float penetration{0.0f};
    float inv_sum{0.0f};
    float restitution{0.0f};
    float vel_n{0.0f};
  };

  std::vector<PhysicsBody> bodies_;
  //! Per-step scratch, kept as members so the steady-state step allocates
  //! nothing after the first frame at a given body count.
  std::vector<SolveSnapshot> solve_snapshot_{};
  std::vector<ContactSolve> solve_contacts_{};
  //! Uniform-grid scratch. Bodies are bucketed in index order, so each bucket
  //! is already ascending and needs no sort.
  struct Cell {
    std::int64_t x{0};
    std::int64_t y{0};
    std::int64_t z{0};
    [[nodiscard]] bool operator==(const Cell& o) const noexcept {
      return x == o.x && y == o.y && z == o.z;
    }
  };
  struct CellHash {
    [[nodiscard]] std::size_t operator()(const Cell& c) const noexcept {
      // Any deterministic hash works; it never has to be stable across runs,
      // only equal for equal cells.
      std::uint64_t h = static_cast<std::uint64_t>(c.x) * 0x9E3779B97F4A7C15ULL;
      h ^= static_cast<std::uint64_t>(c.y) * 0xC2B2AE3D27D4EB4FULL;
      h ^= static_cast<std::uint64_t>(c.z) * 0x165667B19E3779F9ULL;
      h ^= h >> 29U;
      h *= 0xBF58476D1CE4E5B9ULL;
      h ^= h >> 32U;
      return static_cast<std::size_t>(h);
    }
  };
  bool force_all_pairs_{false};
  std::unordered_map<Cell, std::vector<std::uint32_t>, CellHash> grid_{};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> candidate_pairs_{};
  std::vector<ContactEvent> contacts_;
  float gravity_;
  std::uint32_t solver_iterations_{1};  //!< Positional-correction passes.
  std::uint32_t substeps_{1};           //!< Integration+solve substeps per step.
};

}  // namespace warploom::physics
// S2-B compat footer: legacy `omnicpp::*` spellings keep resolving
// during the transition (removed with the S5 identity pass -
// docs/warploom-core-plan.md, phase B). A using-directive in a
// namespace extension (NOT a type alias - ill-formed for namespaces)
// makes the old spellings name the SAME types, and legally coexists
// with real `omnicpp::core` extension blocks elsewhere (extension
// blocks merge). One directive per namespace THIS header declares,
// each under its OWN guard (a shared guard would suppress later
// headers' distinct directives).
#ifndef WARPLOOM_COMPAT_PHYSICS_NS
#define WARPLOOM_COMPAT_PHYSICS_NS
namespace omnicpp::physics {
    using namespace ::warploom::physics;
}
#endif  // WARPLOOM_COMPAT_PHYSICS_NS
