//! @file test_input_state.cpp
//! @brief Input abstraction proofs: edge detection across ticks, axis
//!        clamping, virtual driver script loading/rejection, and tick-ordered
//!        replay with held-state semantics.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "warploom/core/input_state.hpp"

namespace {

//! Writes a JSONL script to a temp file and returns its path.
std::string write_script(const std::string& content) {
  const std::string path = "/tmp/omnicpp_input_script_test.jsonl";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  return path;
}

}  // namespace

TEST(InputState, EdgeDetectionAcrossTicks) {
  omnicpp::core::InputState input;

  // Tick 1: press.
  input.begin_tick();
  input.set_action("jump", true);
  input.commit_tick();
  EXPECT_TRUE(input.action("jump"));
  EXPECT_TRUE(input.action_pressed("jump"));
  EXPECT_FALSE(input.action_released("jump"));

  // Tick 2: hold.
  input.begin_tick();
  input.set_action("jump", true);
  input.commit_tick();
  EXPECT_TRUE(input.action("jump"));
  EXPECT_FALSE(input.action_pressed("jump"));
  EXPECT_FALSE(input.action_released("jump"));

  // Tick 3: release.
  input.begin_tick();
  input.set_action("jump", false);
  input.commit_tick();
  EXPECT_FALSE(input.action("jump"));
  EXPECT_FALSE(input.action_pressed("jump"));
  EXPECT_TRUE(input.action_released("jump"));

  // Tick 4: still released — no edges.
  input.begin_tick();
  input.set_action("jump", false);
  input.commit_tick();
  EXPECT_FALSE(input.action_pressed("jump"));
  EXPECT_FALSE(input.action_released("jump"));
}

TEST(InputState, AxisAccumulationAndClamping) {
  omnicpp::core::InputState input;
  input.begin_tick();
  // Two devices contribute to the same axis; a third pushes past 1.
  input.set_axis("move_x", 0.6f);
  input.add_axis("move_x", 0.3f);
  input.add_axis("move_x", 0.5f);
  input.clamp_axes();
  input.commit_tick();
  EXPECT_FLOAT_EQ(input.axis("move_x"), 1.0f);

  // Delta is measured against the previous tick.
  input.begin_tick();
  input.set_axis("move_x", 0.25f);
  input.clamp_axes();
  input.commit_tick();
  EXPECT_FLOAT_EQ(input.axis("move_x"), 0.25f);
  EXPECT_FLOAT_EQ(input.axis_delta("move_x"), -0.75f);
}

TEST(VirtualInputDriver, ReplaysScriptWithHeldState) {
  const std::string path = write_script(
      "# comment line\n"
      "{\"tick\": 2, \"action\": \"jump\", \"value\": 1.0}\n"
      "{\"tick\": 4, \"action\": \"jump\", \"value\": 0.0}\n"
      "{\"tick\": 2, \"axis\": \"move_x\", \"value\": 0.5}\n");

  omnicpp::core::VirtualInputDriver driver;
  std::string error;
  ASSERT_TRUE(driver.load_script(path, error)) << error;
  ASSERT_EQ(driver.event_count(), 3U);

  omnicpp::core::InputState input;
  // Tick 0 (index 0): nothing yet.
  input.begin_tick();
  driver.poll(input);
  input.clamp_axes();
  input.commit_tick();
  EXPECT_FALSE(input.action("jump"));
  EXPECT_FLOAT_EQ(input.axis("move_x"), 0.0f);

  // Tick 1 (index 1): still nothing.
  input.begin_tick();
  driver.poll(input);
  input.clamp_axes();
  input.commit_tick();
  EXPECT_FALSE(input.action("jump"));

  // Tick 2 (index 2): both events land; action pressed, axis live.
  input.begin_tick();
  driver.poll(input);
  input.clamp_axes();
  input.commit_tick();
  EXPECT_TRUE(input.action_pressed("jump"));
  EXPECT_FLOAT_EQ(input.axis("move_x"), 0.5f);

  // Tick 3 (index 3): held state re-asserted, no new edge.
  input.begin_tick();
  driver.poll(input);
  input.clamp_axes();
  input.commit_tick();
  EXPECT_TRUE(input.action("jump"));
  EXPECT_FALSE(input.action_pressed("jump"));
  EXPECT_FLOAT_EQ(input.axis("move_x"), 0.5f);

  // Tick 4 (index 4): action released.
  input.begin_tick();
  driver.poll(input);
  input.clamp_axes();
  input.commit_tick();
  EXPECT_TRUE(input.action_released("jump"));
  EXPECT_FLOAT_EQ(input.axis("move_x"), 0.5f);  // axes persist until changed
}

TEST(VirtualInputDriver, RejectsMalformedScripts) {
  // Missing value.
  {
    const std::string path = write_script("{\"tick\": 1, \"action\": \"x\"}\n");
    omnicpp::core::VirtualInputDriver driver;
    std::string error;
    EXPECT_FALSE(driver.load_script(path, error));
    EXPECT_NE(error.find("value"), std::string::npos) << error;
  }
  // Both action and axis.
  {
    const std::string path = write_script(
        "{\"tick\": 1, \"action\": \"a\", \"axis\": \"b\", \"value\": 1}\n");
    omnicpp::core::VirtualInputDriver driver;
    std::string error;
    EXPECT_FALSE(driver.load_script(path, error));
    EXPECT_NE(error.find("exactly one"), std::string::npos) << error;
  }
  // Negative tick.
  {
    const std::string path = write_script(
        "{\"tick\": -3, \"action\": \"a\", \"value\": 1}\n");
    omnicpp::core::VirtualInputDriver driver;
    std::string error;
    EXPECT_FALSE(driver.load_script(path, error));
    EXPECT_NE(error.find("non-negative"), std::string::npos) << error;
  }
  // Unopenable file.
  {
    omnicpp::core::VirtualInputDriver driver;
    std::string error;
    EXPECT_FALSE(driver.load_script("/nonexistent/script.jsonl", error));
    EXPECT_NE(error.find("cannot open"), std::string::npos) << error;
  }
}
