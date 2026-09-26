//! @file editor_session.cpp
//! @brief EditorSession bodies (see the header). JSON emission follows the
//!        document writer's conventions (sorted keys via std::map, %.17g
//!        doubles, minimal escaping) so snapshots stay byte-deterministic.

#include "engine/core/editor_session.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

#include "engine/core/contract.hpp"
#include "engine/core/control_server.hpp"
#include "engine/core/replay_scrubber.hpp"

namespace omnicpp::editor {

namespace {

//! %.17g double formatting (matches document.cpp's writer).
std::string num_to_string(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

//! Minimal JSON string escaping (quotes/backslash; control characters are
//! impossible in our payloads — names/keys come from the strict document
//! parser or the protocol reader, which both reject them).
std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
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

//! Registry type name for a type_id ("" when unknown).
std::string type_name_for(std::uint32_t type_id) {
  const auto* desc = default_registry().find(type_id);
  return desc != nullptr ? desc->name : "";
}

//! Serializes one PropValue (bare value; the caller provides the key).
std::string prop_to_json(const PropValue& v) {
  switch (v.type) {
    case PropValue::Type::Number:
      return num_to_string(v.number);
    case PropValue::Type::Bool:
      return v.boolean ? "true" : "false";
    case PropValue::Type::String:
      return quote(v.text);
    case PropValue::Type::Vec3:
      return "[" + num_to_string(v.vec[0]) + "," + num_to_string(v.vec[1]) +
             "," + num_to_string(v.vec[2]) + "]";
  }
  return "null";
}

const char* type_tag(PropValue::Type t) {
  switch (t) {
    case PropValue::Type::Number: return "number";
    case PropValue::Type::Bool: return "bool";
    case PropValue::Type::String: return "string";
    case PropValue::Type::Vec3: return "vec3";
  }
  return "unknown";
}

std::string object_to_json(const SceneObject& o) {
  std::string out = "{\"id\":";
  out += std::to_string(o.id);
  out += ",\"name\":" + quote(o.name);
  out += ",\"type\":" + quote(type_name_for(o.type_id));
  out += ",\"properties\":{";
  bool first = true;
  for (const auto& [key, value] : o.properties) {  // std::map: sorted
    if (!first) {
      out += ",";
    }
    first = false;
    out += quote(key) + ":" + prop_to_json(value);
  }
  out += "}}";
  return out;
}

//! True when `key` exists on `type` and `value` is type-compatible.
[[nodiscard]] bool validate_property(const ObjectTypeDesc& type,
                                     const std::string& key,
                                     const PropValue& value) {
  for (const auto& desc : type.properties) {
    if (desc.name == key) {
      return desc.default_value.type == value.type;
    }
  }
  return false;
}

}  // namespace

EditorSession::EditorSession() {
  // Seed the singleton environment object so camera/sun edits work on a
  // fresh session (matches the bridge's expectations).
  const auto* env_type = default_registry().find_by_name(kTypeEnvironment);
  OMNICPP_CONTRACT(env_type != nullptr);
  SceneObject env;
  env.id = kEnvironmentObjectId;
  env.type_id = env_type->id;
  env.name = "environment";
  for (const auto& desc : env_type->properties) {
    env.properties.emplace(desc.name, desc.default_value);
  }
  doc_.objects.push_back(std::move(env));
  doc_.next_object_id = kEnvironmentObjectId + 1;
  // The session owns the node-type registry: the same registered set the
  // node editor toolbar and document parser validate against.
  register_builtin_node_types(doc_.node_graph);
}

omnicpp::core::ControlReply EditorSession::on_control(
    const omnicpp::core::ControlCommand& command) {
  omnicpp::core::ControlReply reply;

  // 1. Document edits.
  if (handle_edit(command, reply)) {
    if (reply.ok && !replaying_) recorder_.record(command);
    return reply;
  }
  // 2. Queries.
  if (handle_query(command, reply)) {
    return reply;
  }
  // 3. Session passthrough.
  reply = handle_session(command);
  if (reply.ok && !replaying_) recorder_.record(command);
  return reply;
}

bool EditorSession::handle_edit(
    const omnicpp::core::ControlCommand& command,
    omnicpp::core::ControlReply& reply) {
  using CK = omnicpp::core::ControlCommand::Kind;

  switch (command.kind) {
    case CK::SetCamera:
    case CK::SetSun:
    case CK::SpawnCube: {
      const BridgeOutcome outcome =
          bridge_control_command(command, doc_, default_registry());
      if (outcome.kind == BridgeOutcome::Kind::Rejected) {
        reply.ok = false;
        reply.error = outcome.error;
        return true;
      }
      if (outcome.kind == BridgeOutcome::Kind::SessionOnly) {
        return false;  // Fall through to session handling.
      }
      std::string error;
      auto& mutable_outcome = const_cast<BridgeOutcome&>(outcome);
      std::unique_ptr<Command> cmd{mutable_outcome.command.release()};
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "edit failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = stack_.undo_top()->describe();
      return true;
    }
    case CK::SetProperty: {
      if (command.text.empty() || command.text2.empty()) {
        reply.ok = false;
        reply.error = "set_property needs \"object\" and \"key\"";
        return true;
      }
      SceneObject* obj = nullptr;
      for (auto& o : doc_.objects) {
        if (o.name == command.text) {
          obj = &o;
          break;
        }
      }
      if (obj == nullptr) {
        reply.ok = false;
        reply.error = "set_property: unknown object \"" + command.text + "\"";
        return true;
      }
      const auto* type = default_registry().find(obj->type_id);
      if (type == nullptr) {
        reply.ok = false;
        reply.error = "set_property: object type unknown";
        return true;
      }
      // Build the typed PropValue: x/y/z -> vec3, x alone -> number,
      // "value" string -> bool/string depending on the registered type.
      PropValue value;
      if (command.number_count >= 3U) {
        value = PropValue::make_vec3(command.numbers[0], command.numbers[1],
                                     command.numbers[2]);
      } else if (command.number_count == 1U) {
        value = PropValue::make_number(command.numbers[0]);
      } else {
        const PropValue* registered = nullptr;
        for (const auto& desc : type->properties) {
          if (desc.name == command.text2) {
            registered = &desc.default_value;
            break;
          }
        }
        if (registered != nullptr &&
            registered->type == PropValue::Type::Bool &&
            (command.text3 == "true" || command.text3 == "false")) {
          value = PropValue::make_bool(command.text3 == "true");
        } else if (registered != nullptr &&
                   registered->type == PropValue::Type::String) {
          value = PropValue::make_string(command.text3);
        } else {
          reply.ok = false;
          reply.error =
              "set_property: need x/y/z (vec3), x (number), or a valid "
              "\"value\" string";
          return true;
        }
      }
      if (!validate_property(*type, command.text2, value)) {
        reply.ok = false;
        reply.error = "set_property: unknown key or type mismatch for \"" +
                      command.text2 + "\"";
        return true;
      }
      auto set = std::make_unique<SetPropertyCommand>(obj->id, command.text2,
                                                      value);
      std::string error;
      if (!stack_.execute(std::move(set), error)) {
        reply.ok = false;
        reply.error = "set_property failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "set " + command.text + "." + command.text2;
      return true;
    }
    case CK::DestroyObject: {
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "destroy_object needs \"oid\"";
        return true;
      }
      const auto oid = static_cast<std::uint64_t>(command.numbers[0]);
      if (doc_.find(oid) == nullptr) {
        reply.ok = false;
        reply.error = "destroy_object: no object " + std::to_string(oid);
        return true;
      }
      auto destroy = std::make_unique<DestroyObjectCommand>(oid);
      std::string error;
      if (!stack_.execute(std::move(destroy), error)) {
        reply.ok = false;
        reply.error = "destroy_object failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "destroyed object " + std::to_string(oid);
      return true;
    }
    case CK::Select: {
      // Selection is editor state, not document state: it must not enter
      // the undo stack or the serialized document.
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "select needs \"oid\" (0 deselects)";
        return true;
      }
      const auto oid = static_cast<std::uint64_t>(command.numbers[0]);
      if (oid != 0U && doc_.find(oid) == nullptr) {
        reply.ok = false;
        reply.error = "select: no object " + std::to_string(oid);
        return true;
      }
      selected_id_ = oid;
      reply.ok = true;
      reply.detail = (oid == 0U) ? "deselected"
                                 : "selected object " + std::to_string(oid);
      return true;
    }
    case CK::NodeAdd: {
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "add_node needs \"type\"";
        return true;
      }
      const double x = command.number_count > 1U ? command.numbers[1]
                                                 : 40.0;
      const double y = command.number_count > 2U ? command.numbers[2]
                                                 : 40.0;
      auto cmd = std::make_unique<AddNodeCommand>(command.text, x, y);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "add_node failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "added node";
      return true;
    }
    case CK::NodeRemove: {
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "remove_node needs \"nid\"";
        return true;
      }
      const auto nid = static_cast<std::uint64_t>(command.numbers[0]);
      auto cmd = std::make_unique<RemoveNodeCommand>(nid);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "remove_node failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "removed node " + std::to_string(nid);
      return true;
    }
    case CK::LinkNodes: {
      if (command.number_count < 2U || command.text.empty() ||
          command.text2.empty()) {
        reply.ok = false;
        reply.error = "link_nodes needs \"from\",\"to\",\"out\",\"in\"";
        return true;
      }
      const auto from = static_cast<std::uint64_t>(command.numbers[0]);
      const auto to = static_cast<std::uint64_t>(command.numbers[1]);
      auto cmd = std::make_unique<LinkNodesCommand>(from, command.text, to,
                                                    command.text2);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "link_nodes failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "linked";
      return true;
    }
    case CK::UnlinkNodes: {
      if (command.number_count < 1U || command.text2.empty()) {
        reply.ok = false;
        reply.error = "unlink_nodes needs \"nid\" and \"in\"";
        return true;
      }
      const auto to = static_cast<std::uint64_t>(command.numbers[0]);
      auto cmd = std::make_unique<UnlinkNodeCommand>(to, command.text2);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "unlink_nodes failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "unlinked";
      return true;
    }
    case CK::SetNodeParam: {
      if (command.number_count < 1U || command.text.empty()) {
        reply.ok = false;
        reply.error = "set_node_param needs \"nid\" and \"key\"";
        return true;
      }
      const auto nid = static_cast<std::uint64_t>(command.numbers[0]);
      // Value currency: bare number (x), bool ("true"/"false"), or string.
      PropValue value;
      if (command.number_count >= 2U) {
        value = PropValue::make_number(command.numbers[1]);
      } else if (command.text2 == "true" || command.text2 == "false") {
        value = PropValue::make_bool(command.text2 == "true");
      } else {
        value = PropValue::make_string(command.text2);
      }
      auto cmd =
          std::make_unique<SetNodeParamCommand>(nid, command.text, value);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "set_node_param failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "param set";
      return true;
    }
    case CK::SetNodePosition: {
      if (command.number_count < 3U) {
        reply.ok = false;
        reply.error = "set_node_position needs \"nid\",\"x\",\"y\"";
        return true;
      }
      const auto nid = static_cast<std::uint64_t>(command.numbers[0]);
      auto cmd = std::make_unique<SetNodePositionCommand>(
          nid, command.numbers[1], command.numbers[2]);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "set_node_position failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "node moved";
      return true;
    }
    case CK::SaveDocument: {
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "save_document needs \"path\"";
        return true;
      }
      std::string error;
      if (!doc_.save_to_file(command.text, error)) {
        reply.ok = false;
        reply.error = "save_document failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "saved " + command.text;
      return true;
    }
    case CK::LoadDocument: {
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "load_document needs \"path\"";
        return true;
      }
      SceneDocument loaded;
      // Registry first: the parser validates node types against the target.
      register_builtin_node_types(loaded.node_graph);
      std::string error;
      if (!SceneDocument::load_from_file(command.text, loaded, error)) {
        reply.ok = false;
        reply.error = "load_document failed: " + error;
        return true;
      }
      // The document is authoritative state: loading REPLACES it and clears
      // history (undo across a load boundary is meaningless — the stack's
      // captured indices would not survive). Selection resets too.
      doc_ = std::move(loaded);
      stack_ = CommandStack(doc_);
      selected_id_ = 0;
      reply.ok = true;
      reply.detail = "loaded " + command.text;
      return true;
    }
    case CK::ScrubStart: {
      const auto frame = command.number_count > 0U
                             ? static_cast<std::uint64_t>(command.numbers[0])
                             : 0U;
      std::string error;
      if (!scrub_start(frame, error)) {
        reply.ok = false;
        reply.error = error;
        return true;
      }
      // W2: an explicit scrub_start during capture embeds a checkpoint at
      // the scrub TARGET frame (capture-start + explicit-snapshots density,
      // per the format spec; targets never collide, logical frames would).
      if (recorder_.active()) {
        recorder_.embed_checkpoint(doc_, frame);
      }
      reply.ok = true;
      reply.detail = "checkpoint @" + std::to_string(frame);
      return true;
    }
    case CK::ScrubTo: {
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "scrub_to needs frame";
        return true;
      }
      const auto frame = static_cast<std::uint64_t>(command.numbers[0]);
      std::string error;
      if (!scrub_to(frame, error)) {
        reply.ok = false;
        reply.error = error;
        return true;
      }
      reply.ok = true;
      reply.detail = "scrubbed to frame " + std::to_string(frame);
      return true;
    }
    case CK::ScrubInfo: {
      std::string out = "{\"checkpoints\":[";
      bool first = true;
      for (const auto f : scrubber_.frames()) {
        if (!first) {
          out += ",";
        }
        first = false;
        out += std::to_string(f);
      }
      out += "],\"count\":" + std::to_string(scrubber_.size()) +
             ",\"capacity\":" + std::to_string(scrubber_.capacity()) + "}";
      reply.ok = true;
      reply.detail = std::move(out);
      return true;
    }
    // v1.7 record/replay (W2).
    case CK::StartCapture: {
      const auto frame = command.number_count > 0U
                             ? static_cast<std::uint64_t>(command.numbers[0])
                             : 0U;
      std::string error;
      if (!capture_start(frame, command.text, error)) {
        reply.ok = false;
        reply.error = error;
        return true;
      }
      reply.ok = true;
      reply.detail = "recording from frame " + std::to_string(frame);
      return true;
    }
    case CK::StopCapture: {
      std::string error;
      if (!capture_stop(command.text, error)) {
        reply.ok = false;
        reply.error = error;
        return true;
      }
      reply.ok = true;
      reply.detail = "captured " + command.text;
      return true;
    }
    case CK::CaptureStatus: {
      std::string out = "{\"recording\":";
      out += recorder_.active() ? "true" : "false";
      out += ",\"frame\":" + std::to_string(recorder_.frame());
      out += ",\"commands\":" + std::to_string(recorder_.command_count());
      out += ",\"checkpoints\":" +
              std::to_string(recorder_.checkpoint_count());
      out += "}";
      reply.ok = true;
      reply.detail = std::move(out);
      return true;
    }
    case CK::LoadReplay: {
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "load_replay needs \"path\"";
        return true;
      }
      std::string error;
      if (!load_replay(command.text, error)) {
        reply.ok = false;
        reply.error = error;
        return true;
      }
      reply.ok = true;
      reply.detail = "replay loaded " + command.text;
      return true;
    }
    case CK::BindNodeProperty: {
      if (command.number_count < 2U || command.text.empty() ||
          command.text2.empty()) {
        reply.ok = false;
        reply.error = "bind_node_property needs nid, oid, pin, property";
        return true;
      }
      std::string error;
      if (!bind_property(
              static_cast<std::uint64_t>(command.numbers[0]), command.text,
              static_cast<std::uint64_t>(command.numbers[1]), command.text2,
              error)) {
        reply.ok = false;
        reply.error = "bind_node_property failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "bound";
      return true;
    }
    case CK::UnbindNodeProperty: {
      if (command.number_count < 1U || command.text.empty()) {
        reply.ok = false;
        reply.error = "unbind_node_property needs oid + property";
        return true;
      }
      if (!unbind_property(static_cast<std::uint64_t>(command.numbers[0]),
                           command.text)) {
        reply.ok = false;
        reply.error = "no such binding";
        return true;
      }
      reply.ok = true;
      reply.detail = "unbound";
      return true;
    }
    case CK::Undo:
    case CK::Redo: {
      std::string error;
      const bool is_undo = (command.kind == CK::Undo);
      const bool ok = is_undo ? stack_.undo(error) : stack_.redo(error);
      if (!ok) {
        reply.ok = false;
        reply.error = is_undo ? "nothing to undo" : "nothing to redo";
        return true;
      }
      reply.ok = true;
      reply.detail = is_undo ? "undone" : "redone";
      return true;
    }
    default:
      return false;  // Not an edit; caller continues.
  }
}

