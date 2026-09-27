#pragma once

//! @file graph_anim_bridge.hpp
//! @brief M13 fusion: node-graph outputs drive the animation state machine.
//!
//! The state machine consumes `InputSnapshot::action(name)` for its edges.
//! This adapter projects graph node OUTPUT values onto action names: a node
//! output pin's current value (>= threshold, or boolean true) holds the
//! action. Hosts call `set_signal(name, node_id, pin, threshold)` once at
//! setup and `build_snapshot(graph)` each tick before `machine.tick()`.
//!
//! Determinism: the projection is a pure function of the graph's evaluated
//! outputs, which are themselves pure functions of (params, inputs, time,
//! tick) — so the full graph -> animation chain replays byte-exactly.

#include <string>
#include <unordered_map>
#include <vector>

#include "warploom/core/animation_state_machine.hpp"
#include "warploom/core/input_state.hpp"
#include "warploom/core/node_graph.hpp"

namespace omnicpp::editor {

//! One output-pin -> action mapping.
struct GraphSignal {
  std::string action{};   //!< action name in the produced snapshot
  std::uint64_t node_id{0};
  std::string pin{};
  double threshold{0.5};  //!< number pins: held when value >= threshold
};

//! Projects evaluated graph outputs onto a set of named actions.
class GraphSignalAdapter final {
 public:
  //! Registers/replaces a signal mapping. Evaluated after each build.
  void set_signal(GraphSignal signal) {
    signals_.push_back(std::move(signal));
  }

  //! Removes all mappings for `action`.
  void clear_action(const std::string& action) {
    for (std::size_t i = signals_.size(); i-- > 0;) {
      if (signals_[i].action == action) {
        signals_.erase(signals_.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
  }

  //! Evaluates every mapping against the graph's CURRENT outputs and
  //! produces the snapshot the state machine ticks on. Unmapped actions
  //! stay unheld; missing pins/nodes leave their actions unheld (dangling
  //! mappings degrade, never throw).
  [[nodiscard]] omnicpp::core::InputSnapshot build(const NodeGraph& graph) const {
    omnicpp::core::InputSnapshot snap;
    for (const auto& s : signals_) {
      const GraphNode* node = graph.find(s.node_id);
      if (node == nullptr) continue;
      const auto it = node->outputs.find(s.pin);
      if (it == node->outputs.end()) continue;
      bool held = false;
      switch (it->second.type) {
        case PropValue::Type::Bool:
          held = it->second.boolean;
          break;
        case PropValue::Type::Number:
          held = it->second.number >= s.threshold;
          break;
        default:
          held = false;
          break;
      }
      snap.actions[s.action] = held;
    }
    return snap;
  }

  [[nodiscard]] const std::vector<GraphSignal>& signals() const noexcept {
    return signals_;
  }

 private:
  std::vector<GraphSignal> signals_{};
};

}  // namespace omnicpp::editor
