//! @file test_graph_anim_bridge.cpp
//! @brief M13 fusion proofs: graph outputs project onto animation-state
//!        actions; a pulse node drives walk<->idle exactly as a held key
//!        would; the whole chain (context time -> node -> signal -> state
//!        machine) replays deterministically across two instances.

#include <gtest/gtest.h>

#include <string>

#include "engine/editor/graph_anim_bridge.hpp"

namespace {

namespace ed = omnicpp::editor;
using omnicpp::anim::AnimationStateMachine;
using omnicpp::core::InputSnapshot;

//! Builds the standard walk<->idle machine: action "move" fires walk->idle
//! style transitions (mirrors the mannequin's configuration).
struct MachineFixture {
  AnimationStateMachine<InputSnapshot> machine;

  MachineFixture() {
    machine.add_state("walk", 0.0F);
    machine.add_state("idle", 1.0F);
    omnicpp::anim::AnimTransition to_idle;
    to_idle.from = "walk";
    to_idle.to = "idle";
    to_idle.action = "move";
    omnicpp::anim::AnimTransition to_walk;
    to_walk.from = "idle";
    to_walk.to = "walk";
    to_walk.action = "move";
    machine.add_transition(to_idle);
    machine.add_transition(to_walk);
    (void)machine.set_initial("walk");
  }
};

TEST(GraphAnimBridge, NumberOutputAboveThresholdHoldsAction) {
  ed::NodeGraph g;
  ed::register_builtin_node_types(g);
  const auto cn = g.add_node("const_number",
                             {{"value", ed::NodeValue::make_number(1.0)}});
  ed::GraphSignalAdapter adapter;
  adapter.set_signal({"move", cn, "value", 0.5});

  std::string err;
  ASSERT_TRUE(g.evaluate(err)) << err;
  const auto snap = adapter.build(g);
  EXPECT_TRUE(snap.action("move"));
}

TEST(GraphAnimBridge, BelowThresholdAndDanglingStayUnheld) {
  ed::NodeGraph g;
  ed::register_builtin_node_types(g);
  const auto cn = g.add_node("const_number",
                             {{"value", ed::NodeValue::make_number(0.2)}});
  ed::GraphSignalAdapter adapter;
  adapter.set_signal({"move", cn, "value", 0.5});
  // Dangling: unknown node and unknown pin.
  adapter.set_signal({"ghost", 999U, "value", 0.5});
  adapter.set_signal({"ghost2", cn, "nope", 0.5});

  std::string err;
  ASSERT_TRUE(g.evaluate(err)) << err;
  const auto snap = adapter.build(g);
  EXPECT_FALSE(snap.action("move"));
  EXPECT_FALSE(snap.action("ghost"));
  EXPECT_FALSE(snap.action("ghost2"));
}

TEST(GraphAnimBridge, BoolOutputDrivesActionDirectly) {
  ed::NodeGraph g;
  ed::register_builtin_node_types(g);
  const auto p = g.add_node("pulse",
                            {{"frequency", ed::NodeValue::make_number(1.0)},
                             {"threshold", ed::NodeValue::make_number(0.5)}});
  ed::GraphSignalAdapter adapter;
  adapter.set_signal({"move", p, "on", 0.5});

  std::string err;
  ed::GraphContext ctx;
  auto build_at = [&](double t) {
    ctx.time = t;
    EXPECT_TRUE(g.evaluate_with(ctx, err));
    return adapter.build(g);
  };
  // Pulse is on for the first half of each second.
  EXPECT_TRUE(build_at(0.25).action("move"));
  EXPECT_FALSE(build_at(0.75).action("move"));
  EXPECT_TRUE(build_at(1.25).action("move"));
}

TEST(GraphAnimBridge, PulseDrivesWalkIdleLikeAHeldKey) {
  MachineFixture f;
  ed::NodeGraph g;
  ed::register_builtin_node_types(g);
  const auto p = g.add_node("pulse",
                            {{"frequency", ed::NodeValue::make_number(0.5)},
                             {"threshold", ed::NodeValue::make_number(0.5)}});
  ed::GraphSignalAdapter adapter;
  adapter.set_signal({"move", p, "on", 0.5});

  std::string err;
  ed::GraphContext ctx;
  const float dt = 0.1F;
  std::string trace;
  for (int i = 0; i < 60; ++i) {  // 6 seconds = 1.5 pulse periods @0.5Hz
    ctx.time = i * dt;
    ctx.tick = static_cast<std::uint64_t>(i);
    ASSERT_TRUE(g.evaluate_with(ctx, err)) << err;
    const auto snap = adapter.build(g);
    f.machine.tick(snap, dt);
    trace += f.machine.state() == "walk" ? 'W' : 'I';
  }
  // The trace must contain BOTH states (the pulse toggled the machine).
  EXPECT_NE(trace.find('W'), std::string::npos) << trace;
  EXPECT_NE(trace.find('I'), std::string::npos) << trace;
}

TEST(GraphAnimBridge, FullChainReplaysByteExactly) {
  auto run = [] {
    ed::NodeGraph g;
    ed::register_builtin_node_types(g);
    const auto p =
        g.add_node("pulse", {{"frequency", ed::NodeValue::make_number(0.5)}});
    ed::GraphSignalAdapter adapter;
    adapter.set_signal({"move", p, "on", 0.5});
    MachineFixture f;
    std::string err;
    ed::GraphContext ctx;
    std::string trace;
    for (int i = 0; i < 60; ++i) {
      ctx.time = i * 0.1;
      ctx.tick = static_cast<std::uint64_t>(i);
      EXPECT_TRUE(g.evaluate_with(ctx, err));
      f.machine.tick(adapter.build(g), 0.1F);
      trace += f.machine.state() == "walk" ? 'W' : 'I';
    }
    return trace;
  };
  EXPECT_EQ(run(), run());
}

}  // namespace