bool EditorSession::handle_query(
    const omnicpp::core::ControlCommand& command,
    omnicpp::core::ControlReply& reply) {
  using CK = omnicpp::core::ControlCommand::Kind;

  switch (command.kind) {
    case CK::ListObjects: {
      std::string out = "{\"objects\":[";
      bool first = true;
      for (const auto& o : doc_.objects) {
        if (!first) {
          out += ",";
        }
        first = false;
        out += object_to_json(o);
      }
      out += "],\"count\":";
      out += std::to_string(doc_.objects.size());
      out += "}";
      reply.ok = true;
      reply.detail = std::move(out);
      return true;
    }
    case CK::GetObject: {
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "get_object needs \"oid\"";
        return true;
      }
      const auto oid = static_cast<std::uint64_t>(command.numbers[0]);
      const SceneObject* obj = doc_.find(oid);
      if (obj == nullptr) {
        reply.ok = false;
        reply.error = "get_object: no object " + std::to_string(oid);
        return true;
      }
      reply.ok = true;
      reply.detail = object_to_json(*obj);
      return true;
    }
    case CK::Schema: {
      std::string out = "{\"types\":[";
      const auto& registry = default_registry();
      bool first = true;
      for (std::uint32_t i = 0; i < registry.type_count(); ++i) {
        const auto* t = registry.find(i);
        if (!first) {
          out += ",";
        }
        first = false;
        out += "{\"id\":" + std::to_string(t->id) +
               ",\"name\":" + quote(t->name) + ",\"properties\":{";
        bool fp = true;
        for (const auto& p : t->properties) {
          if (!fp) {
            out += ",";
          }
          fp = false;
          out += quote(p.name) +
                 ":{\"type\":\"" + type_tag(p.default_value.type) +
                 "\",\"default\":" + prop_to_json(p.default_value) + "}";
        }
        out += "}}";
      }
      out += "],\"schema_version\":";
      out += std::to_string(kDocumentSchemaVersion);
      out += "}";
      reply.ok = true;
      reply.detail = std::move(out);
      return true;
    }
    case CK::GetGraph: {
      reply.ok = true;
      reply.detail = doc_.node_graph.to_json();
      return true;
    }
    case CK::ListBindings: {
      std::string out = "{\"bindings\":[";
      bool first = true;
      for (const auto& b : bindings_) {
        if (!first) {
          out += ",";
        }
        first = false;
        out += "{\"node\":" + std::to_string(b.node_id) +
               ",\"pin\":" + quote(b.out_pin) +
               ",\"oid\":" + std::to_string(b.object_id) +
               ",\"property\":" + quote(b.property) + "}";
      }
      out += "],\"count\":" + std::to_string(bindings_.size()) + "}";
      reply.ok = true;
      reply.detail = std::move(out);
      return true;
    }
    default:
      return false;  // Not a query; caller continues.
  }
}

