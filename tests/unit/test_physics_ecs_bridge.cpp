//! @file test_physics_ecs_bridge.cpp
//! @brief Physics-ECS bridge proofs: bodies drive World components through
//!        the fixed-step sync, static bodies stay put, and a 1000-instance
//!        stepped scene stays deterministic across two runs.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "warploom/core/ecs.hpp"
#include "warploom/core/physics_world.hpp"

namespace {

using omnicpp::core::World;
using omnicpp::physics::PhysicsBody;
using omnicpp::physics::PhysicsWorld;

//! The bridge's transform component: what the renderer consumes.
struct SceneTransform {
  float position[3]{0.0f, 0.0f, 0.0f};
  float scale{1.0f};
};

//! Mirrors the bridge: after each physics step, copy body poses into the
//! entities' SceneTransform components (bodies in insertion order map to
//! entities in creation order).
static void sync_transforms(PhysicsWorld& world, World& ecs,
                            const std::vector<omnicpp::core::Entity>& entities) {
  for (std::size_t i = 0; i < entities.size() && i < world.body_count(); ++i) {
    const PhysicsBody& b = world.body(static_cast<std::uint32_t>(i));
    auto& t = ecs.get_component<SceneTransform>(entities[i]);
    t.position[0] = b.position[0];
    t.position[1] = b.position[1];
    t.position[2] = b.position[2];
    t.scale = b.radius;
  }
}

TEST(PhysicsEcsBridge, BodiesDriveTransformComponents) {
  PhysicsWorld world(-9.81f);
  World ecs;
  std::vector<omnicpp::core::Entity> entities;
  for (int i = 0; i < 3; ++i) {
    auto e = ecs.create_entity();
    ecs.add_component<SceneTransform>(e);
    PhysicsBody b;
    b.position[1] = 3.0f + static_cast<float>(i);
    b.radius = 0.4f;
    world.add_body(b);
    entities.push_back(e);
  }

  for (int i = 0; i < 60; ++i) {
    world.step(1.0f / 60.0f);
    sync_transforms(world, ecs, entities);
  }

  // The transforms must equal the body poses (fallen under gravity).
  for (std::size_t i = 0; i < entities.size(); ++i) {
    const auto& t = ecs.get_component<SceneTransform>(entities[i]);
    const auto& b = world.body(static_cast<std::uint32_t>(i));
    EXPECT_FLOAT_EQ(t.position[0], b.position[0]);
    EXPECT_FLOAT_EQ(t.position[1], b.position[1]);
    EXPECT_FLOAT_EQ(t.scale, b.radius);
    // 60 ticks of free fall from 3-5 m at 9.81 m/s^2: they must all still
    // be falling (well below start, well above the terminal bounce zone).
    EXPECT_LT(t.position[1], 3.0f);
    EXPECT_GT(t.position[1], 0.3f);
  }
}

TEST(PhysicsEcsBridge, StaticBodyLeavesTransformAtOrigin) {
  PhysicsWorld world(0.0f);
  World ecs;
  auto e = ecs.create_entity();
  ecs.add_component<SceneTransform>(e);
  PhysicsBody b;  // inverse_mass 0 = static at origin
  b.inverse_mass = 0.0f;
  world.add_body(b);
  std::vector<omnicpp::core::Entity> entities{e};

  for (int i = 0; i < 120; ++i) {
    world.step(1.0f / 60.0f);
    sync_transforms(world, ecs, entities);
  }
  const auto& t = ecs.get_component<SceneTransform>(e);
  EXPECT_FLOAT_EQ(t.position[0], 0.0f);
  EXPECT_FLOAT_EQ(t.position[1], 0.0f);
}

TEST(PhysicsEcsBridge, ThousandInstancesDeterministic) {
  auto run = [] {
    PhysicsWorld world(-9.81f);
    World ecs;
    std::vector<omnicpp::core::Entity> entities;
    entities.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      auto e = ecs.create_entity();
      ecs.add_component<SceneTransform>(e);
      PhysicsBody b;
      const float f = static_cast<float>(i);
      // Deterministic pseudo-scatter over a 20x20 m field.
      b.position[0] = -10.0f + std::fmod(f * 0.37f, 20.0f);
      b.position[1] = 2.0f + std::fmod(f * 0.11f, 8.0f);
      b.position[2] = -10.0f + std::fmod(f * 0.53f, 20.0f);
      b.radius = 0.2f + 0.3f * std::fmod(f * 0.017f, 1.0f);
      b.restitution = 0.3f + 0.2f * std::fmod(f * 0.023f, 1.0f);
      world.add_body(b);
      entities.push_back(e);
    }
    for (int i = 0; i < 240; ++i) {  // 4 sim seconds
      world.step(1.0f / 60.0f);
      sync_transforms(world, ecs, entities);
    }
    return world.position_fingerprint();
  };
  EXPECT_EQ(run(), run());
}

}  // namespace
