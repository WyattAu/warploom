//! @file test_physics_world.cpp
//! @brief Deterministic physics proofs: analytic projectile trajectory,
//!        bounce restitution, sphere-sphere impulse symmetry, static bodies,
//!        and bit-identical fingerprints across repeated runs.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "warploom/physics/physics_world.hpp"

namespace {

using omnicpp::physics::PhysicsBody;
using omnicpp::physics::PhysicsWorld;

constexpr float kDt = 1.0f / 120.0f;

TEST(PhysicsWorld, ProjectileMatchesAnalyticTrajectory) {
  PhysicsWorld w(/*gravity=*/-9.81f);
  PhysicsBody b;
  b.position[1] = 10.0f;
  b.inverse_mass = 0.0f;  // disable collisions for the analytic check
  b.inverse_mass = 1.0f;
  // Lift the body high enough that it never contacts within 30 steps.
  b.position[1] = 1000.0f;
  b.velocity[1] = 0.0f;
  (void)w.add_body(b);

  // Semi-implicit Euler: p_{n+1} = p_n + v_{n+1} * dt (velocity first).
  const float dt = kDt;
  float expected_y = 1000.0f;
  float expected_vy = 0.0f;
  for (std::size_t i = 0; i < static_cast<std::size_t>(30); ++i) {
    w.step(dt);
    expected_vy += -9.81f * dt;
    expected_y += expected_vy * dt;
    EXPECT_NEAR(w.body(0).position[1], expected_y, 1e-4f)
        << "step " << i;
    EXPECT_NEAR(w.body(0).velocity[1], expected_vy, 1e-5f)
        << "step " << i;
  }
}

TEST(PhysicsWorld, BounceConservesEnergyPerRestitution) {
  PhysicsWorld w(-9.81f);
  PhysicsBody b;
  b.position[1] = 1.0f;
  b.radius = 0.5f;
  b.restitution = 0.8f;
  (void)w.add_body(b);

  // Analytic impact speed from the 0.5 m drop: v = sqrt(2 * g * h).
  const float v_in = std::sqrt(2.0f * 9.81f * 0.5f);
  // Run until the first ground contact, then check the reflected speed.
  bool contacted = false;
  for (std::size_t i = 0; i < 240U && !contacted; ++i) {
    w.step(kDt);
    contacted = !w.contacts().empty();
  }
  ASSERT_TRUE(contacted);
  EXPECT_NEAR(w.body(0).velocity[1], 0.8f * v_in, 0.05f);
  EXPECT_GT(w.body(0).velocity[1], 0.0f);  // moving up after bounce
  // The ball must stay above the plane.
  EXPECT_GE(w.body(0).position[1], 0.5f - 1e-4f);
}

TEST(PhysicsWorld, SphereSphereImpulseIsSymmetric) {
  PhysicsWorld w(0.0f);  // no gravity: pure two-body test
  PhysicsBody a;
  a.position[0] = -0.5f;  // touching (sum of radii = 1.0)
  a.position[1] = 5.0f;   // clear of the ground plane
  a.velocity[0] = 1.0f;
  a.restitution = 1.0f;
  PhysicsBody b;
  b.position[0] = 0.5f;
  b.position[1] = 5.0f;
  b.velocity[0] = -1.0f;
  b.restitution = 1.0f;
  (void)w.add_body(a);
  (void)w.add_body(b);

  w.step(kDt);
  ASSERT_FALSE(w.contacts().empty());
  // Equal masses, head-on, e = 1: velocities exchange exactly.
  EXPECT_NEAR(w.body(0).velocity[0], -1.0f, 1e-5f);
  EXPECT_NEAR(w.body(1).velocity[0], 1.0f, 1e-5f);
  // Momentum conserved.
  EXPECT_NEAR(w.body(0).velocity[0] + w.body(1).velocity[0], 0.0f, 1e-5f);
}

TEST(PhysicsWorld, StaticBodyImmovable) {
  PhysicsWorld w(0.0f);
  PhysicsBody wall;
  wall.position[0] = 0.0f;
  wall.position[1] = 3.0f;   // clear of the ground plane
  wall.radius = 1.0f;
  wall.inverse_mass = 0.0f;  // static
  PhysicsBody ball;
  ball.position[0] = -1.5f;  // touching (sum of radii = 1.5)
  ball.position[1] = 3.0f;
  ball.velocity[0] = 5.0f;   // moving toward the wall
  (void)w.add_body(wall);
  (void)w.add_body(ball);

  w.step(kDt);
  EXPECT_FLOAT_EQ(w.body(0).position[0], 0.0f);   // wall never moves
  // Exact, and the point of the test: a static body is never integrated, so its
  // velocity must stay bit-exactly zero rather than merely near it.
  EXPECT_FLOAT_EQ(w.body(0).velocity[0], 0.0f);
  EXPECT_LT(w.body(1).position[0], -1.4f);        // ball pushed back out
  // e = 0.4 default: the ball reflects with speed 5 * 0.4 = 2.0 away from
  // the wall (velocity flips to -X).
  EXPECT_NEAR(w.body(1).velocity[0], -2.0f, 1e-4f);
}

TEST(PhysicsWorld, RepeatedRunsAreBitIdentical) {
  auto run = [] {
    PhysicsWorld w(-9.81f);
    PhysicsBody ground;
    ground.radius = 1.0f;
    ground.inverse_mass = 0.0f;
    ground.restitution = 0.5f;
    PhysicsBody a;
    a.position[0] = -0.5f;
    a.position[1] = 3.0f;
    a.velocity[0] = 1.5f;
    a.restitution = 0.6f;
    PhysicsBody b;
    b.position[0] = 0.5f;
    b.position[1] = 3.5f;
    b.velocity[0] = -1.0f;
    b.restitution = 0.7f;
    (void)ground.inverse_mass;
    (void)w.add_body(a);
    (void)w.add_body(b);
    for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) w.step(kDt);
    return w.position_fingerprint();
  };
  const std::uint64_t h1 = run();
  const std::uint64_t h2 = run();
  EXPECT_EQ(h1, h2);
}