omnicpp::core::ControlReply EditorSession::handle_session(
    const omnicpp::core::ControlCommand& command) {
  using CK = omnicpp::core::ControlCommand::Kind;
  omnicpp::core::ControlReply reply;
  switch (command.kind) {
    case CK::Ping:
      reply.ok = true;
      reply.detail = "pong";
      break;
    case CK::Pause:
      reply.ok = true;
      reply.detail = "paused (session default; host may override)";
      break;
    case CK::Resume:
      reply.ok = true;
      reply.detail = "resumed (session default; host may override)";
      break;
    case CK::Step:
      reply.ok = true;
      reply.detail = "step acknowledged (host executes ticks)";
      break;
    case CK::Capture:
      reply.ok = true;
      reply.detail = "capture acknowledged (host executes)";
      break;
    default:
      reply.ok = false;
      reply.error = "unhandled command";
      break;
  }
  return reply;
}

bool EditorSession::bind_property(std::uint64_t node_id,
                                  std::string out_pin,
                                  std::uint64_t object_id,
                                  std::string property,
                                  std::string& error) {
  const GraphNode* node = doc_.node_graph.find(node_id);
  if (node == nullptr) {
    error = "bind: no node " + std::to_string(node_id);
    return false;
  }
  const auto* type = doc_.node_graph.find_type(node->type);
  bool pin_ok = false;
  PinType pin_type = PinType::Number;
  if (type != nullptr) {
    for (const auto& p : type->outputs) {
      if (p.name == out_pin) {
        pin_type = p.type;
        pin_ok = true;
        break;
      }
    }
  }
  if (!pin_ok) {
    error = "bind: node " + std::to_string(node_id) + " has no output \"" +
            out_pin + "\"";
    return false;
  }
  SceneObject* obj = doc_.find(object_id);
  if (obj == nullptr) {
    error = "bind: no object " + std::to_string(object_id);
    return false;
  }
  // Axis-suffix form "<property>.<x|y|z>": a NUMBER pin drives one
  // component of a VEC3 property. Validate the base property here; sync
  // writes the single component.
  std::string base_property = property;
  bool axis_form = false;
  int axis_index = -1;
  if (property.size() >= 3U && property[property.size() - 2U] == '.') {
    const char axis = property.back();
    axis_index = axis == 'x' ? 0 : (axis == 'y' ? 1 : (axis == 'z' ? 2 : -1));
    if (axis_index >= 0) {
      axis_form = true;
      base_property.erase(base_property.size() - 2U, 2U);
    }
  }
  const auto prop_it = obj->properties.find(base_property);
  if (prop_it == obj->properties.end()) {
    error = "bind: object " + std::to_string(object_id) +
            " has no property \"" + base_property + "\"";
    return false;
  }
  // Type compatibility: the pin's value type must match the property's
  // (axis form: number pin -> vec3 property component).
  const PropValue::Type target_type =
      axis_form ? PropValue::Type::Vec3
                : static_cast<PropValue::Type>(pin_type);
  if (prop_it->second.type != target_type) {
    error = "bind: pin type mismatch for \"" + base_property + "\"";
    return false;
  }
  if (axis_form && pin_type != PinType::Number) {
    error = "bind: axis binding needs a number pin";
    return false;
  }
  // One binding per base property (a property has ONE driver; the axis
  // suffix does not create a second driver slot).
  for (const auto& b : bindings_) {
    std::string_view existing = b.property;
    if (existing.size() >= 3U && existing[existing.size() - 2U] == '.') {
      existing.remove_suffix(2U);
    }
    if (b.object_id == object_id && existing == base_property) {
      error = "bind: property \"" + base_property + "\" on object " +
              std::to_string(object_id) + " is already bound";
      return false;
    }
  }
  bindings_.push_back(
      PropertyBinding{node_id, std::move(out_pin), object_id,
                      std::move(property)});
  (void)axis_index;
  return true;
}

