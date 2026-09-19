#pragma once

//! @file node_graph.hpp
//! @brief M5 node-graph authoring layer over the editor document.
//!
//! A `NodeGraph` is a directed acyclic graph of typed nodes whose evaluation
//! produces values. Design contracts (machine-checked in test_node_graph.cpp):
//!   - Deterministic: evaluation is a fixed topological order (Kahn's
//!     algorithm with the lowest node id first), so the same graph always
//!     evaluates identically — same contract as the rest of the engine.
//!   - Acyclic: `add_link` rejects any link that would create a cycle
//!     (checked by walking descendants of the target output).
//!   - Serializable: `to_json`/`from_json` round-trip byte-identically,
//!     using the same conventions as the scene document.
//!   - Engine-agnostic: node types are registered as factories (matching
//!     the property-registry pattern), so gameplay/authoring layers can add
//!     node kinds without touching the graph core.
//!
//! Data model: nodes have typed input/output pins. A link connects one
//! output pin to one input pin; each input accepts at most one link
//! (re-linking replaces). Node evaluation writes output-pin values from
//! input-pin values via a pure std::function — no hidden state.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/prop_value.hpp"  // PropValue as the value currency

namespace omnicpp::editor {

using NodeValue = PropValue;  // Number/Bool/String/Vec3 — reuse the typed set

//! Pin type = PropValue type (same serialization rules apply).
using PinType = PropValue::Type;

struct NodePinDesc final {
  std::string name;
  PinType type{PinType::Number};
};

//! One evaluated node instance.
struct GraphNode final {
  std::uint64_t id{0};
  std::string type;                        // registered factory name
  std::map<std::string, NodeValue> params{};  // sorted: deterministic
  // Runtime: current input/output pin values (keyed by pin name).
  std::map<std::string, NodeValue> inputs{};
  std::map<std::string, NodeValue> outputs{};
};

//! One link: from an output pin to an input pin. Inputs accept one link.
struct GraphLink final {
  std::uint64_t from_node{0};
  std::string from_pin;
  std::uint64_t to_node{0};
  std::string to_pin;
};

//! Node behavior: pin layout + pure evaluate function.
struct NodeType final {
  std::string name;
  std::string doc;
  std::vector<NodePinDesc> inputs{};
  std::vector<NodePinDesc> outputs{};
  //! Pure function: params + inputs -> outputs. Must not capture or mutate
  //! graph state. Type errors (wrong pin type) are contract violations.
  std::function<void(const std::map<std::string, NodeValue>& params,
                     const std::map<std::string, NodeValue>& inputs,
                     std::map<std::string, NodeValue>& outputs)>
      evaluate;
};

class NodeGraph final {
 public:
  //! Registers a node type; duplicate names are a contract violation.
  void register_type(NodeType type);
  [[nodiscard]] const NodeType* find_type(std::string_view name) const;

  //! Adds a node. Params must exist in the type's... (params are free-form;
  //! pin names are validated on link/eval). Returns the assigned id.
  [[nodiscard]] std::uint64_t add_node(std::string type,
                                       std::map<std::string, NodeValue> params);
  //! Deterministic-id variant (undo/redo + document restore): re-adding a
  //! node with its original id reproduces byte-identical state. Fails
  //! (returns false) when the id is already taken or the type is unknown;
  //! the graph's id cursor is moved past `id` either way so later nodes
  //! never collide with restored ones.
  [[nodiscard]] bool add_node_with_id(std::uint64_t id, std::string type,
                                      std::map<std::string, NodeValue> params);
  //! Removes a node and all links touching it. False when unknown.
  [[nodiscard]] bool remove_node(std::uint64_t id);
  //! The id the next add_node will assign (document serialization uses it).
  [[nodiscard]] std::uint64_t peek_next_id() const noexcept {
    return next_node_id_;
  }
  //! Monotonic change counter: bumps on every successful mutation. Hosts
  //! compare it to skip redundant rebuilds; undo/redo restores it so
  //! replays are deterministic.
  [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
  void set_version(std::uint64_t v) noexcept { version_ = v; }
  //! Restores the id cursor after an undone add (LIFO history guarantees
  //! the undone node held the highest claimed id). Contract: `v` exceeds
  //! every existing node id.
  void restore_id_cursor(std::uint64_t v) noexcept { next_node_id_ = v; }

  //! Links output (from_node, from_pin) -> input (to_node, to_pin). Any
  //! existing link on the input pin is replaced. Returns false when either
  //! node/pin is unknown, pin types mismatch, the target pin is not an
  //! input, or the link would create a cycle.
  [[nodiscard]] bool add_link(std::uint64_t from_node,
                              std::string_view from_pin,
                              std::uint64_t to_node,
                              std::string_view to_pin, std::string& error);
  //! Removes the link on an input pin. False when no link exists.
  [[nodiscard]] bool remove_link(std::uint64_t to_node,
                                 std::string_view to_pin);

  //! Evaluates in deterministic topological order. Returns false when the
  //! graph has an unknown node type or a type mismatch (`error` set).
  [[nodiscard]] bool evaluate(std::string& error);

  [[nodiscard]] const GraphNode* find(std::uint64_t id) const;
  [[nodiscard]] std::size_t node_count() const noexcept {
    return nodes_.size();
  }
  [[nodiscard]] std::size_t link_count() const noexcept {
    return links_.size();
  }
  [[nodiscard]] std::vector<GraphNode>& nodes_mutable() noexcept {
    return nodes_;
  }
  [[nodiscard]] const std::vector<GraphNode>& nodes() const noexcept {
    return nodes_;
  }
  [[nodiscard]] const std::vector<GraphLink>& links() const noexcept {
    return links_;
  }
  [[nodiscard]] const std::vector<NodeType>& types() const noexcept {
    return types_;
  }

  //! Byte-deterministic serialization (nodes by id, params by key order).
  [[nodiscard]] std::string to_json() const;
  //! Strict parse; on failure returns false with `error` set, `out` untouched.
  //! Types must be registered in the callee's graph.
  [[nodiscard]] static bool from_json(std::string_view text, NodeGraph& out,
                                      std::string& error);

 private:
  //! True when `target` is reachable from `source` through output->input
  //! links (cycle check walks the link graph downstream).
  [[nodiscard]] bool reachable(std::uint64_t source, std::uint64_t target) const;
  //! Deterministic topo order via Kahn's algorithm, lowest id first.
  //! Empty on cycle (cannot happen: add_link prevents cycles).
  [[nodiscard]] std::vector<std::uint64_t> topological_order() const;

  std::vector<NodeType> types_{};
  std::vector<GraphNode> nodes_{};  // id order (sorted by construction)
  std::vector<GraphLink> links_{};
  std::uint64_t next_node_id_{1};
  std::uint64_t version_{0};
};

//! Registers the built-in node set (math constants/ops, vec compose/split,
//! comment passthrough). Idempotent per graph instance.
void register_builtin_node_types(NodeGraph& graph);

}  // namespace omnicpp::editor