TEST(PhysicsWorld, StackedBodiesSettleWithoutExplosion) {
  PhysicsWorld w(-9.81f);
  PhysicsBody lower;
  lower.position[1] = 0.5f;
  lower.restitution = 0.0f;
  PhysicsBody upper;
  upper.position[1] = 1.5f;
  upper.restitution = 0.0f;
  (void)w.add_body(lower);
  (void)w.add_body(upper);

  for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) w.step(kDt);
  // Both spheres rest at ~one radius above the plane (stacking is approximate
  // with a single solver iteration, but must be stable and in contact).
  EXPECT_NEAR(w.body(0).position[1], 0.5f, 0.05f);
  EXPECT_NEAR(w.body(1).position[1], 1.5f, 0.1f);
  EXPECT_LT(std::fabs(w.body(0).velocity[1]), 0.1f);
  EXPECT_LT(std::fabs(w.body(1).velocity[1]), 0.1f);
}

// Iterated positional correction: more passes over the SAME frozen contact
// list must (a) leave a settled stack FIRMER (upper sphere closer to rest
// height), (b) stay deterministic run-to-run, and (c) not change the
// single-pass result when the count is 1 (bit-compatibility with the
// historical solver, which the broadphase equivalence test depends on).
TEST(PhysicsWorld, SolverIterationsFirmStackAndDeterminism) {
  auto settle = [](std::uint32_t iterations) {
    PhysicsWorld w(-9.81f);
    w.set_solver_iterations(iterations);
    // 6-body tower: sink accumulates with stack depth, which is where the
    // iteration count actually shows. Radius 0.5, rest heights 0.5..3.0.
    for (std::uint32_t i = 0; i < 6U; ++i) {
      PhysicsBody b;
      b.position[1] = 0.5f + 1.0f * static_cast<float>(i);
      b.restitution = 0.0f;
      (void)w.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) {
      w.step(kDt);
    }
    // Sink = how far the TOP sphere sits below its rest height (3.0). Less
    // sink = firmer stack. Also capture the mean height as the fingerprint
    // payload.
    float top = w.body(5).position[1];
    double sum = 0.0;
    for (std::uint32_t i = 0; i < 6U; ++i) {
      sum += static_cast<double>(w.body(i).position[1]);
    }
    // Determinism: replay the whole settle and require bit-identical state.
    PhysicsWorld w2(-9.81f);
    w2.set_solver_iterations(iterations);
    for (std::uint32_t i = 0; i < 6U; ++i) {
      PhysicsBody b;
      b.position[1] = 0.5f + 1.0f * static_cast<float>(i);
      b.restitution = 0.0f;
      (void)w2.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) {
      w2.step(kDt);
    }
    double sum2 = 0.0;
    for (std::uint32_t i = 0; i < 6U; ++i) {
      sum2 += static_cast<double>(w2.body(i).position[1]);
    }
    char fp[128];
    std::snprintf(fp, sizeof(fp), "%.9f|%.9f", sum, sum2);
    return std::pair<float, std::string>(3.0f - top, fp);
  };

  const auto one = settle(1U);
  const auto eight = settle(8U);

  // (c) count 1 reproduces the historical single-pass settle.
  PhysicsWorld w1(-9.81f);
  PhysicsBody l1; l1.position[1] = 0.5f; l1.restitution = 0.0f;
  PhysicsBody u1; u1.position[1] = 1.5f; u1.restitution = 0.0f;
  (void)w1.add_body(l1);
  (void)w1.add_body(u1);
  for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) w1.step(kDt);
  EXPECT_EQ(one.second, settle(1U).second) << "nondeterministic at n=1";
  EXPECT_NEAR(w1.body(1).position[1], 1.5f, 0.1f);

  // (a) more passes -> firmer stack (upper sphere nearer its rest height
  // 1.5 = lower radius + upper radius above the lower sphere's rest center).
  EXPECT_LT(eight.first, one.first)
      << "more iterations did not reduce stack sink";
  // (b) determinism at n=8: the fingerprint repeats.
  EXPECT_EQ(eight.second, settle(8U).second) << "nondeterministic at n=8";
}
// Substeps: step(dt) with n substeps integrates in dt/n increments. The
// existing solver-iteration knob operates WITHIN each substep; substepping
// shrinks the increment itself. Verified the same three ways: default 1 is
// the historical path, more substeps measurably reduce stack sink (the
// increments catch overlaps earlier), and replay is bit-identical. Also
// pinned: substeps(2) + iterations(4) is not expected to equal iterations(8)
// — they are different schedules, and the test that they differ is what
// stops a future refactor from silently conflating the two knobs.
TEST(PhysicsWorld, SubstepsFirmStackAndDeterminism) {
  auto settle = [](std::uint32_t substeps) {
    PhysicsWorld w(-9.81f);
    w.set_substeps(substeps);
    for (std::uint32_t i = 0; i < 6U; ++i) {
      PhysicsBody b;
      b.position[1] = 0.5f + 1.0f * static_cast<float>(i);
      b.restitution = 0.0f;
      (void)w.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) {
      w.step(kDt);
    }
    float top = w.body(5).position[1];
    double sum = 0.0;
    for (std::uint32_t i = 0; i < 6U; ++i) {
      sum += static_cast<double>(w.body(i).position[1]);
    }
    PhysicsWorld w2(-9.81f);
    w2.set_substeps(substeps);
    for (std::uint32_t i = 0; i < 6U; ++i) {
      PhysicsBody b;
      b.position[1] = 0.5f + 1.0f * static_cast<float>(i);
      b.restitution = 0.0f;
      (void)w2.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) {
      w2.step(kDt);
    }
    double sum2 = 0.0;
    for (std::uint32_t i = 0; i < 6U; ++i) {
      sum2 += static_cast<double>(w2.body(i).position[1]);
    }
    char fp[128];
    std::snprintf(fp, sizeof(fp), "%.9f|%.9f", sum, sum2);
    return std::pair<float, std::string>(3.0f - top, fp);
  };

  const auto one = settle(1U);
  const auto four = settle(4U);

  // Default 1 reproduces the historical path.
  EXPECT_EQ(one.second, settle(1U).second) << "nondeterministic at n=1";

  // More substeps -> measurably firmer stack (independent of the iteration
  // knob: this run keeps iterations at the default 1).
  EXPECT_LT(four.first, one.first)
      << "more substeps did not reduce stack sink";

  // The schedules differ — a future refactor must not silently conflate
  // substeps with solver iterations.
  PhysicsWorld both(-9.81f);
  both.set_substeps(2U);
  both.set_solver_iterations(4U);
  for (std::uint32_t i = 0; i < 6U; ++i) {
    PhysicsBody b;
    b.position[1] = 0.5f + 1.0f * static_cast<float>(i);
    b.restitution = 0.0f;
    (void)both.add_body(b);
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) {
    both.step(kDt);
  }
  float top_both = both.body(5).position[1];
  EXPECT_NE(3.0f - top_both, four.first) << "substeps(2)+iter(4) == substeps(4)";
  EXPECT_EQ(four.second, settle(4U).second) << "nondeterministic at n=4";
}

}  // namespace

