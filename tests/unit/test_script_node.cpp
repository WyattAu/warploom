//! @file test_script_node.cpp
//! @brief G4: the script node type, against a builtin module (no .so needed).

#include <cmath>
#include <map>

#include <gtest/gtest.h>

#include "warploom/core/node_graph.hpp"
#include "warploom/core/script_node.hpp"
#include "warploom/core/script_module.hpp"

namespace warploom::core {
namespace {

//! Builtin module: outputs[i] = inputs[i] * 2 + dt (the fixture ABI's own
//! example contract), deterministic in (dt, inputs).
ScriptModuleApi make_double_module() {
  ScriptModuleApi api;
  api.abi_version = []() noexcept { return kScriptModuleAbi; };
  api.name = []() noexcept { return "g4_double"; };
  api.tick = [](double dt, const double* inputs, std::uint32_t input_count,
                double* outputs,
                std::uint32_t output_capacity) noexcept -> std::int32_t {
    if (outputs == nullptr && output_capacity != 0U) return -1;
    const std::uint32_t n =
        input_count < output_capacity ? input_count : output_capacity;
    for (std::uint32_t i = 0; i < n; ++i) {
      outputs[i] = inputs[i] * 2.0 + dt;
    }
    return static_cast<std::int32_t>(n);
  };
  return api;
}

class ScriptNode : public ::testing::Test {
 protected:
  void SetUp() override {
    // register_builtin returns false on duplicate; the suite registers once.
    (void)ScriptModule::register_builtin("g4_double", make_double_module());
    module_ = ScriptModule::load_builtin("g4_double", error_);
    ASSERT_NE(module_, nullptr) << error_;
  }

  std::string error_;
  std::shared_ptr<ScriptModule> module_;
};

TEST_F(ScriptNode, TypeRegistersWithModuleNamedPins) {
  ::warploom::editor::NodeGraph g;
  register_script_node_type(g, module_, 2U, 1U);
  const auto* t = g.find_type("script:g4_double");
  ASSERT_NE(t, nullptr);
  ASSERT_EQ(t->inputs.size(), 2U);
  ASSERT_EQ(t->outputs.size(), 1U);
  EXPECT_EQ(t->inputs[0].name, "in0");
  EXPECT_EQ(t->outputs[0].name, "out0");
}

TEST_F(ScriptNode, TickFeedsInputsAndMapsOutputs) {
  ::warploom::editor::NodeGraph g;
  register_script_node_type(g, module_, 1U, 1U);
  const auto id = g.add_node("script:g4_double", {});
  ASSERT_NE(id, 0U);

  // Drive in0 directly, then evaluate through the graph's public entry.
  ::warploom::editor::GraphContext ctx;
  ctx.time = 0.5;
  ctx.tick = 1;
  // Seed inputs the way links do, then evaluate with the driven context.
  auto* node = g.find_mut(id);
  ASSERT_NE(node, nullptr);
  node->inputs["in0"] = ::warploom::editor::NodeValue::make_number(3.0);
  std::string error;
  ASSERT_TRUE(g.evaluate_with(ctx, error)) << error;
  const auto out = node->outputs.find("out0");
  ASSERT_NE(out, node->outputs.end());
  // dt on the first evaluation is 0 (documented first-call behaviour).
  EXPECT_DOUBLE_EQ(out->second.number, 6.0);
}

TEST_F(ScriptNode, DtFollowsTheTickSequence) {
  ::warploom::editor::NodeGraph g;
  register_script_node_type(g, module_, 1U, 1U);
  const auto id = g.add_node("script:g4_double", {});
  auto* node = g.find_mut(id);
  ASSERT_NE(node, nullptr);
  node->inputs["in0"] = ::warploom::editor::NodeValue::make_number(0.0);
  ::warploom::editor::GraphContext ctx;
  std::string error;
  ctx.time = 1.0; ctx.tick = 1;
  ASSERT_TRUE(g.evaluate_with(ctx, error)) << error;
  // in=0, dt=0 (first) -> out 0.
  EXPECT_DOUBLE_EQ(node->outputs["out0"].number, 0.0);
  ctx.time = 1.5; ctx.tick = 2;
  ASSERT_TRUE(g.evaluate_with(ctx, error)) << error;
  // in=0, dt=0.5 -> out 0.5.
  EXPECT_DOUBLE_EQ(node->outputs["out0"].number, 0.5);
  // Replay the SAME sequence in a fresh graph: byte-identical outputs.
  ::warploom::editor::NodeGraph g2;
  register_script_node_type(g2, module_, 1U, 1U);
  const auto id2 = g2.add_node("script:g4_double", {});
  auto* node2 = g2.find_mut(id2);
  node2->inputs["in0"] = ::warploom::editor::NodeValue::make_number(0.0);
  ::warploom::editor::GraphContext ctx2;
  std::string e2;
  ctx2.time = 1.0; ctx2.tick = 1;
  ASSERT_TRUE(g2.evaluate_with(ctx2, e2)) << e2;
  ctx2.time = 1.5; ctx2.tick = 2;
  ASSERT_TRUE(g2.evaluate_with(ctx2, e2)) << e2;
  EXPECT_DOUBLE_EQ(node2->outputs["out0"].number,
                   node->outputs["out0"].number);
}


// G4 .so path: the same node-type registration bound to a REAL dlopen'd
// module (the fixture .so built for the script-module tests). This is the
// path a gameplay module takes; the builtin path above proves the graph
// plumbing, this one proves the loader hand-off.
TEST_F(ScriptNode, SharedObjectModuleDrivesTheNode) {
#ifdef WARPLOOM_TEST_MODULE_OK
  std::string so_error;
  auto shared = ScriptModule::load_shared(WARPLOOM_TEST_MODULE_OK, so_error);
  ASSERT_NE(shared, nullptr) << so_error;
  EXPECT_STREQ(shared->module_name().data(), "fixture_ok");

  ::warploom::editor::NodeGraph g;
  register_script_node_type(
      g, std::shared_ptr<::warploom::core::ScriptModule>(shared.release()), 2U,
      2U);
  const auto id = g.add_node("script:fixture_ok", {});
  auto* node = g.find_mut(id);
  ASSERT_NE(node, nullptr);
  node->inputs["in0"] = ::warploom::editor::NodeValue::make_number(1.0);
  node->inputs["in1"] = ::warploom::editor::NodeValue::make_number(-2.0);

  ::warploom::editor::GraphContext ctx;
  std::string error;
  ctx.time = 0.1;
  ctx.tick = 1;
  ASSERT_TRUE(g.evaluate_with(ctx, error)) << error;
  // fixture_ok: outputs[i] = inputs[i] * 2 + dt; dt is 0 on first evaluation.
  EXPECT_DOUBLE_EQ(node->outputs["out0"].number, 2.0);
  EXPECT_DOUBLE_EQ(node->outputs["out1"].number, -4.0);
#else
  GTEST_SKIP() << "module fixtures unavailable";
#endif
}

}  // namespace
}  // namespace warploom::core
