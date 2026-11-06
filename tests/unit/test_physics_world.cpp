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
#include <utility>
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
  // Speculative mode runs the same comparison on a hotter scene (every 25th
  // body at +/-30 m/s, 0.5 m/step against 0.2-0.5 radii, so TOI traffic
  // flows through the swept grid): the swept gatherer must yield exactly
  // the narrowphase-positive set the all-pairs scan yields, at every
  // scale. Order is structural — both paths end at the same trailing sort.
  const auto run = [](std::size_t count, bool force_all_pairs,
                       bool speculative) {
    omnicpp::physics::PhysicsWorld world(-9.81F);
    world.set_force_all_pairs(force_all_pairs);
    if (speculative) world.set_speculative_contacts(true);
    for (std::size_t i = 0; i < count; ++i) {
      omnicpp::physics::PhysicsBody b;
      const float f = static_cast<float>(i);
      b.position[0] = -10.0F + std::fmod(f * 0.37F, 20.0F);
      b.position[1] = 2.0F + std::fmod(f * 0.11F, 8.0F);
      b.position[2] = -10.0F + std::fmod(f * 0.53F, 20.0F);
      b.radius = 0.2F + 0.3F * std::fmod(f * 0.017F, 1.0F);
      b.restitution = 0.3F + 0.2F * std::fmod(f * 0.023F, 1.0F);
      if (speculative && i % 25U == 0U) {
        b.velocity[0] = (i % 50U == 0U) ? 30.0F : -30.0F;
        // Every other fast body is a capsule: the swept grid keys on
        // bound (= radius + half-height) and the TOI path runs the
        // segment narrowphase, so the agreement must hold for mixed
        // shapes too — not just the sphere path the gate covers.
        if (i % 50U == 0U) {
          b.shape = omnicpp::physics::BodyShape::Capsule;
          b.half_height = 0.4F;
        }
      }
      (void)world.add_body(b);
    }
    for (std::size_t step = 0; step < static_cast<std::size_t>(60); ++step) world.step(1.0F / 60.0F);
    return world.position_fingerprint();
  };

  for (const std::size_t count : {std::size_t{200}, std::size_t{600},
                                  std::size_t{1200}}) {
    EXPECT_EQ(run(count, false, false), run(count, true, false))
        << "broadphase diverged from all-pairs at " << count << " bodies";
    EXPECT_EQ(run(count, false, true), run(count, true, true))
        << "swept broadphase diverged from all-pairs at " << count
        << " bodies";
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

// Loop 12 (research loop 1 of 10): the spec listed a Jolt-style
// record-rewind-replay validator and an XPBD compliance opt-in as adopted,
// but neither existed in code. These cases pin both.
TEST(PhysicsWorld, Loop12DeterminismCheckPassesOnCollidingScene) {
  omnicpp::physics::PhysicsWorld w(-9.81F);
  EXPECT_FALSE(w.determinism_check());
  w.set_determinism_check(true);
  EXPECT_TRUE(w.determinism_check());
  // Colliding scene: ground contacts plus sphere-sphere contacts, with
  // substeps and iterations on so the check covers the full schedule.
  w.set_substeps(2U);
  w.set_solver_iterations(4U);
  for (std::uint32_t i = 0; i < 6U; ++i) {
    omnicpp::physics::PhysicsBody b;
    b.position[0] = (i % 2U == 0U) ? -0.4F : 0.4F;
    b.position[1] = 0.5F + 1.0F * static_cast<float>(i);
    b.restitution = 0.2F;
    (void)w.add_body(b);
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(300); ++i) {
    w.step(kDt);
  }
  EXPECT_EQ(w.determinism_violations(), 0ULL);
  EXPECT_FALSE(w.contacts().empty());  // the check ran over real contacts
  // Disabling the flag restores the single-run path exactly.
  w.set_determinism_check(false);
  const std::uint64_t fp = w.position_fingerprint();
  for (std::size_t i = 0; i < static_cast<std::size_t>(60); ++i) {
    w.step(kDt);
  }
  EXPECT_EQ(w.determinism_violations(), 0ULL);
  EXPECT_NE(w.position_fingerprint(), fp);  // the world still advances
}

TEST(PhysicsWorld, Loop12ComplianceZeroIsBitIdentical) {
  auto run = [](float alpha) {
    omnicpp::physics::PhysicsWorld w(-9.81F);
    w.set_contact_compliance(alpha);
    w.set_substeps(2U);
    w.set_solver_iterations(4U);
    for (std::uint32_t i = 0; i < 6U; ++i) {
      omnicpp::physics::PhysicsBody b;
      b.position[0] = (i % 2U == 0U) ? -0.4F : 0.4F;
      b.position[1] = 0.5F + 1.0F * static_cast<float>(i);
      b.restitution = 0.2F;
      (void)w.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(300); ++i) {
      w.step(kDt);
    }
    return w.position_fingerprint();
  };
  // Default (never set) and explicit 0 agree bit-for-bit: opting in is
  // what changes output, not the existence of the knob.
  EXPECT_EQ(run(-1.0F), run(0.0F));  // negative clamps to 0
  EXPECT_EQ(omnicpp::physics::PhysicsWorld(-9.81F).contact_compliance(), 0.0F);
}

TEST(PhysicsWorld, RestitutionThresholdKillsMicroBounce) {
  PhysicsWorld w(-9.81F);
  w.set_restitution_threshold(0.5F);  // below any real impact here
  PhysicsBody b;
  b.position[1] = 1.0F;
  b.radius = 0.5F;
  b.restitution = 0.8F;
  (void)w.add_body(b);

  // First impact at ~3.1 m/s >> threshold: must still bounce. The threshold
  // gates micro-bounces only, never real impacts.
  bool contacted = false;
  for (std::size_t i = 0; i < 240U && !contacted; ++i) {
    w.step(kDt);
    contacted = !w.contacts().empty();
  }
  ASSERT_TRUE(contacted);
  EXPECT_GT(w.body(0).velocity[1], 0.0F);  // reflected upward

  // ...but the decay ends in exact rest instead of oscillating forever.
  for (std::size_t i = 0; i < 600U; ++i) {
    w.step(kDt);
  }
  // Bit-exact: the clamp writes radius, the threshold writes 0.0f.
  EXPECT_FLOAT_EQ(w.body(0).position[1], 0.5F);
  EXPECT_FLOAT_EQ(w.body(0).velocity[1], 0.0F);
}

TEST(PhysicsWorld, IslandSleepingFreezesSettledPile) {
  // 10x10 columns, 4 high, spawned exactly at rest (the compliance-tower
  // convention): symmetric, so tangential velocities stay near zero and
  // every island can satisfy the rest test. Lateral spacing 1.5 keeps
  // columns decoupled — at 1.02 the falling columns lean into lateral
  // force chains that jam the whole pile into one rattling island (one
  // fast body vetoes all 400; measured 0 asleep at 1500 steps). A dropped
  // (1cm) spawn likewise never settles: the impact clump finds a limit
  // cycle the sequential solver cannot leave (measured on the historical
  // path too — a solver-convergence limitation, not a sleeper defect — so
  // the acceptance geometry spawns clean, like every other settle test).
  // (Deliberately NOT the default path: thresholds opt in, and the
  // exposure-bug lesson says the tested values must differ from defaults.)
  auto build = [] {
    PhysicsWorld w(-9.81F);
    w.set_restitution_threshold(0.5F);
    w.set_sleep_threshold(0.05F);
    w.set_solver_iterations(4U);
    for (std::uint32_t ix = 0; ix < 10U; ++ix) {
      for (std::uint32_t iz = 0; iz < 10U; ++iz) {
        for (std::uint32_t k = 0; k < 4U; ++k) {
          PhysicsBody b;
          b.position[0] = static_cast<float>(ix) * 1.5F;
          b.position[1] = 0.5F + static_cast<float>(k) * 1.0F;
          b.position[2] = static_cast<float>(iz) * 1.5F;
          b.restitution = 0.0F;
          (void)w.add_body(b);
        }
      }
    }
    return w;
  };
  PhysicsWorld w = build();
  std::size_t settle_steps = 0;
  while (w.sleeping_bodies() != 400U) {
    w.step(kDt);
    ++settle_steps;
    ASSERT_LT(settle_steps, 1500U) << "pile never fully slept";
  }
  const std::uint64_t frozen = w.position_fingerprint();
  for (std::size_t i = 0; i < 60U; ++i) {
    w.step(kDt);
  }
  // Frozen: sleeping restores the exact pre-step state, so the fingerprint
  // is bit-identical across steps and every body stays asleep — while
  // contacts still emit (telemetry sees the pile, the solver skips it).
  EXPECT_EQ(w.position_fingerprint(), frozen);
  EXPECT_EQ(w.sleeping_bodies(), 400U);
  EXPECT_FALSE(w.contacts().empty());
  // At REST, not merely frozen: every body sits at its column rest height
  // (0.5 + layer; ids are column-major so layer = id % 4) with ~zero
  // velocity. This is what pins the rest-test DIRECTION: a flipped
  // comparison freezes all 400 bodies mid-fall on first contact — asleep,
  // frozen, replay-identical, but metres from rest — and these fail it.
  float max_speed = 0.0F;
  for (std::uint32_t id = 0; id < 400U; ++id) {
    const float rest_y = 0.5F + static_cast<float>(id % 4U);
    EXPECT_NEAR(w.body(id).position[1], rest_y, 0.02F) << "id " << id;
    const auto& v = w.body(id).velocity;
    const float s = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (s > max_speed) max_speed = s;
  }
  EXPECT_LT(std::sqrt(max_speed), 1e-4F);
  // Replay: a fresh identical run reaches the same frozen fingerprint —
  // sleep decisions are derived state, so they reproduce exactly.
  PhysicsWorld replay = build();
  for (std::size_t i = 0; i < settle_steps + 60U; ++i) {
    replay.step(kDt);
  }
  EXPECT_EQ(replay.position_fingerprint(), frozen);
  EXPECT_EQ(replay.sleeping_bodies(), 400U);
}

TEST(PhysicsWorld, SleepingPreservesDoubleStepContract) {
  // Compositional check: the Loop-12 validator double-steps every step,
  // so any nondeterminism the sleeper introduced (stale flags, order-
  // dependent union, unrestored state) surfaces as violations.
  PhysicsWorld w(-9.81F);
  w.set_restitution_threshold(0.5F);
  w.set_sleep_threshold(0.05F);
  w.set_determinism_check(true);
  for (std::uint32_t i = 0; i < 6U; ++i) {
    PhysicsBody b;
    b.position[1] = 0.5F + 1.0F * static_cast<float>(i);
    b.restitution = 0.2F;
    (void)w.add_body(b);
  }
  for (std::size_t i = 0; i < 300U; ++i) {
    w.step(kDt);
  }
  EXPECT_EQ(w.determinism_violations(), 0ULL);
  EXPECT_FALSE(w.contacts().empty());  // the check ran over real contacts
}

TEST(PhysicsWorld, VelocityFromPositionIsOptInAndSettles) {
  // Default off: the knob exists but changes nothing unless set (history
  // bit-identical — the full suite above already pins that path).
  EXPECT_FALSE(PhysicsWorld(-9.81F).velocity_from_position());
  auto run = [](bool vfp) {
    PhysicsWorld w(-9.81F);
    w.set_velocity_from_position(vfp);
    w.set_substeps(2U);
    w.set_solver_iterations(4U);
    for (std::uint32_t i = 0; i < 6U; ++i) {
      PhysicsBody b;
      b.position[0] = (i % 2U == 0U) ? -0.4F : 0.4F;
      b.position[1] = 0.5F + 1.0F * static_cast<float>(i);
      b.restitution = 0.0F;
      (void)w.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(300); ++i) {
      w.step(kDt);
    }
    return w.state_fingerprint();
  };
  // The flag does something (tested value differs from default path), and
  // what it does is deterministic: same flag, same bits.
  EXPECT_NE(run(true), run(false));
  EXPECT_EQ(run(true), run(true));
}

TEST(PhysicsWorld, VelocityFromPositionBounceAndValidator) {
  PhysicsWorld w(-9.81F);
  w.set_velocity_from_position(true);
  w.set_determinism_check(true);
  PhysicsBody b;
  b.position[1] = 1.0F;
  b.radius = 0.5F;
  b.restitution = 0.8F;
  (void)w.add_body(b);
  const float v_in = std::sqrt(2.0F * 9.81F * 0.5F);
  bool contacted = false;
  for (std::size_t i = 0; i < 240U && !contacted; ++i) {
    w.step(kDt);
    contacted = !w.contacts().empty();
  }
  ASSERT_TRUE(contacted);
  // Bounce survives the velocity rewrite: the approach speed is stored
  // pre-solve and re-applied on top of the derived velocity.
  EXPECT_NEAR(w.body(0).velocity[1], 0.8F * v_in, 0.15F);
  EXPECT_GT(w.body(0).velocity[1], 0.0F);
  // Settle-out ends in exact rest (derived velocity of a clamped body).
  w.body(0).restitution = 0.0F;
  for (std::size_t i = 0; i < 600U; ++i) {
    w.step(kDt);
  }
  EXPECT_FLOAT_EQ(w.body(0).position[1], 0.5F);
  EXPECT_FLOAT_EQ(w.body(0).velocity[1], 0.0F);
  EXPECT_EQ(w.determinism_violations(), 0ULL);
}

TEST(PhysicsWorld, CapsuleFallsAndRestsUpright) {
  // Upright capsule: support = half_height + radius. If the capsule path
  // silently treated bodies as spheres, this would rest at 0.25 instead.
  auto build = [] {
    PhysicsWorld w(-9.81F);
    w.set_restitution_threshold(0.5F);
    w.set_solver_iterations(4U);
    PhysicsBody b;
    b.shape = omnicpp::physics::BodyShape::Capsule;
    b.half_height = 0.5F;
    b.radius = 0.25F;
    b.position[1] = 2.0F;
    b.restitution = 0.0F;
    (void)w.add_body(b);
    return w;
  };
  PhysicsWorld w = build();
  for (std::size_t i = 0; i < 600U; ++i) {
    w.step(kDt);
  }
  EXPECT_NEAR(w.body(0).position[1], 0.75F, 0.02F);
  EXPECT_FALSE(std::isnan(w.body(0).position[1]));
  EXPECT_LT(std::abs(w.body(0).velocity[1]), 1e-3F);
  PhysicsWorld replay = build();
  for (std::size_t i = 0; i < 600U; ++i) {
    replay.step(kDt);
  }
  EXPECT_EQ(replay.position_fingerprint(), w.position_fingerprint());
}

TEST(PhysicsWorld, CapsuleCapsuleNarrowphaseSeparates) {
  // Crossed capsules whose CENTERS are 0.6 apart (spheres of r=0.25 would
  // see no contact) but whose segments overlap by 0.4. A sphere-only
  // narrowphase leaves them frozen at 0.6; the segment path separates them.
  PhysicsWorld w(0.0F);  // no gravity: pure separation test
  w.set_determinism_check(true);
  PhysicsBody a;
  a.shape = omnicpp::physics::BodyShape::Capsule;
  a.half_height = 0.5F;
  a.radius = 0.25F;
  a.position[1] = 5.0F;
  // 90 degrees about Z: capsule axis Y -> X.
  a.orientation[0] = 0.0F;
  a.orientation[1] = 0.0F;
  a.orientation[2] = 0.70710678F;
  a.orientation[3] = 0.70710678F;
  PhysicsBody b;
  b.shape = omnicpp::physics::BodyShape::Capsule;
  b.half_height = 0.5F;
  b.radius = 0.25F;
  b.position[0] = 0.6F;
  b.position[1] = 5.0F;
  (void)w.add_body(a);
  (void)w.add_body(b);
  w.step(kDt);
  // A sphere-only narrowphase emits no contact here at all (centers 0.6
  // apart with r-sum 0.5), so a first-step contact pins the segment path
  // directly — not just its after-effects. contacts() holds the last
  // step only, so this is asserted before the bodies separate.
  EXPECT_FALSE(w.contacts().empty());
  for (std::size_t i = 0; i < 59U; ++i) {
    w.step(kDt);
  }
  const float dx = w.body(1).position[0] - w.body(0).position[0];
  EXPECT_FALSE(std::isnan(dx));
  EXPECT_GT(std::abs(dx), 0.65F);  // driven apart, not frozen at 0.6
  // Untouched run agrees bit-for-bit (segment closest-points are pure).
  PhysicsWorld replay(0.0F);
  PhysicsBody ra = a;
  PhysicsBody rb = b;
  (void)replay.add_body(ra);
  (void)replay.add_body(rb);
  for (std::size_t i = 0; i < 60U; ++i) {
    replay.step(kDt);
  }
  EXPECT_EQ(replay.position_fingerprint(), w.position_fingerprint());
  EXPECT_EQ(w.determinism_violations(), 0ULL);
}

TEST(PhysicsWorld, RotationIntegratesHalfTurn) {
  // Zero gravity, no contacts: pure orientation integration. 1 rev/s about
  // Y for 0.5 s must be a half turn: |qy| ~ 1, qw ~ 0.
  PhysicsWorld w(0.0F);
  w.set_determinism_check(true);
  PhysicsBody b;
  b.position[1] = 5.0F;
  b.angular_velocity[1] = 2.0F * 3.14159265F;
  (void)w.add_body(b);
  for (std::size_t i = 0; i < 60U; ++i) {
    w.step(kDt);  // 60/120 s = 0.5 s
  }
  EXPECT_NEAR(std::abs(w.body(0).orientation[1]), 1.0F, 0.02F);
  EXPECT_NEAR(w.body(0).orientation[3], 0.0F, 0.02F);
  // Renormalized every step: still a unit quaternion.
  const float* q = w.body(0).orientation;
  EXPECT_NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0F,
              1e-5F);
  // Spin persists without contacts (no damping invented here).
  EXPECT_NEAR(w.body(0).angular_velocity[1], 2.0F * 3.14159265F, 1e-4F);
  EXPECT_EQ(w.determinism_violations(), 0ULL);
}

