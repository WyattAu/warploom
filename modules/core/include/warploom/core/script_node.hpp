#pragma once

//! @file script_node.hpp
//! @brief G4: a node-graph node type backed by a script module.
//!
//! The node's `context_evaluate` calls the module's
//! `warploom_module_tick(dt, inputs, n_in, outputs, n_out)` and maps the
//! returned doubles onto the node's Number output pins, exactly like the
//! built-in math nodes feed the M10 bindings. Module ticks must be pure
//! functions of (dt, inputs) — the ABI contract in
//! tests/module_fixture/module_abi.h states it and the builtin fixtures are
//! proven deterministic byte-for-byte — so replay determinism inherits from
//! the graph's existing tick order.
//!
//! dt comes from the graph context (time minus the time of the previous
//! evaluation of THIS node; 0 on the first call), not from a wall clock, so
//! replay re-simulation produces the same sequence.

#include <cstdint>
#include <memory>
#include <string>

#include <warploom/core/node_graph.hpp>
#include <warploom/core/script_module.hpp>

namespace warploom::editor {

//! Registers one script node type bound to `module`.
//!
//! The type is named "script:<module_name>". Pins: Number inputs "in0".. and
//! Number outputs "out0".., counts chosen by the caller to match the module's
//! expectation. The module handle is owned by the registered type (shared).
//! Missing input pins evaluate as 0; outputs beyond the module's returned
//! count stay 0.
void register_script_node_type(NodeGraph& graph,
                               std::shared_ptr<::warploom::core::ScriptModule> module,
                               std::uint32_t input_count,
                               std::uint32_t output_count);

}  // namespace warploom::editor