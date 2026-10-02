//! @file test_physics_world.cpp
//! @brief Deterministic physics proofs: analytic projectile trajectory,
//!        bounce restitution, sphere-sphere impulse symmetry, static bodies,
//!        and bit-identical fingerprints across repeated runs.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "warploom/core/physics_world.hpp"

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
  w.add_body(b);

  // Semi-implicit Euler: p_{n+1} = p_n + v_{n+1} * dt (velocity first).
  const float dt = kDt;
  float expected_y = 1000.0f;
  float expected_vy = 0.0f;
  for (int i = 0; i < 30; ++i) {
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
  w.add_body(b);

  // Analytic impact speed from the 0.5 m drop: v = sqrt(2 * g * h).
  const float v_in = std::sqrt(2.0f * 9.81f * 0.5f);
  // Run until the first ground contact, then check the reflected speed.
  bool contacted = false;
  for (int i = 0; i < 240 && !contacted; ++i) {
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
  w.add_body(a);
  w.add_body(b);

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
  w.add_body(wall);
  w.add_body(ball);

  w.step(kDt);
  EXPECT_FLOAT_EQ(w.body(0).position[0], 0.0f);   // wall never moves
  EXPECT_TRUE(w.body(0).velocity[0] == 0.0f);
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
    w.add_body(a);
    w.add_body(b);
    for (int i = 0; i < 600; ++i) w.step(kDt);
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
  w.add_body(lower);
  w.add_body(upper);

  for (int i = 0; i < 600; ++i) w.step(kDt);
  // Both spheres rest at ~one radius above the plane (stacking is approximate
  // with a single solver iteration, but must be stable and in contact).
  EXPECT_NEAR(w.body(0).position[1], 0.5f, 0.05f);
  EXPECT_NEAR(w.body(1).position[1], 1.5f, 0.1f);
  EXPECT_LT(std::fabs(w.body(0).velocity[1]), 0.1f);
  EXPECT_LT(std::fabs(w.body(1).velocity[1]), 0.1f);
}

}  // namespace
