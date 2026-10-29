//! @file script_node.cpp
//! @brief G4 script node type (see script_node.hpp).

#include <warploom/core/script_node.hpp>

#include <algorithm>
#include <map>
#include <vector>

#include "warploom/core/contract.hpp"

namespace warploom::editor {

void register_script_node_type(NodeGraph& graph,
                               std::shared_ptr<::warploom::core::ScriptModule> module,
                               std::uint32_t input_count,
                               std::uint32_t output_count) {
  OMNICPP_CONTRACT(module != nullptr);
  OMNICPP_CONTRACT(input_count > 0U || output_count > 0U);

  NodeType t;
  t.name = "script:" + std::string(module->module_name());
  t.doc = "G4 script node: calls the module's tick(dt, inputs) and maps the "
          "returned doubles to out0..";
  for (std::uint32_t i = 0; i < input_count; ++i) {
    t.inputs.push_back({"in" + std::to_string(i), PinType::Number});
  }
  for (std::uint32_t i = 0; i < output_count; ++i) {
    t.outputs.push_back({"out" + std::to_string(i), PinType::Number});
  }
  // dt arrives in the per-node context (computed by NodeGraph::evaluate_with
  // from the graph-owned previous-time map). The old code kept one shared
  // TickState in the TYPE lambda: every node after the first of a type saw
  // dt=0 at a shared timestamp, and re-evaluating the same time gave a
  // different dt — the header's "deterministic under replay" claim was
  // false. The lambda is now stateless: a pure function of its arguments.
  t.context_evaluate =
      [module, input_count, output_count](
          const std::map<std::string, NodeValue>&,
          const std::map<std::string, NodeValue>& inputs,
          std::map<std::string, NodeValue>& outputs,
          const GraphContext& context) {
        const double dt = context.dt;

        std::vector<double> in(static_cast<std::size_t>(input_count), 0.0);
        for (std::uint32_t i = 0; i < input_count; ++i) {
          const auto it = inputs.find("in" + std::to_string(i));
          if (it != inputs.end() && it->second.type == PinType::Number) {
            in[i] = it->second.number;
          }
        }

        std::vector<double> out(static_cast<std::size_t>(output_count), 0.0);
        const std::int32_t written =
            module->tick(dt, in.data(), input_count, out.data(), output_count);
        // Negative return = module error: outputs stay 0, which is the same
        // failure shape as a math node given garbage. The module's own
        // diagnostics are its to print.
        const std::uint32_t n =
            written < 0 ? 0U
                        : std::min(static_cast<std::uint32_t>(written),
                                   output_count);
        for (std::uint32_t i = 0; i < output_count; ++i) {
          outputs["out" + std::to_string(i)] = NodeValue::make_number(
              i < n ? out[static_cast<std::size_t>(i)] : 0.0);
        }
      };
  graph.register_type(std::move(t));
}

}  // namespace warploom::editor