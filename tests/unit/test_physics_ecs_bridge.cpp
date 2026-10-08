//! @file test_physics_ecs_bridge.cpp
//! @brief Physics-to-ECS proofs, through the bridge that actually ships.
//!
//! This file used to define its own `sync_transforms` helper and a local
//! `SceneTransform` component, then asserted that the copy worked. That proved
//! a bridge existing only in this file -- if production changed, the test would
//! still pass. C1 and C3 gave the session a real bridge, so the two bridge
//! tests now drive `EditorSession` and the local helper is gone. The
//! determinism-at-scale case stays at the solver level, where the property
//! actually lives: `position_fingerprint` over a thousand bodies needs no
//! bridge at all, and pretending otherwise only slowed it down.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "warploom/core/document_projection.hpp"
#include "warploom/core/editor_session.hpp"
#include "warploom/core/ecs.hpp"
#include "warploom/physics/physics_world.hpp"

namespace {

using omnicpp::editor::DocumentProjection;
using omnicpp::editor::DocumentTransform;
using omnicpp::editor::EditorSession;
using omnicpp::physics::PhysicsBody;
using omnicpp::physics::PhysicsWorld;

constexpr double kDt = 1.0 / 60.0;

//! Give a session `count` cubes with full transforms, each with a body.
void seed_scene(EditorSession& session, int count, float inverse_mass) {
  omnicpp::editor::SceneDocument document{};
  for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
    omnicpp::editor::SceneObject object{};
    object.id = static_cast<std::uint64_t>(i) + 1U;
    object.type_id = 1U;
    object.name = "body";
    object.properties["position"] = omnicpp::editor::PropValue::make_vec3(
        0.0, 5.0, 0.0);
    object.properties["rotation"] =
        omnicpp::editor::PropValue::make_vec3(0.0, 0.0, 0.0);
    object.properties["scale"] =
        omnicpp::editor::PropValue::make_vec3(1.0, 1.0, 1.0);
    document.objects.push_back(object);
    if (object.id >= document.next_object_id) {
      document.next_object_id = object.id + 1U;
    }
  }
  session.reset_from(std::move(document));
  (void)session.tick({0U, kDt, false});

  for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
    PhysicsBody body{};
    const float f = static_cast<float>(i);
    body.position[0] = -10.0F + std::fmod(f * 0.37F, 20.0F);
    body.position[1] = 2.0F + std::fmod(f * 0.11F, 8.0F);
    body.position[2] = -10.0F + std::fmod(f * 0.53F, 20.0F);
    body.radius = 0.2F + 0.3F * std::fmod(f * 0.017F, 1.0F);
    body.restitution = 0.3F + 0.2F * std::fmod(f * 0.023F, 1.0F);
    body.inverse_mass = inverse_mass;
    (void)session.spawn_physics_body(static_cast<std::uint64_t>(i) + 1U, body);
  }
}

}  // namespace

TEST(PhysicsEcsBridge, BodiesDriveProjectedTransforms) {
  // Previously proven by a local copy of the bridge. Now proven against the
  // one the renderer actually reads.
  EditorSession session{};
  seed_scene(session, 1, 1.0F);
  ASSERT_NE(session.physics_body_for(1U), nullptr);

  (void)session.tick({1U, kDt, false});
  ASSERT_TRUE(session.projection().projected(1U));
  const auto entity = session.projection().entity_for(1U);
  const auto& transform =
      session.projection().world().get_component<DocumentTransform>(entity);
  EXPECT_DOUBLE_EQ(transform.position[0],
                   static_cast<double>(session.physics_body_for(1U)->position[0]));
  EXPECT_DOUBLE_EQ(transform.position[1],
                   static_cast<double>(session.physics_body_for(1U)->position[1]));
  EXPECT_DOUBLE_EQ(transform.position[2],
                   static_cast<double>(session.physics_body_for(1U)->position[2]));
}

TEST(PhysicsEcsBridge, StaticBodyLeavesTransformAtOrigin) {
  EditorSession session{};
  seed_scene(session, 1, 0.0F);  // inverse_mass 0 == immovable
  const auto start = session.physics_body_for(1U)->position[1];

  for (std::uint64_t frame = 1; frame <= 10; ++frame) {
    (void)session.tick({frame, kDt, false});
  }
  EXPECT_FLOAT_EQ(session.physics_body_for(1U)->position[1], start)
      << "a static body must not move under gravity";
  const auto entity = session.projection().entity_for(1U);
  EXPECT_DOUBLE_EQ(
      session.projection().world().get_component<DocumentTransform>(entity)
          .position[1],
      static_cast<double>(start));
}

TEST(PhysicsEcsBridge, ThousandInstancesDeterministic) {
  // Solver-level, so no bridge is involved: `position_fingerprint` collapses
  // every body's position to one value, and two identical runs must produce
  // the identical value. This is the property replay depends on, and it is
  // about PhysicsWorld rather than about ECS plumbing.
  const auto run = [] {
    PhysicsWorld world(-9.81F);
    for (std::size_t i = 0; i < static_cast<std::size_t>(1000); ++i) {
      PhysicsBody b;
      const float f = static_cast<float>(i);
      // Deterministic pseudo-scatter over a 20x20 m field.
      b.position[0] = -10.0F + std::fmod(f * 0.37F, 20.0F);
      b.position[1] = 2.0F + std::fmod(f * 0.11F, 8.0F);
      b.position[2] = -10.0F + std::fmod(f * 0.53F, 20.0F);
      b.radius = 0.2F + 0.3F * std::fmod(f * 0.017F, 1.0F);
      b.restitution = 0.3F + 0.2F * std::fmod(f * 0.023F, 1.0F);
      (void)world.add_body(b);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(240); ++i) {  // 4 sim seconds
      world.step(1.0F / 60.0F);
    }
    return world.position_fingerprint();
  };
  EXPECT_EQ(run(), run());
}

TEST(PhysicsEcsBridge, ThousandInstancesThroughTheRealBridgeStayDeterministic) {
  // The same property, but end to end: two sessions each project a thousand
  // objects, simulate them through the production bridge, and must agree.
  // Slower than the solver-only case (the projection runs per tick), so it
  // uses fewer frames -- the claim is determinism, not convergence.
  const auto run = [] {
    EditorSession session{};
    seed_scene(session, 1000, 1.0F);
    for (std::uint64_t frame = 1; frame <= 5; ++frame) {
      (void)session.tick({frame, kDt, false});
    }
    return session.physics().position_fingerprint();
  };
  EXPECT_EQ(run(), run());
}