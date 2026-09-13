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

#include "engine/core/contract.hpp"
#include "engine/core/control_server.hpp"

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
}

omnicpp::core::ControlReply EditorSession::on_control(
    const omnicpp::core::ControlCommand& command) {
  omnicpp::core::ControlReply reply;

  // 1. Document edits.
  if (handle_edit(command, reply)) {
    return reply;
  }
  // 2. Queries.
  if (handle_query(command, reply)) {
    return reply;
  }
  // 3. Session passthrough.
  return handle_session(command);
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
  out += "}";
  return out;
}

}  // namespace omnicpp::editor
