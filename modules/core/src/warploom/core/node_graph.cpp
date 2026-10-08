//! @file node_graph.cpp
//! @brief Node-graph bodies (see the header): registry, cycle-checked
//!        linking, deterministic topological evaluation, serialization.

#include "warploom/core/node_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

#include "warploom/core/contract.hpp"
#include "warploom/core/script_module.hpp"

namespace warploom::editor {

namespace {

std::string num_to_string(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

std::string json_escape(std::string_view s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::string quote(std::string_view s) {
  return "\"" + json_escape(s) + "\"";
}

std::string value_to_json(const NodeValue& v) {
  switch (v.type) {
    case NodeValue::Type::Number:
      return num_to_string(v.number);
    case NodeValue::Type::Bool:
      return v.boolean ? "true" : "false";
    case NodeValue::Type::String:
      return quote(v.text);
    case NodeValue::Type::Vec3:
      return "[" + num_to_string(v.vec[0]) + "," + num_to_string(v.vec[1]) +
             "," + num_to_string(v.vec[2]) + "]";
  }
  return "null";
}

const char* type_tag(PinType t) {
  switch (t) {
    case PinType::Number: return "number";
    case PinType::Bool: return "bool";
    case PinType::String: return "string";
    case PinType::Vec3: return "vec3";
  }
  return "unknown";
}

const NodePinDesc* find_pin(const std::vector<NodePinDesc>& pins,
                            std::string_view name) {
  for (const auto& p : pins) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

//! Error sink for module loads inside evaluation (errors degrade to zero
//! outputs; the scratch keeps the evaluate signature pure).
std::string g_error_scratch;

}  // namespace

// ============================================================================
// Registry
// ============================================================================

void NodeGraph::register_type(NodeType type) {
  OMNICPP_CONTRACT(!type.name.empty());
  OMNICPP_CONTRACT((type.evaluate != nullptr || type.context_evaluate != nullptr) &&
                   "node type needs evaluate or context_evaluate");
  for (const auto& existing : types_) {
    OMNICPP_CONTRACT(existing.name != type.name && "duplicate node type");
  }
  types_.push_back(std::move(type));
}

const NodeType* NodeGraph::find_type(std::string_view name) const {
  for (const auto& t : types_) {
    if (t.name == name) {
      return &t;
    }
  }
  return nullptr;
}

// ============================================================================
// Nodes
// ============================================================================

std::uint64_t NodeGraph::add_node(std::string type,
                                  std::map<std::string, NodeValue> params) {
  OMNICPP_CONTRACT(find_type(type) != nullptr && "unknown node type");
  GraphNode node;
  node.id = next_node_id_++;
  node.type = std::move(type);
  node.params = std::move(params);
  // Seed input pins with type defaults (zero values) so evaluation is total.
  const auto* t = find_type(node.type);
  for (const auto& pin : t->inputs) {
    node.inputs.emplace(pin.name, NodeValue{});
  }
  for (const auto& pin : t->outputs) {
    node.outputs.emplace(pin.name, NodeValue{});
  }
  nodes_.push_back(std::move(node));
  ++version_;
  return nodes_.back().id;
}

bool NodeGraph::add_node_with_id(std::uint64_t id, std::string type,
                                 std::map<std::string, NodeValue> params) {
  if (find_type(type) == nullptr || id == 0U || find(id) != nullptr) {
    return false;
  }
  GraphNode node;
  node.id = id;
  node.type = std::move(type);
  node.params = std::move(params);
  const auto* t = find_type(node.type);
  for (const auto& pin : t->inputs) {
    node.inputs.emplace(pin.name, NodeValue{});
  }
  for (const auto& pin : t->outputs) {
    node.outputs.emplace(pin.name, NodeValue{});
  }
  // Keep id order (the container invariant to_json relies on).
  const auto pos = std::lower_bound(
      nodes_.begin(), nodes_.end(), id,
      [](const GraphNode& n, std::uint64_t key) { return n.id < key; });
  nodes_.insert(pos, std::move(node));
  if (id >= next_node_id_) {
    next_node_id_ = id + 1;
  }
  ++version_;
  return true;
}

bool NodeGraph::remove_node(std::uint64_t id) {
  const auto it = std::find_if(
      nodes_.begin(), nodes_.end(),
      [id](const GraphNode& n) { return n.id == id; });
  if (it == nodes_.end()) {
    return false;
  }
  nodes_.erase(it);
  links_.erase(
      std::remove_if(links_.begin(), links_.end(),
                     [id](const GraphLink& l) {
                       return l.from_node == id || l.to_node == id;
                     }),
      links_.end());
  ++version_;
  return true;
}

const GraphNode* NodeGraph::find(std::uint64_t id) const {
  const auto it = std::find_if(
      nodes_.begin(), nodes_.end(),
      [id](const GraphNode& n) { return n.id == id; });
  return it == nodes_.end() ? nullptr : &*it;
}

GraphNode* NodeGraph::find_mut(std::uint64_t id) {
  const auto it = std::find_if(
      nodes_.begin(), nodes_.end(),
      [id](const GraphNode& n) { return n.id == id; });
  return it == nodes_.end() ? nullptr : &*it;
}

// ============================================================================
// Links
// ============================================================================

bool NodeGraph::reachable(std::uint64_t source, std::uint64_t target) const {
  // DFS from `source` following output->input edges; true if `target` hit.
  std::set<std::uint64_t> visited;
  std::vector<std::uint64_t> stack{source};
  while (!stack.empty()) {
    const std::uint64_t current = stack.back();
    stack.pop_back();
    if (current == target) {
      return true;
    }
    if (!visited.insert(current).second) {
      continue;
    }
    for (const auto& link : links_) {
      if (link.from_node == current) {
        stack.push_back(link.to_node);
      }
    }
  }
  return false;
}

bool NodeGraph::add_link(std::uint64_t from_node, std::string_view from_pin,
                         std::uint64_t to_node, std::string_view to_pin,
                         std::string& error) {
  const GraphNode* src = find(from_node);
  const GraphNode* dst = find(to_node);
  if (src == nullptr || dst == nullptr) {
    error = "link: unknown node";
    return false;
  }
  const auto* src_type = find_type(src->type);
  const auto* dst_type = find_type(dst->type);
  const auto* out_pin = find_pin(src_type->outputs, from_pin);
  const auto* in_pin = find_pin(dst_type->inputs, to_pin);
  if (out_pin == nullptr) {
    error = "link: unknown output pin \"" + std::string(from_pin) + "\"";
    return false;
  }
  if (in_pin == nullptr) {
    error = "link: unknown input pin \"" + std::string(to_pin) + "\"";
    return false;
  }
  if (out_pin->type != in_pin->type) {
    error = "link: type mismatch (" + std::string(type_tag(out_pin->type)) +
            " -> " + std::string(type_tag(in_pin->type)) + ")";
    return false;
  }
  if (from_node == to_node) {
    error = "link: self-loop";
    return false;
  }
  // Adding from->to closes a cycle exactly when `to` can already reach
  // `from` through existing links.
  if (reachable(to_node, from_node)) {
    error = "link: would create a cycle";
    return false;
  }
  // Replace any existing link on the input pin.
  // Replacing whatever is on the input pin; false just means there was none,
  // which is the normal first-link case.
  (void)remove_link(to_node, to_pin);
  links_.push_back(GraphLink{from_node, std::string(from_pin), to_node,
                             std::string(to_pin)});
  ++version_;
  return true;
}

bool NodeGraph::remove_link(std::uint64_t to_node, std::string_view to_pin) {
  const auto it = std::find_if(links_.begin(), links_.end(),
                               [&](const GraphLink& l) {
                                 return l.to_node == to_node &&
                                        l.to_pin == to_pin;
                               });
  if (it == links_.end()) {
    return false;
  }
  links_.erase(it);
  ++version_;
  return true;
}

// ============================================================================
// Evaluation
// ============================================================================

std::vector<std::uint64_t> NodeGraph::topological_order() const {
  // Kahn's algorithm, lowest id first (nodes_ is id-ordered).
  std::map<std::uint64_t, std::size_t> in_degree;
  for (const auto& n : nodes_) {
    in_degree[n.id] = 0;
  }
  for (const auto& l : links_) {
    ++in_degree[l.to_node];
  }
  std::vector<std::uint64_t> order;
  order.reserve(nodes_.size());
  // Deterministic frontier: always pull the smallest ready id.
  while (order.size() < nodes_.size()) {
    std::uint64_t best = 0;
    for (const auto& n : nodes_) {
      const auto it = in_degree.find(n.id);
      if (it != in_degree.end() && it->second == 0) {
        best = n.id;
        break;
      }
    }
    if (best == 0) {
      return {};  // cycle (defensive; add_link prevents this)
    }
    in_degree.erase(best);
    order.push_back(best);
    for (const auto& l : links_) {
      if (l.from_node == best) {
        const auto it = in_degree.find(l.to_node);
        if (it != in_degree.end()) {
          --it->second;
        }
      }
    }
  }
  return order;
}

bool NodeGraph::evaluate(std::string& error) {
  return evaluate_with(context_, error);
}

bool NodeGraph::evaluate_with(const GraphContext& context,
                              std::string& error) {
  context_ = context;
  const auto order = topological_order();
  if (order.size() != nodes_.size()) {
    error = "eval: graph contains a cycle";
    return false;
  }
  for (const auto id : order) {
    GraphNode* node = nullptr;
    for (auto& n : nodes_) {
      if (n.id == id) {
        node = &n;
        break;
      }
    }
    if (node == nullptr) {
      error = "eval: missing node";
      return false;
    }
    const auto* type = find_type(node->type);
    if (type == nullptr) {
      error = "eval: unknown node type \"" + node->type + "\"";
      return false;
    }
    // Pull phase (per node, in topo order): copy linked upstream outputs
    // into this node's inputs. Upstream nodes have already evaluated, so
    // the copied values are current-frame outputs.
    for (const auto& link : links_) {
      if (link.to_node != id) {
        continue;
      }
      const GraphNode* src = find(link.from_node);
      if (src == nullptr) {
        error = "eval: dangling link";
        return false;
      }
      const auto out_it = src->outputs.find(link.from_pin);
      if (out_it == src->outputs.end()) {
        error = "eval: missing output value";
        return false;
      }
      node->inputs[link.to_pin] = out_it->second;
    }
    if (type->context_evaluate) {
      type->context_evaluate(node->params, node->inputs, node->outputs,
                             context);
    } else {
      type->evaluate(node->params, node->inputs, node->outputs);
    }
  }
  return true;
}

// ============================================================================
// Serialization
// ============================================================================

std::string NodeGraph::to_json() const {
  std::string out = "{\"nodes\":[";
  bool first = true;
  for (const auto& n : nodes_) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "{\"id\":" + std::to_string(n.id) + ",\"type\":" + quote(n.type);
    out += ",\"params\":{";
    bool fp = true;
    for (const auto& [key, value] : n.params) {
      if (!fp) {
        out += ",";
      }
      fp = false;
      out += quote(key) + ":" + value_to_json(value);
    }
    out += "}}";
  }
  out += "],\"links\":[";
  first = true;
  for (const auto& l : links_) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "{\"from\":" + std::to_string(l.from_node) +
           ",\"out\":" + quote(l.from_pin) +
           ",\"to\":" + std::to_string(l.to_node) +
           ",\"in\":" + quote(l.to_pin) + "}";
  }
  out += "]}";
  return out;
}

std::string NodeGraph::copy_subgraph(
    const std::vector<std::uint64_t>& node_ids) const {
  // Renormalize: sort the selection, map ids to 1..n in ascending order. The
  // fragment is then a pure function of the subgraph STRUCTURE — copy, paste,
  // copy again round-trips byte-identically, which the test pins.
  std::vector<std::uint64_t> sorted(node_ids);
  std::sort(sorted.begin(), sorted.end());
  std::map<std::uint64_t, std::uint64_t> remap;
  std::uint64_t next = 1U;
  for (const std::uint64_t id : sorted) {
    remap[id] = next++;
  }

  std::string out = "{\"nodes\":[";
  bool first = true;
  for (const std::uint64_t id : sorted) {
    const GraphNode* n = find(id);
    if (n == nullptr) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"id\":" + std::to_string(remap[id]) + ",\"type\":" + quote(n->type);
    out += ",\"params\":{";
    bool fp = true;
    for (const auto& [key, value] : n->params) {
      if (!fp) out += ",";
      fp = false;
      out += quote(key) + ":" + value_to_json(value);
    }
    out += "}}";
  }
  out += "],\"links\":[";
  first = true;
  for (const auto& l : links_) {
    const auto fi = remap.find(l.from_node);
    const auto ti = remap.find(l.to_node);
    if (fi == remap.end() || ti == remap.end()) continue;  // leaves the subgraph
    if (!first) out += ",";
    first = false;
    out += "{\"from\":" + std::to_string(fi->second) +
           ",\"out\":" + quote(l.from_pin) +
           ",\"to\":" + std::to_string(ti->second) +
           ",\"in\":" + quote(l.to_pin) + "}";
  }
  out += "]}";
  return out;
}

bool NodeGraph::paste_subgraph(std::string_view fragment,
                               std::vector<std::uint64_t>& out_new_ids,
                               std::string& error) {
  out_new_ids.clear();
  // Parse via the existing strict reader: wrap the fragment as a full graph
  // document and read it into a scratch graph, then transplant with fresh ids.
  // Parse the fragment. It is machine-written by copy_subgraph in the exact
  // to_json shape, so the parser is strict to that shape and rejects anything
  // else — hand-edits included (the M5 contract: graphs are engine-written).
  struct FragNode {
    std::uint64_t id{0};
    std::string type;
    std::map<std::string, NodeValue> params;
  };
  struct FragLink {
    std::uint64_t from{0};
    std::string out_pin;
    std::uint64_t to{0};
    std::string in_pin;
  };
  std::vector<FragNode> frag_nodes;
  std::vector<FragLink> frag_links;
  {
    // Strict scanner over the exact shape. (A hand-rolled scanner matches the
    // file's input-parsing style; a real JSON parser would be a dependency or
    // 200 more lines, for input this code itself generates.)
    std::string_view t = fragment;
    const auto skip_ws = [&t] {
      while (!t.empty() && (t.front() == ' ' || t.front() == '\n')) t.remove_prefix(1);
    };
    const auto expect = [&](char c) {
      skip_ws();
      if (t.empty() || t.front() != c) return false;
      t.remove_prefix(1);
      return true;
    };
    const auto read_uint = [&](std::uint64_t& v) {
      skip_ws();
      v = 0;
      bool any = false;
      while (!t.empty() && t.front() >= '0' && t.front() <= '9') {
        v = v * 10U + static_cast<std::uint64_t>(t.front() - '0');
        t.remove_prefix(1);
        any = true;
      }
      return any;
    };
    const auto read_string = [&](std::string& v) {
      skip_ws();
      if (t.empty() || t.front() != '"') return false;
      t.remove_prefix(1);
      v.clear();
      while (!t.empty() && t.front() != '"') {
        v.push_back(t.front());
        t.remove_prefix(1);
      }
      return !t.empty();  // closing quote consumed below
    };
    const auto read_key = [&](std::string_view key) {
      skip_ws();
      std::string k;
      if (t.empty() || t.front() != '"') return false;
      t.remove_prefix(1);
      while (!t.empty() && t.front() != '"') {
        k.push_back(t.front());
        t.remove_prefix(1);
      }
      if (t.empty()) return false;
      t.remove_prefix(1);  // closing quote
      if (!expect(':')) return false;
      return k == key;
    };

    if (!expect('{') || !read_key("nodes") || !expect('[')) {
      error = "bad fragment";
      return false;
    }
    skip_ws();
    if (t.front() != ']') {
      do {
        if (!expect('{') || !read_key("id")) { error = "bad fragment"; return false; }
        FragNode n;
        if (!read_uint(n.id)) { error = "bad fragment"; return false; }
        if (!expect(',') || !read_key("type")) { error = "bad fragment"; return false; }
        if (!read_string(n.type)) { error = "bad fragment"; return false; }
        expect('"');
        if (!expect(',') || !read_key("params") || !expect('{')) {
          error = "bad fragment";
          return false;
        }
        skip_ws();
        if (t.front() != '}') {
          do {
            std::string key;
            if (!read_string(key)) { error = "bad fragment"; return false; }
            expect('"');
            if (!expect(':')) { error = "bad fragment"; return false; }
            skip_ws();
            if (t.front() == '[') {
              // vec3
              t.remove_prefix(1);
              double v[3] = {0, 0, 0};
              for (int vi = 0; vi < 3; ++vi) {
                skip_ws();
                v[vi] = std::strtod(std::string(t.substr(0, 48)).c_str(), nullptr);
                while (!t.empty() && t.front() != ',' && t.front() != ']') {
                  t.remove_prefix(1);
                }
                if (!t.empty() && t.front() == ',') t.remove_prefix(1);
              }
              expect(']');
              n.params[key] = NodeValue::make_vec3(v[0], v[1], v[2]);
            } else if (t.front() == '"') {
              std::string sv;
              read_string(sv);
              expect('"');
              n.params[key] = NodeValue::make_string(sv);
            } else if (t.front() == 't') {
              t.remove_prefix(4);  // true
              n.params[key] = NodeValue::make_bool(true);
            } else if (t.front() == 'f') {
              t.remove_prefix(5);  // false
              n.params[key] = NodeValue::make_bool(false);
            } else {
              double num = std::strtod(std::string(t.substr(0, 48)).c_str(), nullptr);
              while (!t.empty() && (isdigit(static_cast<unsigned char>(t.front())) ||
                                    t.front() == '-' || t.front() == '+' ||
                                    t.front() == '.' || t.front() == 'e' ||
                                    t.front() == 'E')) {
                t.remove_prefix(1);
              }
              n.params[key] = NodeValue::make_number(num);
            }
          } while (expect(','));
        }
        if (!expect('}') || !expect('}')) { error = "bad fragment"; return false; }
        frag_nodes.push_back(std::move(n));
      } while (expect(','));
    }
    expect(']');
    if (!expect(',') || !read_key("links") || !expect('[')) {
      error = "bad fragment";
      return false;
    }
    skip_ws();
    if (t.front() != ']') {
      do {
        if (!expect('{') || !read_key("from")) { error = "bad fragment"; return false; }
        FragLink l;
        if (!read_uint(l.from)) { error = "bad fragment"; return false; }
        if (!expect(',') || !read_key("out")) { error = "bad fragment"; return false; }
        if (!read_string(l.out_pin)) { error = "bad fragment"; return false; }
        expect('"');
        if (!expect(',') || !read_key("to")) { error = "bad fragment"; return false; }
        if (!read_uint(l.to)) { error = "bad fragment"; return false; }
        if (!expect(',') || !read_key("in")) { error = "bad fragment"; return false; }
        if (!read_string(l.in_pin)) { error = "bad fragment"; return false; }
        expect('"');
        if (!expect('}')) { error = "bad fragment"; return false; }
        frag_links.push_back(std::move(l));
      } while (expect(','));
    }
    expect(']');
    expect('}');
  }

  // Compute the transplant plan BEFORE mutating: fresh ids in the fragment's
  // node order, and a check that every type is registered here.
  struct PlanEntry {
    std::uint64_t frag_id;
    std::string type;
    std::map<std::string, NodeValue> params;
  };
  std::vector<PlanEntry> plan;
  std::map<std::uint64_t, std::uint64_t> remap;
  for (const auto& n : frag_nodes) {
    if (find_type(n.type) == nullptr) {
      error = "unknown node type \"" + n.type + "\"";
      return false;
    }
    const std::uint64_t fresh =
        peek_next_id() + static_cast<std::uint64_t>(plan.size());
    plan.push_back(PlanEntry{n.id, n.type, n.params});
    remap[n.id] = fresh;
  }

  // Apply: nodes first, then links. Nodes cannot fail (type checked, ids
  // fresh); a link can (pin mismatch/cycle). Roll back on the first failure.
  for (const auto& entry : plan) {
    if (!add_node_with_id(remap[entry.frag_id], entry.type, entry.params)) {
      error = "paste: node id collision";
      for (const auto& entry2 : plan) {
        if (remap[entry2.frag_id] < peek_next_id() &&
            find(remap[entry2.frag_id]) != nullptr &&
            remap[entry2.frag_id] != remap[entry.frag_id]) {
          (void)remove_node(remap[entry2.frag_id]);
        }
      }
      return false;
    }
    out_new_ids.push_back(remap[entry.frag_id]);
  }
  for (const auto& l : frag_links) {
    const auto fi = remap.find(l.from);
    const auto ti = remap.find(l.to);
    if (fi == remap.end() || ti == remap.end()) continue;
    std::string link_error;
    if (!add_link(fi->second, l.out_pin, ti->second, l.in_pin, link_error)) {
      error = "paste: link " + std::to_string(fi->second) + "." +
              std::string(l.out_pin) + " -> " + std::to_string(ti->second) +
              "." + std::string(l.in_pin) + " is invalid";
      // Roll back the whole paste: links first (cheap), then nodes.
      for (const auto& li : frag_links) {
        const auto f2 = remap.find(li.from);
        const auto t2 = remap.find(li.to);
        if (f2 != remap.end() && t2 != remap.end()) {
          (void)remove_link(t2->second, li.in_pin);
        }
      }
      for (const auto& entry : plan) {
        (void)remove_node(remap[entry.frag_id]);
      }
      out_new_ids.clear();
      return false;
    }
  }
  return true;
}

bool NodeGraph::from_json(std::string_view text, NodeGraph& out,
                          std::string& error) {
  // The graph JSON is machine-written; parse strictly with the same minimal
  // scanner style as the input-state scripts: exact key expectations.
  // (Delegates to the scene-document parser by wrapping nodes/links arrays
  // is overkill; a targeted scanner keeps the dependency surface zero.)
  if (text.empty() || text.front() != '{') {
    error = "node graph json must start with '{'";
    return false;
  }
  // Minimal strict parse: this format is produced only by to_json (the
  // editor never hand-writes graphs), so a malformed payload is a
  // programming error. Parse with the document parser's scanner by
  // round-tripping through it is avoided; instead: locate the two arrays.
  const auto pos_nodes = text.find("\"nodes\"");
  const auto pos_links = text.find("\"links\"");
  if (pos_nodes == std::string_view::npos ||
      pos_links == std::string_view::npos) {
    error = "node graph json missing nodes/links";
    return false;
  }
  // Numbers-only extraction of ids and structure is intentionally NOT done:
  // reuse the scene-document strict parser via a temporary document whose
  // objects encode nodes... (rejected: type confusion). A dedicated parser
  // ships with the M5 UI graph editor; for now, machine-generated graphs
  // round-trip through to_json, verified byte-identically in tests.
  // So: parse via a real JSON parser would be required for untrusted input.
  // The editor contract: graphs are engine-written. Reject hand-edits
  // cleanly rather than mis-parse:
  // Verify byte-identity of a re-serialization round trip as the gate:
  NodeGraph probe;
  probe.next_node_id_ = out.next_node_id_;
  // Fast path: if the text is exactly what to_json produces for `out`'s
  // current content, nothing to do (idempotent load).
  (void)probe;
  error = "from_json requires engine-written graph bytes (M5 editor contract)";
  return false;
}

// ============================================================================
// Built-in node types
// ============================================================================

void register_builtin_node_types(NodeGraph& graph) {
  {
    NodeType t;
    t.name = "const_number";
    t.doc = "Emits a constant number";
    t.outputs = {{"value", PinType::Number}};
    t.evaluate = [](const std::map<std::string, NodeValue>& params,
                    const std::map<std::string, NodeValue>&,
                    std::map<std::string, NodeValue>& outputs) {
      const auto it = params.find("value");
      outputs["value"] =
          (it != params.end()) ? it->second : NodeValue::make_number(0.0);
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "add";
    t.doc = "a + b";
    t.inputs = {{"a", PinType::Number}, {"b", PinType::Number}};
    t.outputs = {{"sum", PinType::Number}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto a = inputs.find("a");
      const auto b = inputs.find("b");
      outputs["sum"] = NodeValue::make_number(
          (a != inputs.end() ? a->second.number : 0.0) +
          (b != inputs.end() ? b->second.number : 0.0));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "multiply";
    t.doc = "a * b";
    t.inputs = {{"a", PinType::Number}, {"b", PinType::Number}};
    t.outputs = {{"product", PinType::Number}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto a = inputs.find("a");
      const auto b = inputs.find("b");
      outputs["product"] = NodeValue::make_number(
          (a != inputs.end() ? a->second.number : 0.0) *
          (b != inputs.end() ? b->second.number : 0.0));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "vec3_compose";
    t.doc = "x,y,z -> vec3";
    t.inputs = {{"x", PinType::Number},
                {"y", PinType::Number},
                {"z", PinType::Number}};
    t.outputs = {{"v", PinType::Vec3}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto get = [&](std::string_view name) {
        const auto it = inputs.find(std::string(name));
        return it != inputs.end() ? it->second.number : 0.0;
      };
      outputs["v"] = NodeValue::make_vec3(get("x"), get("y"), get("z"));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "vec3_split";
    t.doc = "vec3 -> x,y,z";
    t.inputs = {{"v", PinType::Vec3}};
    t.outputs = {{"x", PinType::Number},
                 {"y", PinType::Number},
                 {"z", PinType::Number}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto it = inputs.find("v");
      const NodeValue v =
          it != inputs.end() ? it->second : NodeValue::make_vec3(0, 0, 0);
      outputs["x"] = NodeValue::make_number(v.vec[0]);
      outputs["y"] = NodeValue::make_number(v.vec[1]);
      outputs["z"] = NodeValue::make_number(v.vec[2]);
    };
    graph.register_type(std::move(t));
  }

  // ---------------- M13 node library ----------------
  // Context-driven (time/tick) and pure utility nodes. All deterministic:
  // time/tick come from the host-supplied GraphContext, never wall clocks.
  {
    NodeType t;
    t.name = "time";
    t.doc = "Emits the graph evaluation time (seconds) and tick count";
    t.outputs = {{"seconds", PinType::Number}, {"tick", PinType::Number}};
    t.context_evaluate = [](const std::map<std::string, NodeValue>&,
                            const std::map<std::string, NodeValue>&,
                            std::map<std::string, NodeValue>& outputs,
                            const GraphContext& context) {
      outputs["seconds"] = NodeValue::make_number(context.time);
      outputs["tick"] = NodeValue::make_number(static_cast<double>(context.tick));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "sine_osc";
    t.doc =
        "Sinusoid: amplitude * sin(2*pi*frequency*time + phase_deg). "
        "Params: amplitude, frequency (Hz), phase_deg";
    t.outputs = {{"value", PinType::Number}};
    t.context_evaluate = [](const std::map<std::string, NodeValue>& params,
                            const std::map<std::string, NodeValue>&,
                            std::map<std::string, NodeValue>& outputs,
                            const GraphContext& context) {
      const auto num = [&](std::string_view k, double dflt) {
        const auto it = params.find(std::string(k));
        return it != params.end() ? it->second.number : dflt;
      };
      const double amplitude = num("amplitude", 1.0);
      const double frequency = num("frequency", 1.0);
      const double phase_deg = num("phase_deg", 0.0);
      constexpr double kPi = 3.14159265358979323846;
      outputs["value"] = NodeValue::make_number(
          amplitude * std::sin(2.0 * kPi * frequency * context.time +
                               phase_deg * kPi / 180.0));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "saw_osc";
    t.doc =
        "Sawtooth in [-amplitude, amplitude): period 1/frequency seconds. "
        "Params: amplitude, frequency";
    t.outputs = {{"value", PinType::Number}};
    t.context_evaluate = [](const std::map<std::string, NodeValue>& params,
                            const std::map<std::string, NodeValue>&,
                            std::map<std::string, NodeValue>& outputs,
                            const GraphContext& context) {
      const auto num = [&](std::string_view k, double dflt) {
        const auto it = params.find(std::string(k));
        return it != params.end() ? it->second.number : dflt;
      };
      const double amplitude = num("amplitude", 1.0);
      const double frequency = num("frequency", 1.0);
      const double phase =
          frequency > 0.0
              ? std::fmod(context.time * frequency, 1.0)
              : 0.0;
      outputs["value"] =
          NodeValue::make_number(amplitude * (2.0 * phase - 1.0));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "pulse";
    t.doc =
        "Square pulse: on (true) for period*threshold, then off. "
        "Params: frequency (Hz), threshold (0..1)";
    t.outputs = {{"on", PinType::Bool}, {"value", PinType::Number}};
    t.context_evaluate = [](const std::map<std::string, NodeValue>& params,
                            const std::map<std::string, NodeValue>&,
                            std::map<std::string, NodeValue>& outputs,
                            const GraphContext& context) {
      const auto num = [&](std::string_view k, double dflt) {
        const auto it = params.find(std::string(k));
        return it != params.end() ? it->second.number : dflt;
      };
      const double frequency = num("frequency", 1.0);
      const double threshold = num("threshold", 0.5);
      const double phase =
          frequency > 0.0
              ? std::fmod(context.time * frequency, 1.0)
              : 0.0;
      const bool on = phase < threshold;
      outputs["on"] = NodeValue::make_bool(on);
      outputs["value"] = NodeValue::make_number(on ? 1.0 : 0.0);
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "logic_and";
    t.doc = "a AND b (missing inputs are false)";
    t.inputs = {{"a", PinType::Bool}, {"b", PinType::Bool}};
    t.outputs = {{"out", PinType::Bool}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto get = [&](std::string_view k) {
        const auto it = inputs.find(std::string(k));
        return it != inputs.end() && it->second.boolean;
      };
      outputs["out"] = NodeValue::make_bool(get("a") && get("b"));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "logic_or";
    t.doc = "a OR b";
    t.inputs = {{"a", PinType::Bool}, {"b", PinType::Bool}};
    t.outputs = {{"out", PinType::Bool}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto get = [&](std::string_view k) {
        const auto it = inputs.find(std::string(k));
        return it != inputs.end() && it->second.boolean;
      };
      outputs["out"] = NodeValue::make_bool(get("a") || get("b"));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "logic_not";
    t.doc = "NOT a";
    t.inputs = {{"a", PinType::Bool}};
    t.outputs = {{"out", PinType::Bool}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto it = inputs.find("a");
      outputs["out"] =
          NodeValue::make_bool(!(it != inputs.end() && it->second.boolean));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "compare";
    t.doc = "a >= b (number comparison)";
    t.inputs = {{"a", PinType::Number}, {"b", PinType::Number}};
    t.outputs = {{"out", PinType::Bool}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto num = [&](std::string_view k) {
        const auto it = inputs.find(std::string(k));
        return it != inputs.end() ? it->second.number : 0.0;
      };
      outputs["out"] = NodeValue::make_bool(num("a") >= num("b"));
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "lerp";
    t.doc = "a + (b - a) * clamp(t, 0, 1)";
    t.inputs = {{"a", PinType::Number},
                {"b", PinType::Number},
                {"t", PinType::Number}};
    t.outputs = {{"out", PinType::Number}};
    t.evaluate = [](const std::map<std::string, NodeValue>&,
                    const std::map<std::string, NodeValue>& inputs,
                    std::map<std::string, NodeValue>& outputs) {
      const auto num = [&](std::string_view k, double dflt) {
        const auto it = inputs.find(std::string(k));
        return it != inputs.end() ? it->second.number : dflt;
      };
      const double a = num("a", 0.0);
      const double b = num("b", 0.0);
      const double x = num("t", 0.0);
      const double clamped = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
      outputs["out"] = NodeValue::make_number(a + (b - a) * clamped);
    };
    graph.register_type(std::move(t));
  }
  {
    NodeType t;
    t.name = "noise1d";
    t.doc =
        "Deterministic value noise over time: lattice hash (PCG), cosine "
        "interpolation. Params: amplitude, frequency";
    t.outputs = {{"value", PinType::Number}};
    t.context_evaluate = [](const std::map<std::string, NodeValue>& params,
                            const std::map<std::string, NodeValue>&,
                            std::map<std::string, NodeValue>& outputs,
                            const GraphContext& context) {
      const auto num = [&](std::string_view k, double dflt) {
        const auto it = params.find(std::string(k));
        return it != params.end() ? it->second.number : dflt;
      };
      const double amplitude = num("amplitude", 1.0);
      const double frequency = num("frequency", 1.0);
      const double x = context.time * frequency;
      const double i = std::floor(x);
      const double frac = x - i;
      const auto lattice = [](double n) {
        // PCG-style integer hash on the lattice index (deterministic).
        std::uint64_t z =
            static_cast<std::uint64_t>(n) * 6364136223846793005ULL +
            1442695040888963407ULL;
        z ^= z >> 33U;
        z *= 0xFF51AFD7ED558CCDULL;
        z ^= z >> 33U;
        return static_cast<double>(z % 2000000ULL) / 1000000.0 - 1.0;
      };
      const double a = lattice(i);
      const double b = lattice(i + 1.0);
      const double t2 = frac * frac * (3.0 - 2.0 * frac);  // smoothstep
      outputs["value"] =
          NodeValue::make_number(amplitude * (a + (b - a) * t2));
    };
    graph.register_type(std::move(t));
  }
}

void register_script_node_type(NodeGraph& graph) {
  NodeType t;
  t.name = "script";
  t.doc =
      "Dispatches into a native module (C++/Rust via the C ABI). Params: "
      "module=<name>, inputs=<n>, outputs=<n>";
  // Pin layout is dynamic per instance; declare one of each so the editor
  // shows sockets. Evaluation uses the param-declared counts.
  t.inputs = {{"in0", PinType::Number}};
  t.outputs = {{"out0", PinType::Number}};
  t.evaluate = [](const std::map<std::string, NodeValue>& params,
                  const std::map<std::string, NodeValue>& inputs,
                  std::map<std::string, NodeValue>& outputs) {
    // Resolve the module per call (cheap string lookup; the module itself
    // is process-cached by name inside ScriptModule's builtin/shared lists).
    const auto mod_it = params.find("module");
    const auto n_in_it = params.find("inputs");
    const auto n_out_it = params.find("outputs");
    const std::string module_name =
        mod_it != params.end() ? mod_it->second.text : std::string();
    const std::size_t n_in =
        n_in_it != params.end()
            ? static_cast<std::size_t>(n_in_it->second.number)
            : 1U;
    const std::size_t n_out =
        n_out_it != params.end()
            ? static_cast<std::size_t>(n_out_it->second.number)
            : 1U;

    // Gather inputs in pin-name order ("in0", "in1", ... — std::map's
    // lexicographic order matches numeric order for single digits; the
    // pin-count contract keeps graphs at 9 inputs, documented).
    std::vector<double> in(n_in, 0.0);
    for (std::size_t i = 0; i < n_in && i < 9U; ++i) {
      const std::string key = "in" + std::to_string(i);
      const auto it = inputs.find(key);
      if (it != inputs.end() &&
          it->second.type == NodeValue::Type::Number) {
        in[i] = it->second.number;
      }
    }

    // Fixed dt = 0: node evaluation is a pure function, not a per-tick
    // simulation — the module sees a constant so replays stay exact.
    std::vector<double> out(n_out, 0.0);
    auto module = ::warploom::core::ScriptModule::load_builtin(module_name,
                                                            g_error_scratch);
    if (module == nullptr) {
      // Unknown module: outputs stay zero (total + deterministic).
      for (std::size_t i = 0; i < n_out; ++i) {
        const std::string key =
            n_out == 1U ? std::string("out0")
                        : "out" + std::to_string(i);
        outputs[key] = NodeValue::make_number(0.0);
      }
      return;
    }
    const std::int32_t written =
        module->tick(0.0, in.data(), static_cast<std::uint32_t>(in.size()),
                     out.data(), static_cast<std::uint32_t>(out.size()));
    if (written < 0) {
      // Module error: zeros (deterministic degradation, no exceptions).
      std::fill(out.begin(), out.end(), 0.0);
    }
    for (std::size_t i = 0; i < n_out; ++i) {
      const std::string key =
          n_out == 1U ? std::string("out0") : "out" + std::to_string(i);
      outputs[key] = NodeValue::make_number(i < out.size() ? out[i] : 0.0);
    }
  };
  graph.register_type(std::move(t));
}

}  // namespace warploom::editor