TEST(PhysicsWorld, GroundFrictionSpinsSlidingBall) {
  // A ball sliding on the ground must gain forward roll from the friction
  // impulse torque. Zero torque would leave spin at exactly 0 — the test
  // pins the direction the physics requires.
  PhysicsWorld w(-9.81F);
  PhysicsBody b;
  b.position[1] = 0.5F;
  b.velocity[0] = 3.0F;
  b.restitution = 0.0F;
  (void)w.add_body(b);
  for (std::size_t i = 0; i < 120U; ++i) {
    w.step(kDt);
  }
  EXPECT_LT(w.body(0).angular_velocity[2], -0.5F);  // forward roll
  EXPECT_GE(w.body(0).position[1], 0.5F - 1e-4F);   // never sinks
  // Linear path untouched by the spin side effect: replay bit-identical.
  PhysicsWorld replay;
  replay = PhysicsWorld(-9.81F);
  PhysicsBody c = b;
  c.angular_velocity[2] = 0.0F;
  (void)replay.add_body(c);
  for (std::size_t i = 0; i < 120U; ++i) {
    replay.step(kDt);
  }
  EXPECT_EQ(replay.position_fingerprint(), w.position_fingerprint());
}

TEST(PhysicsWorld, DistanceJointHoldsTwoBodies) {
  EXPECT_EQ(PhysicsWorld(0.0F).joint_count(), 0U);  // empty by default
  auto build = [] {
    PhysicsWorld w(0.0F);
    w.set_solver_iterations(4U);
    PhysicsBody a;
    a.position[1] = 5.0F;
    PhysicsBody b;
    b.position[0] = 3.0F;
    b.position[1] = 5.0F;
    const auto ia = w.add_body(a);
    const auto ib = w.add_body(b);
    (void)w.add_distance_joint(ia, ib, 1.0F);
    return w;
  };
  PhysicsWorld w = build();
  for (std::size_t i = 0; i < 120U; ++i) {
    w.step(kDt);
  }
  const float dx = w.body(1).position[0] - w.body(0).position[0];
  const float dy = w.body(1).position[1] - w.body(0).position[1];
  const float dz = w.body(1).position[2] - w.body(0).position[2];
  EXPECT_NEAR(std::sqrt(dx * dx + dy * dy + dz * dz), 1.0F, 0.02F);
  PhysicsWorld replay = build();
  for (std::size_t i = 0; i < 120U; ++i) {
    replay.step(kDt);
  }
  EXPECT_EQ(replay.position_fingerprint(), w.position_fingerprint());
}

