//! @file test_document.cpp
//! @brief warploom-core module smoke test: the document model is
//!        byte-deterministic and round-trips through the module's own
//!        public surface (warploom/core includes only; namespaces stay
//!        omnicpp::* in phase A — docs/warploom-core-plan.md).

#include <gtest/gtest.h>

#include <string>

#include "warploom/core/document.hpp"
#include "warploom/core/node_graph.hpp"

namespace {

TEST(WarploomCoreModule, DocumentRoundTripsByteIdentically) {
  omnicpp::editor::SceneDocument doc;
  omnicpp::editor::register_builtin_node_types(doc.node_graph);
  (void)doc.node_graph.add_node("const_number", {});
  const std::string bytes = doc.to_json();

  omnicpp::editor::SceneDocument parsed;
  omnicpp::editor::register_builtin_node_types(parsed.node_graph);
  std::string error;
  ASSERT_TRUE(
      omnicpp::editor::SceneDocument::from_json(bytes, parsed, error))
      << error;
  EXPECT_EQ(parsed.to_json(), bytes);
}

}  // namespace