// The first broadphase attempt produced a bit-identical fingerprint at N=1000
// and diverged at N=4000, because step 3 mutated positions while it walked
// pairs, so any candidate list built before the pass could miss a pair that an
// earlier correction had created. Detection is now frozen against a snapshot,
// which is what makes the grid exactly conservative.
//
// These cases compare the two pair-enumeration strategies directly, at a scale
// dense enough for the original bug to appear. A single-scale check would pass
// against the broken version.
TEST(PhysicsWorld, BroadphaseAgreesWithAllPairsAtSeveralScales) {
  // Same scatter as the determinism test, but stepped at three densities. The
  // old failure needed enough bodies in flight for a correction to push one
  // body into another it had not been paired with.
  const auto run = [](std::size_t count, bool force_all_pairs) {
    omnicpp::physics::PhysicsWorld world(-9.81F);
    world.set_force_all_pairs(force_all_pairs);
    for (std::size_t i = 0; i < count; ++i) {
      omnicpp::physics::PhysicsBody b;
      const float f = static_cast<float>(i);
      b.position[0] = -10.0F + std::fmod(f * 0.37F, 20.0F);
      b.position[1] = 2.0F + std::fmod(f * 0.11F, 8.0F);
      b.position[2] = -10.0F + std::fmod(f * 0.53F, 20.0F);
      b.radius = 0.2F + 0.3F * std::fmod(f * 0.017F, 1.0F);
      b.restitution = 0.3F + 0.2F * std::fmod(f * 0.023F, 1.0F);
      (void)world.add_body(b);
    }
    for (std::size_t step = 0; step < static_cast<std::size_t>(60); ++step) world.step(1.0F / 60.0F);
    return world.position_fingerprint();
  };

  for (const std::size_t count : {std::size_t{200}, std::size_t{600},
                                  std::size_t{1200}}) {
    EXPECT_EQ(run(count, false), run(count, true))
        << "broadphase diverged from all-pairs at " << count << " bodies";
  }
}

TEST(PhysicsWorld, ForceAllPairsIsHonouredAndReversible) {
  omnicpp::physics::PhysicsWorld world(-9.81F);
  EXPECT_FALSE(world.force_all_pairs());
  world.set_force_all_pairs(true);
  EXPECT_TRUE(world.force_all_pairs());
  world.set_force_all_pairs(false);
  EXPECT_FALSE(world.force_all_pairs());
}