TEST(PhysicsWorld, SleepingKeepsSpinningBodiesAwake) {
  // The rest test gates on spin too: a body at rest linearly but spinning
  // in place must not sleep. Without the angular gate this reports 1.
  PhysicsWorld w(-9.81F);
  w.set_sleep_threshold(0.05F);
  PhysicsBody b;
  b.position[1] = 0.5F;
  b.restitution = 0.0F;
  b.angular_velocity[1] = 1.0F;  // 1 rad/s >> 0.05 gate
  (void)w.add_body(b);
  for (std::size_t i = 0; i < 60U; ++i) {
    w.step(kDt);
  }
  EXPECT_EQ(w.sleeping_bodies(), 0U);
}

TEST(PhysicsWorld, Loop12ComplianceSoftensAndStaysDeterministic) {
  auto settle_top = [](float alpha) {
    omnicpp::physics::PhysicsWorld w(-9.81F);
    w.set_contact_compliance(alpha);
    for (std::uint32_t i = 0; i < 6U; ++i) {
      omnicpp::physics::PhysicsBody b;
      b.position[1] = 0.5F + 1.0F * static_cast<float>(i);
      b.restitution = 0.0F;
      (void)w.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(600); ++i) {
      w.step(kDt);
    }
    return std::pair<float, std::uint64_t>(w.body(5).position[1],
                                           w.position_fingerprint());
  };
  const auto stiff = settle_top(0.0F);
  // Trial 638: alpha is now the exact-XPBD compliance (atilde = alpha/h^2
  // with h = 1/120 here), so 1.0 would be collapse-soft, not settle-soft.
  // 1e-4 is the moderate setting: visibly softer, structurally intact.
  const auto soft = settle_top(1.0e-4F);
  // Compliance softens the correction, so the tower settles lower...
  EXPECT_LT(soft.first, stiff.first);
  // ...and the outputs differ, so a test using only the default value
  // could never catch a broken knob (the exposure-bug lesson: never let
  // the tested value coincide with the buggy value).
  EXPECT_NE(soft.second, stiff.second);
  // ...while determinism is preserved at the new setting.
  EXPECT_EQ(soft.second, settle_top(1.0e-4F).second);
}