bool EditorSession::unbind_property(std::uint64_t object_id,
                                    const std::string& property) {
  for (std::size_t i = 0; i < bindings_.size(); ++i) {
    if (bindings_[i].object_id == object_id &&
        bindings_[i].property == property) {
      bindings_.erase(bindings_.begin() +
                      static_cast<std::ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}

std::size_t EditorSession::sync_graph(std::string& error) {
  if (!doc_.node_graph.evaluate(error)) {
    return 0;
  }
  std::size_t applied = 0;
  for (const auto& b : bindings_) {
    const GraphNode* node = doc_.node_graph.find(b.node_id);
    SceneObject* obj = doc_.find(b.object_id);
    if (node == nullptr || obj == nullptr) {
      continue;  // dangling binding (node/object destroyed): skipped
    }
    const auto value_it = node->outputs.find(b.out_pin);
    if (value_it == node->outputs.end()) {
      continue;
    }
    // Axis-suffix bindings ("<property>.<x|y|z>"): a NUMBER pin writes one
    // component of the VEC3 property (validated at bind time).
    if (b.property.size() >= 3U && b.property[b.property.size() - 2U] == '.') {
      std::string base = b.property;
      base.erase(base.size() - 2U, 2U);
      const auto prop_it = obj->properties.find(base);
      if (prop_it == obj->properties.end() ||
          prop_it->second.type != PropValue::Type::Vec3 ||
          value_it->second.type != PropValue::Type::Number) {
        continue;
      }
      const char axis = b.property.back();
      const int idx =
          axis == 'x' ? 0 : (axis == 'y' ? 1 : (axis == 'z' ? 2 : -1));
      if (idx < 0) {
        continue;
      }
      prop_it->second.vec[idx] = value_it->second.number;
      ++applied;
      continue;
    }
    // Whole-property bindings: types must match exactly.
    const auto prop_it = obj->properties.find(b.property);
    if (prop_it == obj->properties.end()) {
      continue;
    }
    if (prop_it->second.type == value_it->second.type) {
      prop_it->second = value_it->second;
      ++applied;
    }
  }
  return applied;
}

bool EditorSession::scrub_start(std::uint64_t frame, std::string& error) {
  return scrubber_.capture(frame, doc_, error);
}

bool EditorSession::scrub_to(std::uint64_t frame, std::string& error) {
  SceneDocument restored;
  if (!scrubber_.restore(frame, restored, error)) {
    return false;
  }
  doc_ = std::move(restored);
  stack_ = CommandStack(doc_);  // undo cannot cross a time warp
  // Selection survives only when the restored document still has the object.
  if (selected_id_ != 0U && doc_.find(selected_id_) == nullptr) {
    selected_id_ = 0;
  }
  return true;
}

// ============================================================================
// W2: warploom-replay-v1 loading (docs/replay-format.md is the contract)
// ============================================================================

namespace {

//! Flat-JSON scans over one replay line (same approach as the protocol's
//! parse_command: targeted, dependency-free, strict about accepted shape).

[[nodiscard]] bool replay_find_string(const std::string& line, const char* key,
                                      std::string& out) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  const std::size_t open = line.find('"', key_pos + needle.size());
  if (open == std::string::npos) return false;
  const std::size_t close = line.find('"', open + 1);
  if (close == std::string::npos) return false;
  out = line.substr(open + 1, close - open - 1);
  return true;
}

[[nodiscard]] bool replay_find_u64(const std::string& line, const char* key,
                                   std::uint64_t& out) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  try {
    std::size_t consumed = 0;
    const unsigned long long v =
        std::stoull(line.substr(key_pos + needle.size()), &consumed);
    out = static_cast<std::uint64_t>(v);
    return consumed > 0U;
  } catch (const std::exception&) {
    return false;
  }
}

//! Parses the positional "n":[...] array (optional; empty when absent).
[[nodiscard]] bool replay_find_numbers(const std::string& line,
                                       std::vector<double>& out) {
  out.clear();
  const std::size_t key_pos = line.find("\"n\":[");
  if (key_pos == std::string::npos) return true;
  std::size_t cur = key_pos + 5U;
  while (cur < line.size() && line[cur] != ']') {
    try {
      std::size_t consumed = 0;
      out.push_back(std::stod(line.substr(cur), &consumed));
      cur += consumed;
    } catch (const std::exception&) {
      return false;
    }
    while (cur < line.size() && (line[cur] == ',' || line[cur] == ' ')) {
      ++cur;
    }
  }
  return cur < line.size();  // found the closing ']'
}

}  // namespace

bool EditorSession::load_replay(const std::string& path,
                                std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    error = "load_replay: cannot open \"" + path + "\": " +
            std::strerror(errno);
    return false;
  }
  std::vector<std::string> lines;
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  for (std::size_t pos = 0; pos < text.size();) {
    const std::size_t nl = text.find('\n', pos);
    const std::size_t end = nl == std::string::npos ? text.size() : nl;
    if (end > pos) lines.emplace_back(text.substr(pos, end - pos));
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  if (lines.size() < 2U) {
    error = "load_replay: truncated file (need header + end)";
    return false;
  }

  // Header contract.
  std::string record;
  std::string format;
  std::uint64_t schema_version = 0;
  if (!replay_find_string(lines.front(), "record", record) ||
      record != "header" ||
      !replay_find_string(lines.front(), "format", format) ||
      format != "warploom-replay-v1" ||
      !replay_find_u64(lines.front(), "schema_version", schema_version) ||
      schema_version != 1U) {
    error = "load_replay: not a warploom-replay-v1 file";
    return false;
  }
  // Truncation guard: the last line must be the end record.
  if (!replay_find_string(lines.back(), "record", record) ||
      record != "end") {
    error = "load_replay: truncated file (missing end record)";
    return false;
  }

  // Pass 1: collect command records and checkpoint pairs.
  struct LoggedCommand {
    std::uint64_t seq{0};
    omnicpp::core::ControlCommand command{};
  };
  std::vector<LoggedCommand> logged;
  struct HydratedCheckpoint {
    std::uint64_t frame{0};
    std::uint64_t hash{0};
    std::string json{};
  };
  std::vector<HydratedCheckpoint> hydrated;
  std::uint64_t expected_seq = 0;
  for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (!replay_find_string(line, "record", record)) {
      error = "load_replay: line " + std::to_string(i + 1) +
              " has no record field";
      return false;
    }
    if (record == "cmd") {
      std::string name;
      std::uint64_t seq = 0;
      std::vector<double> numbers;
      if (!replay_find_string(line, "cmd", name) ||
          !replay_find_u64(line, "seq", seq) ||
          !replay_find_numbers(line, numbers)) {
        error = "load_replay: malformed cmd record at line " +
                std::to_string(i + 1);
        return false;
      }
      LoggedCommand entry;
      entry.seq = seq;
      entry.command.number_count =
          static_cast<std::uint32_t>(
              std::min(numbers.size(), size_t{8}));
      for (std::uint32_t n = 0; n < entry.command.number_count; ++n) {
        entry.command.numbers[n] = numbers[n];
      }
      (void)replay_find_string(line, "t", entry.command.text);
      (void)replay_find_string(line, "t2", entry.command.text2);
      (void)replay_find_string(line, "t3", entry.command.text3);
      for (const auto& kn : omnicpp::core::ControlCommand::kind_names()) {
        if (name == kn.name) {
          entry.command.kind = kn.kind;
          break;
        }
      }
      if (entry.command.kind == omnicpp::core::ControlCommand::Kind::Unknown) {
        error = "load_replay: unknown command \"" + name + "\" at line " +
                std::to_string(i + 1);
        return false;
      }
      if (seq != expected_seq++) {
        error = "load_replay: seq gap at line " + std::to_string(i + 1);
        return false;
      }
      logged.push_back(std::move(entry));
    } else if (record == "ckpt") {
      HydratedCheckpoint cp;
      std::string hash_text;
      if (!replay_find_u64(line, "frame", cp.frame) ||
          !replay_find_string(line, "hash", hash_text)) {
        error = "load_replay: malformed ckpt record at line " +
                std::to_string(i + 1);
        return false;
      }
      if (i + 1 >= lines.size()) {
        error = "load_replay: ckpt at line " + std::to_string(i + 1) +
                " has no document line";
        return false;
      }
      cp.json = lines[++i];  // the raw document bytes
      cp.hash = std::stoull(hash_text);
      if (fnv1a64(cp.json.data(), cp.json.size()) != cp.hash) {
        error = "load_replay: checkpoint hash mismatch at line " +
                std::to_string(i);
        return false;
      }
      hydrated.push_back(std::move(cp));
    }
    // "header"/"end" records: already validated.
  }

  // Hydrate the scrubber: the loaded session's timeline REPLACES the ring.
  scrubber_.clear();
  for (const auto& cp : hydrated) {
    scrubber_.insert(cp.frame, cp.hash, cp.json);
  }

  // Re-apply needs a defined start state: restore the EARLIEST checkpoint
  // (the capture-start snapshot; `start` embeds at the start frame before
  // any command exists, so earliest == opening) before replaying the log.
  if (!hydrated.empty()) {
    std::string restore_error;
    if (!scrubber_.restore(hydrated.front().frame, doc_, restore_error)) {
      error = "load_replay: opening checkpoint failed to restore: " +
              restore_error;
      return false;
    }
    stack_ = CommandStack(doc_);
    selected_id_ = 0;
  }

  // Re-apply the command log in seq order through the normal mutation
  // authority. Re-applied commands are NOT re-recorded (spec): the guard
  // flag suppresses the on_control hook. A failed command aborts the load;
  // state is whatever the prefix produced and the hydrated checkpoints
  // remain available for scrub_to recovery.
  replaying_ = true;
  for (const auto& entry : logged) {
    const auto reply = on_control(entry.command);
    if (!reply.ok) {
      replaying_ = false;
      error = "load_replay: command seq " + std::to_string(entry.seq) +
              " (" +
              omnicpp::core::ControlCommand::kind_name(entry.command.kind) +
              ") failed: " + reply.error;
      return false;
    }
  }
  replaying_ = false;
  return true;
}

std::string EditorSession::snapshot_json() const {
  std::string out = "{\"objects\":[";
  bool first = true;
  for (const auto& o : doc_.objects) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += object_to_json(o);
  }
  out += "],\"count\":";
  out += std::to_string(doc_.objects.size());
  out += ",\"undo_depth\":";
  out += std::to_string(stack_.undo_count());
  out += ",\"redo_depth\":";
  out += std::to_string(stack_.redo_count());
  out += ",\"selected\":";
  out += std::to_string(selected_id_);
  out += ",\"nodes\":";
  out += std::to_string(doc_.node_graph.node_count());
  out += ",\"links\":";
  out += std::to_string(doc_.node_graph.link_count());
  out += ",\"graph_version\":";
  out += std::to_string(doc_.node_graph.version());
  out += "}";
  return out;
}

}  // namespace omnicpp::editor
