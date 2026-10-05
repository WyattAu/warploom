//! @file test_animation_state_machine.cpp
//! @brief Deterministic state machine proofs: edges fire on action/time
//!        conditions, first-match ordering holds, blend weights ease
//!        linearly at the fade rate, and a scripted 200-tick input sequence
//!        produces byte-identical state/weight traces across two runs.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "warploom/core/animation_state_machine.hpp"

namespace {

//! Minimal input snapshot mirroring omnicpp::core::InputSnapshot's interface.
struct FakeSnapshot {
  std::string held;
  [[nodiscard]] bool action(const std::string& name) const {
    return name == held;
  }
};

//! Builds the mannequin configuration: Walk (level 0) <-> Idle (level 1),
//! bidirectional "fade_toggle" edges with a 0.25 s re-trigger guard.
omnicpp::anim::AnimationStateMachine<FakeSnapshot> make_walk_idle() {
  omnicpp::anim::AnimationStateMachine<FakeSnapshot> m;
  m.add_state("walk", 0.0f);
  m.add_state("idle", 1.0f);
  omnicpp::anim::AnimTransition to_idle;
  to_idle.from = "walk";
  to_idle.to = "idle";
  to_idle.action = "fade_toggle";
  to_idle.min_time_in_state = 0.25f;
  to_idle.fade_duration = 0.4f;
  m.add_transition(to_idle);
  omnicpp::anim::AnimTransition to_walk;
  to_walk.from = "idle";
  to_walk.to = "walk";
  to_walk.action = "fade_toggle";
  to_walk.min_time_in_state = 0.25f;
  to_walk.fade_duration = 0.4f;
  m.add_transition(to_walk);
  (void)m.set_initial("walk");
  return m;
}

constexpr float kDt = 1.0f / 60.0f;

TEST(AnimationStateMachine, HoldsInitialStateWithoutInput) {
  const auto m = make_walk_idle();
  EXPECT_EQ(m.state(), "walk");
  EXPECT_FLOAT_EQ(m.blended_weight(), 0.0f);
  EXPECT_FALSE(m.fading());
}

TEST(AnimationStateMachine, EdgeRequiresAction) {
  auto m = make_walk_idle();
  const FakeSnapshot none{""};
  m.tick(none, kDt);
  EXPECT_EQ(m.state(), "walk");  // no action -> no transition
}

TEST(AnimationStateMachine, EdgeRequiresMinTimeInState) {
  auto m = make_walk_idle();
  const FakeSnapshot pressed{"fade_toggle"};
  m.tick(pressed, kDt);  // 1/60 s < 0.25 s guard
  EXPECT_EQ(m.state(), "walk");
  m.tick(pressed, kDt * 20.0f);  // past the guard
  EXPECT_EQ(m.state(), "idle");
}

TEST(AnimationStateMachine, TransitionEasesWeightAtFadeRate) {
  auto m = make_walk_idle();
  const FakeSnapshot pressed{"fade_toggle"};
  m.tick(pressed, kDt * 20.0f);  // take walk -> idle (and ease one step)
  ASSERT_EQ(m.state(), "idle");
  // The transition tick itself eases one step of its own dt: weight =
  // min(1, 2.5 * 20 * dt) = 0.8333.
  EXPECT_FLOAT_EQ(m.blended_weight(), 2.5f * 20.0f * kDt);
  // Weight advances linearly by rate*dt per subsequent tick.
  const FakeSnapshot none{""};
  m.tick(none, kDt);
  EXPECT_FLOAT_EQ(m.blended_weight(), 0.8333333f + 2.5f * kDt);
  // ~0.4 s at 2.5/s to settle; 30 more ticks must saturate exactly.
  for (std::size_t i = 0; i < static_cast<std::size_t>(30); ++i) m.tick(none, kDt);
  EXPECT_FLOAT_EQ(m.blended_weight(), 1.0f);
  EXPECT_FALSE(m.fading());
}

TEST(AnimationStateMachine, FirstMatchWinsOnOverlappingEdges) {
  omnicpp::anim::AnimationStateMachine<FakeSnapshot> m;
  m.add_state("a", 0.0f);
  m.add_state("b", 1.0f);
  m.add_state("c", 0.5f);
  omnicpp::anim::AnimTransition first;
  first.from = "a";
  first.to = "b";
  first.action = "go";
  omnicpp::anim::AnimTransition second;
  second.from = "a";
  second.to = "c";
  second.action = "go";
  m.add_transition(first);  // declared first: must win
  m.add_transition(second);
  (void)m.set_initial("a");
  const FakeSnapshot pressed{"go"};
  m.tick(pressed, kDt);
  EXPECT_EQ(m.state(), "b");
}

TEST(AnimationStateMachine, ScriptedSequenceIsByteDeterministic) {
  // A 200-tick press/release script; run twice, compare full traces.
  auto run = [] {
    auto m = make_walk_idle();
    std::string trace;
    trace.reserve(200 * 64);
    for (std::size_t tick = 0; tick < static_cast<std::size_t>(200); ++tick) {
      // Press every 40th tick for 3 ticks: walk -> idle -> walk ...
      const bool held = (tick % 40) < 3;
      const FakeSnapshot snap{held ? "fade_toggle" : ""};
      m.tick(snap, kDt);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%u:%s:%.6f:%d;",
                    m.state_index(), m.state().c_str(),
                    static_cast<double>(m.blended_weight()),
                    m.fading() ? 1 : 0);
      trace += buf;
    }
    return trace;
  };
  const std::string a = run();
  const std::string b = run();
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(a, b);
  // And the trace must actually visit both states.
  EXPECT_NE(a.find("idle"), std::string::npos);
  EXPECT_NE(a.find("walk"), std::string::npos);
}

}  // namespace