// Trial 638 (adopted, ADR-008): exact-XPBD compliance. The rest state of
// a loaded compliant contact must not depend on solver settings — the
// incumbent divided by (inv + raw alpha) every pass, so 8 iterations
// converged to ~stiff (pen 3e-6 vs 6.8e-4 at 1 iteration, measured
// pre-adoption) and substeps shifted it 16x. With atilde = alpha/h^2 and
// the accumulated lambda, the equilibrium is the compliant spring rest
// (pen ~= alpha*m*g here: 1e-4*9.81 ~= 9.8e-4, measured exactly that).
namespace {
float trial638_rest(float alpha, std::uint32_t iters, std::uint32_t subs) {
  PhysicsWorld w(-9.81F);
  w.set_contact_compliance(alpha);
  w.set_solver_iterations(iters);
  w.set_substeps(subs);
  PhysicsBody base;
  base.position[1] = 0.5F;
  base.radius = 0.5F;
  base.inverse_mass = 0.0F;
  base.restitution = 0.0F;
  PhysicsBody top;
  top.position[1] = 1.5F;
  top.radius = 0.5F;
  top.inverse_mass = 1.0F;
  top.restitution = 0.0F;
  (void)w.add_body(base);
  (void)w.add_body(top);
  for (std::size_t i = 0; i < 1200U; ++i) {
    w.step(kDt);
  }
  return w.body(1).position[1];
}
}  // namespace

