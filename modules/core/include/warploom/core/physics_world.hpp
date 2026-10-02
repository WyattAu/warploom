#pragma once

/**
 * @file physics_world.hpp
 * @brief Deterministic fixed-step physics: semi-implicit Euler integration,
 *        sphere-plane and sphere-sphere contacts, and an ECS bridge that
 *        copies body poses into SceneTransform components.
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
#include <cstdint>
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

  //! One fixed step, in phase order:
  //!   1. integrate velocities (gravity) and positions (semi-implicit Euler),
  //!   2. resolve sphere-plane contacts (ground at y = 0, restitution),
  //!   3. resolve sphere-sphere contacts (equal-mass elastic-ish impulse,
  //!      positional correction, index-order pairs, single iteration).
  void step(float dt) noexcept {
    contacts_.clear();
    const float g = gravity_;

    // --- 1. Integration -------------------------------------------------
    for (PhysicsBody& b : bodies_) {
      if (b.inverse_mass == 0.0f) continue;
      b.velocity[1] += g * dt;
      b.position[0] += b.velocity[0] * dt;
      b.position[1] += b.velocity[1] * dt;
      b.position[2] += b.velocity[2] * dt;
    }

    // --- 2. Ground plane contacts (y = 0) -------------------------------
    for (PhysicsBody& b : bodies_) {
      if (b.inverse_mass == 0.0f) continue;
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

    // --- 3. Sphere-sphere contacts (index-order pairs) -------------------
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
      PhysicsBody& a = bodies_[i];
      for (std::size_t j = i + 1; j < bodies_.size(); ++j) {
        PhysicsBody& b = bodies_[j];
        const float dx = b.position[0] - a.position[0];
        const float dy = b.position[1] - a.position[1];
        const float dz = b.position[2] - a.position[2];
        const float dist_sq = dx * dx + dy * dy + dz * dz;
        const float min_dist = a.radius + b.radius;
        if (dist_sq >= min_dist * min_dist) continue;

        const float dist = std::sqrt(dist_sq);
        // Degenerate: coincident centers -> separate along +Y.
        const float nx = dist > 1e-8f ? dx / dist : 0.0f;
        const float ny = dist > 1e-8f ? dy / dist : 1.0f;
        const float nz = dist > 1e-8f ? dz / dist : 0.0f;
        const float penetration = min_dist - dist;

        const float inv_a = a.inverse_mass;
        const float inv_b = b.inverse_mass;
        const float inv_sum = inv_a + inv_b;
        if (inv_sum <= 0.0f) continue;  // both static

        // Positional correction proportional to inverse mass.
        const float corr = penetration / inv_sum;
        if (inv_a > 0.0f) {
          a.position[0] -= nx * corr * inv_a;
          a.position[1] -= ny * corr * inv_a;
          a.position[2] -= nz * corr * inv_a;
        }
        if (inv_b > 0.0f) {
          b.position[0] += nx * corr * inv_b;
          b.position[1] += ny * corr * inv_b;
          b.position[2] += nz * corr * inv_b;
        }

        // Impulse along the normal (restitution-averaged, equal treatment).
        const float rvx = b.velocity[0] - a.velocity[0];
        const float rvy = b.velocity[1] - a.velocity[1];
        const float rvz = b.velocity[2] - a.velocity[2];
        const float vel_n = rvx * nx + rvy * ny + rvz * nz;
        if (vel_n < 0.0f) {
          const float e =
              0.5f * (a.restitution + b.restitution);
          const float j = -(1.0f + e) * vel_n / inv_sum;
          if (inv_a > 0.0f) {
            a.velocity[0] -= j * nx * inv_a;
            a.velocity[1] -= j * ny * inv_a;
            a.velocity[2] -= j * nz * inv_a;
          }
          if (inv_b > 0.0f) {
            b.velocity[0] += j * nx * inv_b;
            b.velocity[1] += j * ny * inv_b;
            b.velocity[2] += j * nz * inv_b;
          }
        }
        contacts_.push_back(ContactEvent{
            static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j),
            penetration});
      }
    }
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
  std::vector<PhysicsBody> bodies_;
  std::vector<ContactEvent> contacts_;
  float gravity_;
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
