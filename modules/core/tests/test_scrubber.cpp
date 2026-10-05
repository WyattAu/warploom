//! @file test_scrubber.cpp
//! @brief warploom-core module smoke test: checkpoint capture/restore is
//!        lossless through the module's public surface.

#include <gtest/gtest.h>

#include <string>

#include "warploom/core/document.hpp"
#include "warploom/core/node_graph.hpp"
#include "warploom/core/replay_scrubber.hpp"

namespace {

TEST(WarploomCoreModule, ScrubberCaptureRestoreIsLossless) {
  omnicpp::editor::SceneDocument doc;
  omnicpp::editor::register_builtin_node_types(doc.node_graph);
  omnicpp::editor::ReplayScrubber scrubber(8);

  std::string error;
  ASSERT_TRUE(scrubber.capture(3, doc, error)) << error;
  const std::string bytes = doc.to_json();

  (void)doc.node_graph.add_node("const_number", {});  // mutate past the checkpoint
  omnicpp::editor::SceneDocument restored;
  ASSERT_TRUE(scrubber.restore(3, restored, error)) << error;
  EXPECT_EQ(restored.to_json(), bytes);
}

}  // namespace