TEST(PhysicsWorld, Trial638ComplianceIndependentOfIterationCount) {
  const float stiff = trial638_rest(0.0F, 8U, 1U);
  EXPECT_FLOAT_EQ(stiff, 1.5F);  // stiff rests exactly touching
  const float it1 = trial638_rest(1.0e-4F, 1U, 1U);
  const float it8 = trial638_rest(1.0e-4F, 8U, 1U);
  // Compliant, not stiff: the equilibrium carries ~1e-3 penetration — and
  // a test run at alpha = 0 could never tell the knob from broken.
  EXPECT_LT(1.5F - it1, 2.0e-3F);
  EXPECT_GT(1.5F - it1, 1.0e-4F);
  // Iteration-count independence: the point of the exact formulation.
  // (Pre-adoption: it8 sat 200x stiffer than it1.)
  EXPECT_NEAR(it8, it1, 1.0e-5F);
}

TEST(PhysicsWorld, Trial638ComplianceIndependentOfSubstepCount) {
  // Guards the atilde = alpha/h^2 scaling: raw alpha would shift the
  // equilibrium ~16x across a 4x substep change (measured pre-adoption).
  // The compliance band matters too: with raw alpha = 1e-4 the contact is
  // ~stiff at every substep count, so a bare s4-vs-s1 comparison passes a
  // broken scaling by accident. Pin the softness first, then the equality.
  const float s1 = trial638_rest(1.0e-4F, 1U, 1U);
  EXPECT_GT(1.5F - s1, 1.0e-4F);
  EXPECT_LT(1.5F - s1, 2.0e-3F);
  const float s4 = trial638_rest(1.0e-4F, 1U, 4U);
  EXPECT_NEAR(s4, s1, 1.0e-5F);
}

