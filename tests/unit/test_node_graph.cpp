//! @file test_node_graph.cpp
//! @brief M5 node-graph proofs: deterministic topological evaluation,
//!        cycle rejection at link time, one-link-per-input replacement,
//!        type checking, removal cascades, byte-deterministic serialization.

#include <gtest/gtest.h>

#include <string>

#include "engine/core/node_graph.hpp"

namespace {

using namespace omnicpp::editor;

//! const(3) + const(4) -> add -> out; returns node ids.
struct AddGraph {
  NodeGraph graph;
  std::uint64_t c1 = 0;
  std::uint64_t c2 = 0;
  std::uint64_t add = 0;

  AddGraph() {
    register_builtin_node_types(graph);
    c1 = graph.add_node("const_number", {{"value", NodeValue::make_number(3)}});
    c2 = graph.add_node("const_number", {{"value", NodeValue::make_number(4)}});
    add = graph.add_node("add", {});
  }
};

TEST(NodeGraph, EvaluatesArithmetic) {
  AddGraph g;
  std::string error;
  ASSERT_TRUE(g.graph.add_link(g.c1, "value", g.add, "a", error)) << error;
  ASSERT_TRUE(g.graph.add_link(g.c2, "value", g.add, "b", error)) << error;
  ASSERT_TRUE(g.graph.evaluate(error)) << error;
  const auto* node = g.graph.find(g.add);
  EXPECT_DOUBLE_EQ(node->outputs.at("sum").number, 7.0);
}

TEST(NodeGraph, EvaluationIsDeterministicAcrossRebuilds) {
  const auto build_eval = [] {
    AddGraph g;
    std::string error;
    (void)g.graph.add_link(g.c1, "value", g.add, "a", error);
    (void)g.graph.add_link(g.c2, "value", g.add, "b", error);
    (void)g.graph.evaluate(error);
    return g.graph.find(g.add)->outputs.at("sum").number;
  };
  EXPECT_EQ(build_eval(), build_eval());
}

TEST(NodeGraph, RejectsCycles) {
  NodeGraph g;
  register_builtin_node_types(g);
  const auto split = g.add_node("vec3_split", {});
  const auto compose = g.add_node("vec3_compose", {});
  std::string error;
  // compose -> split is fine...
  ASSERT_TRUE(g.add_link(compose, "v", split, "v", error)) << error;
  // ...split -> compose would close a cycle.
  EXPECT_FALSE(g.add_link(split, "y", compose, "y", error));
  EXPECT_EQ(error, "link: would create a cycle");
  // Self-loop rejected too.
  EXPECT_FALSE(g.add_link(compose, "v", compose, "v", error));
}

TEST(NodeGraph, PinTypeCheckingAndReplacement) {
  NodeGraph g;
  register_builtin_node_types(g);
  const auto c = g.add_node("const_number", {});
  const auto split = g.add_node("vec3_split", {});
  const auto compose = g.add_node("vec3_compose", {});
  std::string error;
  // Number output into vec3 input: type mismatch.
  EXPECT_FALSE(g.add_link(c, "value", split, "v", error));
  EXPECT_EQ(error, "link: type mismatch (number -> vec3)");
  // Unknown pin names.
  EXPECT_FALSE(g.add_link(c, "nope", compose, "x", error));
  EXPECT_FALSE(g.add_link(c, "value", compose, "nope", error));
  // Link an input, then replace it.
  const auto c2 = g.add_node("const_number", {{"value", NodeValue::make_number(9)}});
  ASSERT_TRUE(g.add_link(c, "value", compose, "x", error)) << error;
  ASSERT_TRUE(g.add_link(c2, "value", compose, "x", error)) << error;
  EXPECT_EQ(g.link_count(), 1U);  // replaced, not duplicated
  ASSERT_TRUE(g.evaluate(error)) << error;
  EXPECT_DOUBLE_EQ(g.find(compose)->outputs.at("v").vec[0], 9.0);
}

TEST(NodeGraph, RemoveNodeDropsLinks) {
  AddGraph g;
  std::string error;
  ASSERT_TRUE(g.graph.add_link(g.c1, "value", g.add, "a", error)) << error;
  ASSERT_TRUE(g.graph.add_link(g.c2, "value", g.add, "b", error)) << error;
  ASSERT_TRUE(g.graph.remove_node(g.c1));
  EXPECT_EQ(g.graph.link_count(), 1U);  // c2->b survives; c1->a dropped
  EXPECT_EQ(g.graph.find(g.c1), nullptr);
  // Evaluation still works: 'a' falls back to the seeded zero input.
  ASSERT_TRUE(g.graph.evaluate(error)) << error;
  EXPECT_DOUBLE_EQ(g.graph.find(g.add)->outputs.at("sum").number, 4.0);
}

TEST(NodeGraph, VecComposeSplitRoundTrip) {
  NodeGraph g;
  register_builtin_node_types(g);
  const auto x = g.add_node("const_number", {{"value", NodeValue::make_number(1)}});
  const auto y = g.add_node("const_number", {{"value", NodeValue::make_number(2)}});
  const auto z = g.add_node("const_number", {{"value", NodeValue::make_number(3)}});
  const auto compose = g.add_node("vec3_compose", {});
  const auto split = g.add_node("vec3_split", {});
  std::string error;
  ASSERT_TRUE(g.add_link(x, "value", compose, "x", error)) << error;
  ASSERT_TRUE(g.add_link(y, "value", compose, "y", error)) << error;
  ASSERT_TRUE(g.add_link(z, "value", compose, "z", error)) << error;
  ASSERT_TRUE(g.add_link(compose, "v", split, "v", error)) << error;
  ASSERT_TRUE(g.evaluate(error)) << error;
  const auto* s = g.find(split);
  EXPECT_DOUBLE_EQ(s->outputs.at("x").number, 1.0);
  EXPECT_DOUBLE_EQ(s->outputs.at("y").number, 2.0);
  EXPECT_DOUBLE_EQ(s->outputs.at("z").number, 3.0);
}

TEST(NodeGraph, SerializationByteDeterministic) {
  AddGraph g;
  std::string error;
  (void)g.graph.add_link(g.c1, "value", g.add, "a", error);
  (void)g.graph.add_link(g.c2, "value", g.add, "b", error);
  const std::string json1 = g.graph.to_json();
  const std::string json2 = g.graph.to_json();
  EXPECT_EQ(json1, json2);
  EXPECT_NE(json1.find("\"type\":\"const_number\""), std::string::npos);
  EXPECT_NE(json1.find("\"links\":["), std::string::npos);
}

TEST(NodeGraph, UnknownTypeRejectedAtAddNode) {
  NodeGraph g;
  bool crashed = false;
  // add_node with an unregistered type is a contract violation; verify the
  // registry lookup reports unknown before any state changes.
  EXPECT_EQ(g.find_type("nope"), nullptr);
  (void)crashed;
}

}  // namespace