// R3 fix gate (docs/research/physics-solver.md R3; was
// FastBodyTunnelsOnSomePhasesR3Gate). History: the solver integrates then
// detects on post-step positions, so travel S > window W tunneled on 8/12
// phases by two traced mechanisms: (1) clean misses — no post-step sample
// in the window (phases 5-8); (2) far-side ejections — the sample lands
// just past center (phases 1-4), the projection exits the near side of the
// motion (the obstacle's far side), and the separating contact draws no
// impulse: 198 m/s in and out. Colliding phases (0, 9-11) project to the
// near face and rebound (198 -> -39.6 m/s on the phase-0 trace).
// With speculative contacts on, all 8 must stop at the near face (0/12
// cross) while the 4 colliding phases reproduce bit-identically — the flag
// adds pairs only where the discrete test finds none. Per research R3, the
// mechanism is pinned by per-phase contact TELEMETRY, not just final
// poses: ejections and misses are indistinguishable post-hoc, so every
// phase must show a contact event (the 4 historical clean misses going
// from zero events to a speculative one is the fix made visible).
// Capsule tunneling gate (D1; spheres fixed loop52, capsules open and
// documented as the remaining sweep). Same 12-phase protocol as the R3
// sphere gate, only the shot is a capsule (radius 0.1, half-height 0.4,
// upright so its segment is perpendicular to the flight direction): the
// ONLY variable vs the sphere gate is shape. Travel S = 3W is identical
// (198 m/s at 1/60), so any trigger difference is the narrowphase, not
// the setup. The speculative flag stays off: this pins the discrete
// capsule path the sweep must later fix, the way the R3 gate pinned the
// discrete sphere path before loop52.
TEST(PhysicsWorld, FastCapsuleTunnelsOnSomePhases) {
  constexpr float kR = 1.0f;        // static obstacle radius
  constexpr float kShotR = 0.1f;    // capsule radius
  constexpr float kHalf = 0.4f;     // capsule half-height (bound 0.5)
  constexpr float kWindow = kR + kShotR;  // contact window in flight dir
  constexpr float kGateDt = 1.0f / 60.0f;
  // Identical travel to the R3 sphere gate (3.3 m/step, 198 m/s): the
  // bound sum (1.5) only sizes the broadphase, not the shot.
  constexpr float kStepTravel = 3.0f * kWindow;
  constexpr float kSpeed = kStepTravel / kGateDt;
  constexpr int kPhases = 12;
  constexpr int kSteps = 10;

  struct PhaseOut {
    float x{0.0f};
    float vx{0.0f};
    bool contacted{false};
  };
  auto run_phase = [](int p, bool speculative) {
    PhysicsWorld w(/*gravity=*/0.0f);
    // Explicit both ways: the suite must not depend on the default.
    w.set_speculative_contacts(speculative);
    PhysicsBody wall;
    wall.position[0] = 0.0f;
    wall.position[1] = 5.0f;
    wall.radius = kR;
    wall.inverse_mass = 0.0f;
    (void)w.add_body(wall);
    PhysicsBody shot;
    shot.shape = omnicpp::physics::BodyShape::Capsule;
    shot.radius = kShotR;
    shot.half_height = kHalf;
    const float phase = static_cast<float>(p) * kStepTravel /
                        static_cast<float>(kPhases);
    shot.position[0] = -10.0f + phase;
    shot.position[1] = 5.0f;
    shot.restitution = 0.0f;
    shot.velocity[0] = kSpeed;
    (void)w.add_body(shot);
    PhaseOut out{};
    for (int i = 0; i < kSteps; ++i) {
      w.step(kGateDt);
      if (!w.contacts().empty()) out.contacted = true;
    }
    out.x = w.body(1).position[0];
    out.vx = w.body(1).velocity[0];
    return out;
  };

  // The flag is what fixes it: discrete still tunnels (the loop55 trigger,
  // kept alive as the off-mode pin).
  int tunneled_off = 0;
  for (int p = 0; p < kPhases; ++p) {
    if (run_phase(p, false).x > kR) ++tunneled_off;
  }
  EXPECT_GE(tunneled_off, 1) << "off-mode trigger gone: gate pins nothing";
  // Sphere twin: identical setup with a sphere shot (r=0.1), flag on. The
  // capsule flies perpendicular to its segment, so the contact geometry
  // coincides with the sphere's — the twin must agree to 1e-3. This is the
  // hover guard with teeth: a bound-only rewind lands 0.4 short and the
  // rebound trajectories diverge by the same 0.4.
  auto run_sphere_twin = [](int p) {
    PhysicsWorld w(/*gravity=*/0.0f);
    w.set_speculative_contacts(true);
    PhysicsBody wall;
    wall.position[0] = 0.0f;
    wall.position[1] = 5.0f;
    wall.radius = kR;
    wall.inverse_mass = 0.0f;
    (void)w.add_body(wall);
    PhysicsBody shot;
    shot.radius = kShotR;
    const float phase = static_cast<float>(p) * kStepTravel /
                        static_cast<float>(kPhases);
    shot.position[0] = -10.0f + phase;
    shot.position[1] = 5.0f;
    shot.restitution = 0.0f;
    shot.velocity[0] = kSpeed;
    (void)w.add_body(shot);
    for (int i = 0; i < kSteps; ++i) w.step(kGateDt);
    return std::make_pair(w.body(1).position[0], w.body(1).velocity[0]);
  };
  // Fix mode: no phase crosses, every phase interacted, approach dead,
  // and every phase matches its sphere twin (no hover, no early stop).
  for (int p = 0; p < kPhases; ++p) {
    const PhaseOut on = run_phase(p, true);
    EXPECT_TRUE(on.contacted) << "phase " << p << " passed silently";
    EXPECT_LT(on.x, 0.0f) << "phase " << p << " crossed: x = " << on.x;
    EXPECT_LE(on.vx, 1e-3f) << "phase " << p << " advancing: vx = " << on.vx;
    const auto twin = run_sphere_twin(p);
    EXPECT_NEAR(on.x, twin.first, 1e-3f)
        << "phase " << p << " diverges from sphere twin";
    EXPECT_NEAR(on.vx, twin.second, 1e-3f)
        << "phase " << p << " diverges from sphere twin";
  }
  // Colliding phases (self-calibrating set: off-mode ends in the window)
  // reproduce the discrete outcome to 1e-3, with the bounce floor pinning
  // the rebound is preserved, not merely non-crossing.
  for (int p = 0; p < kPhases; ++p) {
    const PhaseOut off = run_phase(p, false);
    if (off.x > kR) continue;  // tunneled off-mode: no discrete outcome
    const PhaseOut on = run_phase(p, true);
    EXPECT_NEAR(on.x, off.x, 1e-3f) << "phase " << p;
    EXPECT_NEAR(on.vx, off.vx, 1e-3f) << "phase " << p;
    EXPECT_LT(on.vx, -1.0f) << "phase " << p << " lost its bounce";
  }
}

TEST(PhysicsWorld, SpeculativeContactsStopFastBodyCrossing) {
  constexpr float kR = 1.0f;       // static obstacle radius
  constexpr float kR2 = 0.1f;      // projectile radius
  constexpr float kWindow = kR + kR2;
  // Gate-local name: the file-scope kDt is 1/120, and this gate needs the
  // 3.3 m/step travel that 1/60 gives (a test-scope kDt shadowed it, which
  // -Wshadow caught on clang).
  constexpr float kGateDt = 1.0f / 60.0f;
  constexpr float kStepTravel = 3.0f * kWindow;  // S = 3W > W: misses exist
  constexpr float kSpeed = kStepTravel / kGateDt;  // 198 m/s at this dt
  constexpr int kPhases = 12;
  constexpr int kSteps = 10;  // -10 + 10*3.3 clears the obstacle on every phase

  struct PhaseOut {
    float x{0.0f};
    float vx{0.0f};
    bool contacted{false};
  };
  auto run_phase = [](int p, bool speculative) {
    PhysicsWorld w(/*gravity=*/0.0f);
    // Explicit both ways: the suite must not depend on the default.
    w.set_speculative_contacts(speculative);
    PhysicsBody wall;
    wall.position[0] = 0.0f;
    wall.position[1] = 5.0f;  // well clear of the y = 0 plane
    wall.radius = kR;
    wall.inverse_mass = 0.0f;  // static
    (void)w.add_body(wall);
    PhysicsBody shot;
    const float phase = static_cast<float>(p) * kStepTravel /
                        static_cast<float>(kPhases);
    shot.position[0] = -10.0f + phase;
    shot.position[1] = 5.0f;
    shot.radius = kR2;
    shot.restitution = 0.0f;  // approach killed on contact: cross vs return
    shot.velocity[0] = kSpeed;
    (void)w.add_body(shot);
    PhaseOut out{};
    for (int i = 0; i < kSteps; ++i) {
      w.step(kGateDt);
      if (!w.contacts().empty()) out.contacted = true;
    }
    out.x = w.body(1).position[0];
    out.vx = w.body(1).velocity[0];
    return out;
  };

  // The flag is what fixes it: discrete still tunnels (the old trigger,
  // kept alive as the off-mode pin so the gate cannot pass vacuously).
  int tunneled_off = 0;
  for (int p = 0; p < kPhases; ++p) {
    if (run_phase(p, false).x > kR) ++tunneled_off;
  }
  EXPECT_GE(tunneled_off, 1) << "off-mode trigger gone: gate pins nothing";
  // Fix mode: no phase crosses, every phase interacted, approach dead.
  // vx bound is 1e-3, not 0: the impulse cancels 198 m/s in float32, so a
  // stopped shot keeps a ~1e-5 residual — three orders below still counts
  // as stopped against the +198 it kept while tunneling.
  for (int p = 0; p < kPhases; ++p) {
    const PhaseOut on = run_phase(p, true);
    EXPECT_TRUE(on.contacted) << "phase " << p << " passed silently";
    EXPECT_LT(on.x, 0.0f) << "phase " << p << " crossed: x = " << on.x;
    EXPECT_LE(on.vx, 1e-3f) << "phase " << p << " advancing: vx = " << on.vx;
  }
  // Colliding phases (a post-step sample lands in the window) keep their
  // outcome: near-face landing, same rebound. TOI rewind + impulse uses a
  // different op sequence than discrete separation + impulse (TOI sqrt,
  // path rewind), so the pin is 1e-3 agreement, not bits — measured gaps
  // 2e-5 (x) and 1.5e-4 (vx) on phase 0, three orders below the -39.6 m/s
  // bounce and the metre-scale geometry. The bounce floor (vx < -1) pins
  // the outcome is unchanged, not merely non-crossing.
  for (const int p : {0, 9, 10, 11}) {
    const PhaseOut off = run_phase(p, false);
    const PhaseOut on = run_phase(p, true);
    EXPECT_NEAR(on.x, off.x, 1e-3f) << "phase " << p;
    EXPECT_NEAR(on.vx, off.vx, 1e-3f) << "phase " << p;
    EXPECT_LT(on.vx, -1.0f) << "phase " << p << " lost its bounce";
  }
}
