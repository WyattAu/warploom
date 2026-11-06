//! @file editor_session.cpp
//! @brief EditorSession bodies (see the header). JSON emission follows the
//!        document writer's conventions (sorted keys via std::map, %.17g
//!        doubles, minimal escaping) so snapshots stay byte-deterministic.

#include "warploom/core/editor_session.hpp"

#include <charconv>
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

#include "warploom/core/contract.hpp"
#include "warploom/core/control_server.hpp"
#include "warploom/core/physics_snapshot.hpp"
#include "warploom/core/replay_scrubber.hpp"

namespace warploom::editor {

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

//! Append a float in a form that round-trips EXACTLY as a float.
//!
//! Widening to double and printing that would also round-trip -- every float is
//! representable in double, and narrowing back is lossless -- but it prints the
//! double's shortest form, which is three to four times longer: 1e+20 becomes
//! 21 characters and 3.4e+38 becomes 22. Physics telemetry is written every
//! frame, so that is bytes spent saying nothing. Serialising at float precision
//! gives the shortest text that still recovers the original float exactly, and
//! keeps the format independent of double entirely.
void append_float(std::string& out, float value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc{}) {
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
  } else {
    out += "0";
  }
}

}  // namespace

// ============================================================================
// G3: timeline clips — armed-record/playback tick + disarm points
// ============================================================================

// File-local by intent; external linkage is what -Wmissing-declarations flags.
namespace {
//! Splits "property" or "property.<x|y|z>" (the M10 axis-suffix form).
//! axis = -1 for the whole-property form.
void split_axis_property(const std::string& property, std::string& base,
                         int& axis) {
  const std::size_t dot = property.rfind('.');
  if (dot != std::string::npos && dot + 2 == property.size() &&
      (property[dot + 1] == 'x' || property[dot + 1] == 'y' ||
       property[dot + 1] == 'z')) {
    base = property.substr(0, dot);
    axis = property[dot + 1] == 'x' ? 0 : (property[dot + 1] == 'y' ? 1 : 2);
    return;
  }
  base = property;
  axis = -1;
}

}  // namespace


namespace {
//! Append a double in a form that round-trips EXACTLY. Physics state has to
//! survive a snapshot -> resume cycle bit-for-bit: std::to_string gives six
//! decimals, so a body would come back slightly moved and the simulation would
//! diverge from the original on the next tick. to_chars' shortest round-trip
//! form is both exact and deterministic for a given value.
void append_number(std::string& out, double value) {
  char buffer[32];
  const auto result =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc{}) {
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
  } else {
    out += "0";
  }
}
}  // namespace

std::string EditorSession::physics_bytes() const {
  return physics_snapshot_json(physics_, physics_bodies_);
}

// ============================================================================
// T5c: reserved render_settings object (trackable render channels)
// ============================================================================

bool EditorSession::is_render_channel(const std::string& key) noexcept {
  return key == "exposure" || key == "bloom" || key == "render_mode" ||
         key == "tone_map";
}

const SceneObject* EditorSession::find_render_settings() const noexcept {
  const ObjectTypeDesc* type =
      default_registry().find_by_name(kTypeRenderSettings);
  if (type == nullptr) return nullptr;
  for (const SceneObject& object : doc_.objects) {
    if (object.type_id == type->id) return &object;
  }
  return nullptr;
}

SceneObject* EditorSession::ensure_render_settings(std::string& error) {
  const ObjectTypeDesc* type =
      default_registry().find_by_name(kTypeRenderSettings);
  if (type == nullptr) {
    error = "render_settings type missing from registry";
    return nullptr;
  }
  for (SceneObject& object : doc_.objects) {
    if (object.type_id == type->id) return &object;
  }
  std::map<std::string, PropValue> props;
  for (const PropertyDesc& desc : type->properties) {
    props.emplace(desc.name, desc.default_value);
  }
  auto spawn = std::make_unique<SpawnObjectCommand>(
      doc_.next_object_id, type->id, "render_settings", std::move(props));
  const std::uint64_t fresh_id = doc_.next_object_id;
  if (!stack_.execute(std::move(spawn), error)) {
    error = "render_settings spawn failed: " + error;
    return nullptr;
  }
  return doc_.find(fresh_id);
}

EditorSession::RenderConfig EditorSession::render_config() const noexcept {
  RenderConfig cfg{};
  cfg.exposure_driven = (render_driven_ & 0x01U) != 0U;
  cfg.bloom_driven = (render_driven_ & 0x02U) != 0U;
  cfg.render_mode_driven = (render_driven_ & 0x04U) != 0U;
  cfg.tone_map_driven = (render_driven_ & 0x08U) != 0U;
  const SceneObject* rs = find_render_settings();
  if (rs == nullptr) return cfg;
  const auto get = [&](const char* key, double fallback) {
    const auto it = rs->properties.find(key);
    if (it == rs->properties.end() ||
        it->second.type != PropValue::Type::Number ||
        !std::isfinite(it->second.number)) {
      return fallback;
    }
    return it->second.number;
  };
  cfg.exposure = get("exposure", 1.0);
  cfg.bloom = get("bloom", 0.0);
  cfg.render_mode = get("render_mode", 0.0);
  cfg.tone_map = get("tone_map", 0.0);
  return cfg;
}

void EditorSession::advance_timeline(std::uint64_t frame, double sub_frame) {
  // One body for step-hold and interpolated playback (D2b): two
  // implementations would drift. NaN fails the > 0.0 check and lands on
  // step-hold — the same fate as a hostile live host's fraction.
  if (sub_frame > 0.0) {
    tick_timeline_interpolated(frame, sub_frame, Easing::Linear);
  } else {
    tick_timeline(frame);
  }
}

std::uint32_t EditorSession::step_physics(double fixed_dt) {
  // Substep count is derived from fixed_dt alone, never from wall-clock, so
  // a replay that re-ticks the same frames reproduces the same integration.
  // Clamped so a pathological dt cannot make one tick arbitrarily expensive.
  // Shared by tick() and load_replay's re-apply loop: one derivation, one
  // integration order, or the two would drift.
  auto substeps = static_cast<std::uint32_t>(
      std::ceil(fixed_dt / kMaxPhysicsSubstep));
  substeps = std::clamp<std::uint32_t>(substeps, 1U, kMaxPhysicsSubsteps);
  const auto substep_dt =
      static_cast<float>(fixed_dt / static_cast<double>(substeps));
  for (std::uint32_t i = 0; i < substeps; ++i) physics_.step(substep_dt);
  return substeps;
}

EditorSession::TickReport EditorSession::tick(const FrameInput& input) {
  TickReport report{};

  // Latch the host's fixed step for capture headers (T4a): both shipped
  // hosts tick at a constant dt, so the last positive value is the
  // increment the live simulation integrated with.
  if (input.fixed_dt > 0.0) {
    last_fixed_dt_ = input.fixed_dt;
  }

  // 1. Bindings write their pin values into object properties.
  std::string error;
  const std::size_t applied = sync_graph(error);
  report.bindings_applied = static_cast<std::uint32_t>(applied);
  // A graph evaluation failure is NOT a tick failure: bindings that resolve
  // still apply, and the session has always treated a bad binding as
  // non-fatal. Reporting it lets a host surface it without a host-specific
  // second error path.
  report.synced = error.empty();

  // 2. Physics. Skipped while paused -- a paused host must not advance the
  // simulation, but must still see graph edits apply, which is why this sits
  // after sync_graph.
  if (!input.paused && input.fixed_dt > 0.0) {
    report.physics_substeps = step_physics(input.fixed_dt);

    // Push the simulated poses into the projected store so the renderer shows
    // the simulation. Done before project() below? No -- project() refreshes
    // transforms from the document, which would overwrite them. So the pose
    // write happens after project(), below.
  }

  // 3. The clip wins the frame against a binding on the same property. With a
  // sub-frame position the sample point moves inside the frame; at alpha 0 this
  // is the old step-hold path exactly.
  //
  // Gated on pause, like physics above. Previously playback kept running while
  // paused: the physics stage checked `paused` and the timeline stage did not,
  // so a paused host stopped bodies moving but carried on driving recorded
  // values. That only became visible once a sub-frame tick could move the
  // sample point inside a frame -- at whole frames the pause was easy to miss
  // because playback mostly held. Both stages now obey the same gate.
  if (!input.paused) {
    advance_timeline(input.frame, input.sub_frame);
  }

  // 4. Carry the document into the ECS. After the timeline so a clip's write
  // is what gets projected, not the pre-playback value.
  (void)projection_.project(doc_);

  // 5. Physics poses override the authored transform, but only for objects
  // that actually have a body: project() has just written the authored value
  // for everything, so a simulated object would otherwise snap back to its
  // authored position on the next tick.
  for (const auto& [object_id, body_id] : physics_bodies_) {
    if (!projection_.projected(object_id)) continue;
    const auto entity = projection_.entity_for(object_id);
    if (!projection_.world().has_component<DocumentTransform>(entity)) continue;
    const auto& body = physics_.body(body_id);
    auto& transform =
        projection_.world().get_component<DocumentTransform>(entity);
    transform.position[0] = static_cast<double>(body.position[0]);
    transform.position[1] = static_cast<double>(body.position[1]);
    transform.position[2] = static_cast<double>(body.position[2]);
  }

  return report;
}

bool EditorSession::spawn_physics_body(
    std::uint64_t object_id, const ::warploom::physics::PhysicsBody& body) {
  if (!projection_.projected(object_id)) return false;
  physics_bodies_[object_id] = physics_.add_body(body);
  return true;
}

const ::warploom::physics::PhysicsBody* EditorSession::physics_body_for(
    std::uint64_t object_id) const {
  const auto it = physics_bodies_.find(object_id);
  if (it == physics_bodies_.end()) return nullptr;
  return &physics_.body(it->second);
}

void EditorSession::tick_timeline_interpolated(std::uint64_t frame, double alpha,
                                              Easing easing) {
  // Clamp rather than trust the host: an out-of-range alpha would sample
  // outside the clip, or rewind into the previous frame.
  if (!(alpha > 0.0)) {
    tick_timeline(frame);
    return;
  }
  if (alpha >= 1.0) {
    alpha = 0.999999999;
  }
  tick_timeline_at(static_cast<double>(frame) + alpha, easing);
}

void EditorSession::tick_timeline(std::uint64_t frame) {
  tick_timeline_at(static_cast<double>(frame), Easing::Step);
}

void EditorSession::tick_timeline_at(double frame_time, Easing easing) {
  // T5c: driven-channel mask is recomputed every tick, never sticky: a
  // tick that plays nothing (paused gate, clip ended, nothing armed)
  // leaves every flag clear, so the host holds the renderer untouched.
  render_driven_ = 0;
  // 1. Playback first: apply the armed clip's step-hold values (direct
  //    application, bindings model). Auto-stops at clip end.
  const SceneObject* render_settings = find_render_settings();
  const std::uint64_t render_id =
      render_settings != nullptr ? render_settings->id : 0U;
  if (playing_) {
    const TimelineClip* clip = doc_.find_clip(playing_clip_);
    if (clip == nullptr) {
      playing_ = false;  // clip removed while armed
    } else if (frame_time >=
               static_cast<double>(clip->start_frame + clip->length_frames)) {
      playing_ = false;  // deterministic end: the clip's own length
    } else if (frame_time >= static_cast<double>(play_started_)) {
      for (const auto& [tkey, track] : clip->tracks) {
        PropValue value;
        if (clip->evaluate_at(frame_time, track.object_id, track.property,
                              easing, value)) {
          SceneObject* obj = doc_.find(track.object_id);
          if (obj == nullptr) continue;
          std::string base;
          int axis = -1;
          split_axis_property(track.property, base, axis);
          if (axis >= 0) {
            // Axis-suffix track: writes one vec3 component.
            auto it = obj->properties.find(base);
            if (it != obj->properties.end() &&
                it->second.type == PropValue::Type::Vec3) {
              it->second.vec[axis] = value.number;
            }
          } else {
            obj->properties[track.property] = value;
          }
          // T5c host half: a playing clip writing a render channel marks
          // it driven, so the host applies exactly the tracked channels.
          if (render_id != 0U && track.object_id == render_id &&
              is_render_channel(track.property)) {
            if (track.property == "exposure") render_driven_ |= 0x01U;
            if (track.property == "bloom") render_driven_ |= 0x02U;
            if (track.property == "render_mode") render_driven_ |= 0x04U;
            if (track.property == "tone_map") render_driven_ |= 0x08U;
          }
        }
      }
    }
  }
  // 2. Recording second: sample the CURRENT document value into each armed
  //    track in insertion order (T5d; read AFTER playback applied, so
  //    clip->same-track chains reproduce themselves). Per-arm auto-disarm
  //    at clip end; a dead arm (clip/object gone) is dropped, the rest
  //    keep sampling.
  if (!record_arms_.empty()) {
    std::vector<RecordArm> surviving;
    surviving.reserve(record_arms_.size());
    for (const RecordArm& arm : record_arms_) {
      TimelineClip* clip =
          doc_.find_clip(arm.clip_id);  // mutable: samples land here
      SceneObject* obj = doc_.find(arm.object_id);
      if (clip == nullptr || obj == nullptr) continue;  // dead arm: drop
      if (frame_time >=
          static_cast<double>(clip->start_frame + clip->length_frames)) {
        continue;  // past the end: per-arm disarm (T5a contract)
      }
      if (frame_time >= static_cast<double>(clip->start_frame)) {
        std::string base;
        int axis = -1;
        split_axis_property(arm.property, base, axis);
        const auto it = obj->properties.find(base);
        double number = 0.0;
        bool have = false;
        if (it != obj->properties.end()) {
          if (axis >= 0) {
            // Axis-suffix track: one vec3 component as the sample.
            if (it->second.type == PropValue::Type::Vec3) {
              number = it->second.vec[axis];
              have = true;
            }
          } else if (it->second.type == PropValue::Type::Number) {
            number = it->second.number;
            have = true;
          }
        }
        if (have) {
          // Samples land on whole frame offsets by contract, so a fractional
          // sample point floors rather than producing a fractional offset --
          // which the sample ordering and the on-disk format both forbid.
          const std::uint64_t offset = static_cast<std::uint64_t>(frame_time) -
                                       clip->start_frame;
          auto& track = clip->tracks[track_key(arm.object_id, arm.property)];
          track.object_id = arm.object_id;
          track.property = arm.property;
          if (track.samples.empty() ||
              track.samples.back().frame_offset < offset) {
            // Resumed arms append at the next unseen offset (never duplicate
            // a frame); the next_clip_id monotonic contract keeps save/load
            // round-trips byte-stable through re-records.
            track.samples.push_back(
                ClipSample{offset, PropValue::make_number(number)});
          }
        }
      }
      surviving.push_back(arm);
    }
    record_arms_.swap(surviving);
  }
}

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
  // node editor toolbar and document parser validate against. The unbound
  // "script" type rides along (G4 viewport wiring): a saved document may
  // name it, and the toolbar lists it, whether or not a module with the
  // requested name is loaded — an unknown module degrades to zero outputs
  // deterministically, never to a load failure.
  register_builtin_node_types(doc_.node_graph);
  register_script_node_type(doc_.node_graph);
}

::warploom::core::ControlReply EditorSession::on_control(
    const ::warploom::core::ControlCommand& command) {
  ::warploom::core::ControlReply reply;

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
    const ::warploom::core::ControlCommand& command,
    ::warploom::core::ControlReply& reply) {
  using CK = ::warploom::core::ControlCommand::Kind;

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
      // T5c: the reserved object cannot be destroyed while tracks reference
      // it — a dangling render track would silently stop applying, so the
      // deletion is loud instead. (Unreferenced, deletion is allowed; a
      // later clip_record recreates the object with a fresh id.)
      if (const SceneObject* rs = find_render_settings();
          rs != nullptr && rs->id == oid) {
        std::size_t refs = 0;
        for (const TimelineClip& c : doc_.clips) {
          for (const auto& [tkey, track] : c.tracks) {
            if (track.object_id == oid) ++refs;
          }
        }
        if (refs > 0U) {
          reply.ok = false;
          reply.error = "destroy_object: render_settings object " +
                        std::to_string(oid) + " is referenced by " +
                        std::to_string(refs) + " track(s); remove them first";
          return true;
        }
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
      // T5c: the reserved render-channel store is not scene content, so it
      // is not selectable. Loud rather than silent: a client that resolves
      // its dynamic id deserves to be told why nothing happened.
      if (const SceneObject* reserved = find_render_settings();
          reserved != nullptr && reserved->id == oid) {
        reply.ok = false;
        reply.error = "select: object " + std::to_string(oid) +
                      " is the reserved render_settings store";
        return true;
      }
      selected_id_ = oid;
      reply.ok = true;
      reply.detail = (oid == 0U) ? "deselected"
                                 : "selected object " + std::to_string(oid);
      return true;
    }
    // v1.8 timeline clips (G3): document entities — undoable edits.
    case CK::ClipAdd: {
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "clip_add needs \"name\"";
        return true;
      }
      const auto start = command.number_count > 0U
                             ? static_cast<std::uint64_t>(command.numbers[0])
                             : 0U;
      const auto length = command.number_count > 1U
                              ? static_cast<std::uint64_t>(command.numbers[1])
                              : 0U;
      const auto expected_id = doc_.next_clip_id;  // apply claims exactly this
      auto cmd = std::make_unique<AddClipCommand>(command.text, start,
                                                  length);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "clip_add failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "added clip " + std::to_string(expected_id);
      return true;
    }
    case CK::ClipRemove: {
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "clip_remove needs clip";
        return true;
      }
      const auto clip_id =
          static_cast<std::uint64_t>(command.numbers[0]);
      auto cmd = std::make_unique<RemoveClipCommand>(clip_id);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "clip_remove failed: " + error;
        return true;
      }
      // T5d: disarm-by-clip removes the matching arms only; arms on other
      // clips keep sampling.
      record_arms_.erase(
          std::remove_if(record_arms_.begin(), record_arms_.end(),
                         [&](const RecordArm& arm) {
                           return arm.clip_id == clip_id;
                         }),
          record_arms_.end());
      if (playing_ && playing_clip_ == clip_id) {
        playing_ = false;
      }
      reply.ok = true;
      reply.detail = "removed clip " + std::to_string(clip_id);
      return true;
    }
    case CK::ClipMove: {
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "clip_move needs clip";
        return true;
      }
      const auto clip_id =
          static_cast<std::uint64_t>(command.numbers[0]);
      const TimelineClip* clip = doc_.find_clip(clip_id);
      if (clip == nullptr) {
        reply.ok = false;
        reply.error = "clip_move: no clip " + std::to_string(clip_id);
        return true;
      }
      const auto new_start = command.number_count > 1U
                                 ? static_cast<std::uint64_t>(
                                       command.numbers[1])
                                 : clip->start_frame;
      auto cmd = std::make_unique<MoveClipCommand>(clip_id, new_start);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "clip_move failed: " + error;
        return true;
      }
      reply.ok = true;
      reply.detail = "moved clip " + std::to_string(clip_id) + " to frame " +
                     std::to_string(new_start);
      return true;
    }
    case CK::NodeAdd: {
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "add_node needs \"type\"";
        return true;
      }
      // numbers[0..1] = x,y per the documented wire payload (the old code
      // read [1]/[2], so a wire add_node landed at (y, 40) and the viewport
      // toolbar dropped nodes at (mouse_y, 40) instead of the cursor.
      const double x = command.number_count > 0U ? command.numbers[0]
                                                 : 40.0;
      const double y = command.number_count > 1U ? command.numbers[1]
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
    case CK::CopySubgraph: {
      // Strict comma-separated id list (flat wire format has no arrays):
      // digits and commas only, no empties, no whitespace.
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "copy_subgraph needs \"nids\" (e.g. \"1,2,3\")";
        return true;
      }
      std::vector<std::uint64_t> ids;
      std::string token;
      std::string parse_error;
      const auto flush_token = [&] {
        if (token.empty() ||
            token.find_first_not_of("0123456789") != std::string::npos) {
          parse_error = "copy_subgraph: bad id \"" + token + "\"";
          return false;
        }
        std::uint64_t v = 0;
        for (const char c : token) {
          const auto d = static_cast<std::uint64_t>(c - '0');
          if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10U) {
            parse_error = "copy_subgraph: id out of range";
            return false;
          }
          v = v * 10U + d;
        }
        ids.push_back(v);
        token.clear();
        return true;
      };
      bool parse_ok = true;
      for (const char c : command.text) {
        if (c == ',') {
          parse_ok = flush_token();
          if (!parse_ok) break;
        } else {
          token.push_back(c);
        }
      }
      if (parse_ok) parse_ok = flush_token();
      if (!parse_ok) {
        reply.ok = false;
        reply.error = parse_error;
        return true;
      }
      for (const auto id : ids) {
        if (doc_.node_graph.find(id) == nullptr) {
          reply.ok = false;
          reply.error =
              "copy_subgraph: no node " + std::to_string(id);
          return true;
        }
      }
      // Layout-aware copy: pastes restore canvas positions (cascade offset
      // applied at paste time by PasteSubgraphCommand).
      reply.ok = true;
      reply.detail = doc_.node_graph.copy_subgraph(ids, doc_.node_layout);
      return true;
    }
    case CK::PasteSubgraph: {
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "paste_subgraph needs \"fragment\"";
        return true;
      }
      // Cascade so repeated pastes do not stack exactly on the originals.
      // Baked into the command at construction, so undo+redo is stable
      // and replays reproduce the same offsets through the same counter.
      const double off =
          32.0 * static_cast<double>((paste_count_ % 8U) + 1U);
      auto cmd =
          std::make_unique<PasteSubgraphCommand>(command.text, off, off);
      std::string error;
      if (!stack_.execute(std::move(cmd), error)) {
        reply.ok = false;
        reply.error = "paste_subgraph failed: " + error;
        return true;
      }
      ++paste_count_;
      const auto* pasted = static_cast<const PasteSubgraphCommand*>(
          stack_.undo_top());
      std::string detail = "{\"ids\":[";
      if (pasted != nullptr) {
        bool first = true;
        for (const auto id : pasted->new_ids()) {
          if (!first) detail += ",";
          first = false;
          detail += std::to_string(id);
        }
      }
      detail += "]}";
      reply.ok = true;
      reply.detail = detail;
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
      // A wholesale document replace mid-capture is unrepresentable in
      // warploom-replay-v1: the loader replays from the single opening
      // checkpoint plus the command log, and neither the load command nor
      // the loaded bytes would be in the file (docs/replay-format.md used
      // to claim this was "self-consistent by construction" — it was
      // silent divergence). Reject loudly instead; stop capture first.
      if (recorder_.active()) {
        reply.ok = false;
        reply.error =
            "load_document during active capture forks the timeline; "
            "stop capture first";
        return true;
      }
      if (command.text.empty()) {
        reply.ok = false;
        reply.error = "load_document needs \"path\"";
        return true;
      }
      SceneDocument loaded;
      // Registry first: the parser validates node types against the target.
      register_builtin_node_types(loaded.node_graph);
      register_script_node_type(loaded.node_graph);
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
      // G3: armed clip intent does not survive a wholesale document replace.
      record_arms_.clear();
      playing_ = false;
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
        recorder_.embed_checkpoint(doc_, physics_bytes(), frame);
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
      // The scrub_to command replays through the hydrated ring, so the
      // target bytes must travel in the file. scrub_start already embeds
      // them when the warp was prepared that way; a warp to an older
      // (pre-capture) checkpoint would otherwise write a file that fails
      // to load itself. Embed the restored bytes when missing.
      if (recorder_.active() && !recorder_.has_checkpoint(frame)) {
        recorder_.embed_checkpoint(doc_, physics_bytes(), frame);
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
      // Same unrepresentability as LoadDocument above: a nested replay
      // replaces the document and resets the stack with no trace the file
      // format could re-apply (re-applied commands are deliberately NOT
      // re-recorded). Reject loudly instead of writing a file that cannot
      // replay itself.
      if (recorder_.active()) {
        reply.ok = false;
        reply.error =
            "load_replay during active capture forks the timeline; "
            "stop capture first";
        return true;
      }
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
    // v1.8 timeline clips (G3): arm/disarm + query (session-side intent;
    // the EDITS — add/remove/move — are document commands above).
    case CK::ClipRecord: {
      if (command.number_count < 2U || command.text.empty()) {
        reply.ok = false;
        reply.error = "clip_record needs clip, oid, key";
        return true;
      }
      const auto clip_id =
          static_cast<std::uint64_t>(command.numbers[0]);
      auto object_id =
          static_cast<std::uint64_t>(command.numbers[1]);
      const TimelineClip* clip = doc_.find_clip(clip_id);
      if (clip == nullptr) {
        reply.ok = false;
        reply.error = "clip_record: no clip " + std::to_string(clip_id);
        return true;
      }
      // T5c: oid 0 + a bare render channel resolves to the reserved
      // object, creating it on demand (undoable spawn, so replay
      // reproduces the same id from the same checkpoint).
      if (object_id == 0U && is_render_channel(command.text)) {
        std::string error;
        SceneObject* rs = ensure_render_settings(error);
        if (rs == nullptr) {
          reply.ok = false;
          reply.error = "clip_record: " + error;
          return true;
        }
        object_id = rs->id;
      }
      const SceneObject* obj = doc_.find(object_id);
      if (obj == nullptr) {
        reply.ok = false;
        reply.error = "clip_record: no object " + std::to_string(object_id);
        return true;
      }
      std::string base;
      int axis = -1;
      split_axis_property(command.text, base, axis);
      const auto prop_it = obj->properties.find(base);
      if (prop_it == obj->properties.end() ||
          (axis >= 0 &&
           prop_it->second.type != PropValue::Type::Vec3) ||
          (axis < 0 &&
           prop_it->second.type != PropValue::Type::Number)) {
        reply.ok = false;
        reply.error = "clip_record: object " + std::to_string(object_id) +
                      " has no recordable \"" + command.text +
                      "\" (number property or vec3 axis)";
        return true;
      }
      // T5d: two arms on the same track are rejected (two writers to one
      // track is a race expressed as data); re-arming an identical track
      // after a stop succeeds because the stop removed the arm.
      for (const RecordArm& arm : record_arms_) {
        if (arm.clip_id == clip_id && arm.object_id == object_id &&
            arm.property == command.text) {
          reply.ok = false;
          reply.error = "clip_record: track " + std::to_string(object_id) +
                        ":" + command.text + " already armed on clip " +
                        std::to_string(clip_id);
          return true;
        }
      }
      record_arms_.push_back(
          RecordArm{clip_id, object_id, command.text});
      // Existing samples on this track stay (a resumed arm appends at the
      // next unseen offset — deterministic, never duplicated).
      reply.ok = true;
      reply.detail = "recording clip " + std::to_string(clip_id) + " track " +
                     std::to_string(object_id) + ":" + command.text;
      return true;
    }
    case CK::ClipRecordAll: {
      // T5b: one clip+object pair, every recordable property as its own
      // track (Number as-is, Vec3 as three axis tracks), armed through
      // the T5d multi-arm model in deterministic property order (the
      // document's property map is ordered, so replays arm identically).
      if (command.number_count < 2U) {
        reply.ok = false;
        reply.error = "clip_record_all needs clip, oid";
        return true;
      }
      const auto all_clip_id =
          static_cast<std::uint64_t>(command.numbers[0]);
      auto all_object_id =
          static_cast<std::uint64_t>(command.numbers[1]);
      const TimelineClip* all_clip = doc_.find_clip(all_clip_id);
      if (all_clip == nullptr) {
        reply.ok = false;
        reply.error =
            "clip_record_all: no clip " + std::to_string(all_clip_id);
        return true;
      }
      // T5c: oid 0 arms every render channel (creating the object first).
      if (all_object_id == 0U) {
        std::string error;
        SceneObject* rs = ensure_render_settings(error);
        if (rs == nullptr) {
          reply.ok = false;
          reply.error = "clip_record_all: " + error;
          return true;
        }
        all_object_id = rs->id;
      }
      const SceneObject* all_obj = doc_.find(all_object_id);
      if (all_obj == nullptr) {
        reply.ok = false;
        reply.error = "clip_record_all: no object " +
                      std::to_string(all_object_id);
        return true;
      }
      std::vector<std::string> keys;
      for (const auto& [pname, pvalue] : all_obj->properties) {
        if (pvalue.type == PropValue::Type::Number) {
          keys.push_back(pname);
        } else if (pvalue.type == PropValue::Type::Vec3) {
          keys.push_back(pname + ".x");
          keys.push_back(pname + ".y");
          keys.push_back(pname + ".z");
        }
      }
      if (keys.empty()) {
        reply.ok = false;
        reply.error = "clip_record_all: object " +
                      std::to_string(all_object_id) + " has no recordable";
        return true;
      }
      // Atomic: any already-armed track rejects the whole command so a
      // partial arm set never lands (the caller stops the old arms first).
      for (const std::string& key : keys) {
        for (const RecordArm& arm : record_arms_) {
          if (arm.clip_id == all_clip_id && arm.object_id == all_object_id &&
              arm.property == key) {
            reply.ok = false;
            reply.error = "clip_record_all: track " +
                          std::to_string(all_object_id) + ":" + key +
                          " already armed on clip " + std::to_string(all_clip_id);
            return true;
          }
        }
      }
      for (const std::string& key : keys) {
        record_arms_.push_back(RecordArm{all_clip_id, all_object_id, key});
      }
      reply.ok = true;
      reply.detail = "recording clip " + std::to_string(all_clip_id) +
                     " object " + std::to_string(all_object_id) + " (" +
                     std::to_string(keys.size()) + " tracks)";
      return true;
    }
    case CK::ClipRecordStop: {
      const bool was = !record_arms_.empty();
      record_arms_.clear();
      reply.ok = true;
      reply.detail = was ? "record disarmed" : "record was not armed";
      return true;
    }
    case CK::ClipPlay: {
      if (command.number_count < 1U) {
        reply.ok = false;
        reply.error = "clip_play needs clip";
        return true;
      }
      const auto clip_id =
          static_cast<std::uint64_t>(command.numbers[0]);
      const TimelineClip* clip = doc_.find_clip(clip_id);
      if (clip == nullptr) {
        reply.ok = false;
        reply.error = "clip_play: no clip " + std::to_string(clip_id);
        return true;
      }
      if (clip->tracks.empty()) {
        reply.ok = false;
        reply.error = "clip_play: clip " + std::to_string(clip_id) +
                      " has no tracks";
        return true;
      }
      const auto from = command.number_count > 1U
                            ? static_cast<std::uint64_t>(command.numbers[1])
                            : clip->start_frame;
      playing_ = true;
      playing_clip_ = clip_id;
      play_started_ = from;
      reply.ok = true;
      reply.detail = "playing clip " + std::to_string(clip_id) + " from " +
                     std::to_string(from);
      return true;
    }
    case CK::ClipStop: {
      const bool was = playing_;
      playing_ = false;
      reply.ok = true;
      reply.detail = was ? "playback stopped" : "playback was not armed";
      return true;
    }
    case CK::ClipsInfo: {
      std::string out = "[";
      bool first_clip = true;
      for (const auto& clip : doc_.clips) {
        if (!first_clip) out += ",";
        first_clip = false;
        out += "{\"id\":" + std::to_string(clip.id);
        out += ",\"name\":" + quote(clip.name);
        out += ",\"start_frame\":" + std::to_string(clip.start_frame);
        out += ",\"length_frames\":" + std::to_string(clip.length_frames);
        out += ",\"tracks\":{";
        bool first_track = true;
        for (const auto& [tkey, track] : clip.tracks) {  // map: sorted
          if (!first_track) out += ",";
          first_track = false;
          out += quote(tkey) + ":{\"object_id\":" +
                 std::to_string(track.object_id) + ",\"property\":" +
                 quote(track.property) + ",\"samples\":[";
          if (!track.samples.empty()) {
            // Head/tail values for display (std::to_string's 6 fixed
            // decimals; exact values live in the document itself).
            out += std::to_string(track.samples.front().value.number);
            out += ',';
            out += std::to_string(track.samples.back().value.number);
          }
          out += "],\"count\":" + std::to_string(track.samples.size()) +
                 "}";
        }  // tracks
        // T5d: armed record tracks on this clip, sorted by (object id,
        // property) for a deterministic report (arms sample in insertion
        // order; the report is sorted for output). Additive: existing
        // readers (live_proof.py) parse id/name/tracks and ignore this.
        std::vector<std::string> armed;
        for (const RecordArm& arm : record_arms_) {
          if (arm.clip_id == clip.id) {
            armed.push_back(std::to_string(arm.object_id) + ":" + arm.property);
          }
        }
        std::sort(armed.begin(), armed.end());
        out += ",\"armed\":[";
        for (std::size_t i = 0; i < armed.size(); ++i) {
          if (i > 0U) out += ",";
          out += quote(armed[i]);
        }
        out += "]}}";
      }  // clips
      out += "]";
      reply.ok = true;
      reply.detail = std::move(out);
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
    const ::warploom::core::ControlCommand& command,
    ::warploom::core::ControlReply& reply) {
  using CK = ::warploom::core::ControlCommand::Kind;

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
    case CK::GetRenderConfig: {
      // T5c: the session owns the values (they live on the reserved
      // render_settings object), so both hosts answer identically. "driven"
      // names the channels a playing clip wrote on the last tick, which is
      // what tells a host "track drove 1.0" from "nobody is driving".
      const RenderConfig cfg = render_config();
      std::string out = "{\"exposure\":";
      append_number(out, cfg.exposure);
      out += ",\"bloom\":";
      append_number(out, cfg.bloom);
      out += ",\"render_mode\":";
      append_number(out, cfg.render_mode);
      out += ",\"tone_map\":";
      append_number(out, cfg.tone_map);
      out += ",\"driven\":[";
      const bool flags[4] = {cfg.exposure_driven, cfg.bloom_driven,
                             cfg.render_mode_driven, cfg.tone_map_driven};
      const char* names[4] = {"exposure", "bloom", "render_mode", "tone_map"};
      bool first = true;
      for (std::size_t i = 0; i < 4U; ++i) {
        if (!flags[i]) continue;
        if (!first) out += ",";
        first = false;
        out += quote(names[i]);
      }
      out += "]}";
      reply.ok = true;
      reply.detail = std::move(out);
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

::warploom::core::ControlReply EditorSession::handle_session(
    const ::warploom::core::ControlCommand& command) {
  using CK = ::warploom::core::ControlCommand::Kind;
  ::warploom::core::ControlReply reply;
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
    // v1.9 host-owned visuals: acknowledged here so visual-less hosts stay
    // protocol-complete; the real effect lives in the viewport host.
    case CK::SetRenderMode:
    case CK::GetRenderMode:
      reply.ok = true;
      reply.detail = "render mode acknowledged (host executes)";
      break;
    case CK::SetExposure:
      reply.ok = true;
      reply.detail = "exposure acknowledged (host executes)";
      break;
    case CK::SetBloom:
    case CK::GetBloom:
      reply.ok = true;
      reply.detail = "bloom acknowledged (host executes)";
      break;
    case CK::SetRtReflections:
    case CK::GetRtReflections:
    case CK::SetRtAo:
    case CK::GetRtAo:
      reply.ok = true;
      reply.detail = "rt sub-toggle acknowledged (host executes)";
      break;
    case CK::SetPtSampleTarget:
    case CK::GetPtSamples:
    case CK::GetPtMean:
      reply.ok = true;
      reply.detail = "pt stepped accumulation acknowledged (host executes)";
      break;
    case CK::SetAdaptive:
    case CK::GetAdaptive:
      reply.ok = true;
      reply.detail = "pt adaptive sampling acknowledged (host executes)";
      break;
    case CK::SetDenoise:
    case CK::GetDenoise:
    case CK::GetDenoisedMean:
      reply.ok = true;
      reply.detail = "pt denoiser acknowledged (host executes)";
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
  return scrubber_.capture(frame, doc_, physics_bytes(), error);
}

bool EditorSession::scrub_to(std::uint64_t frame, std::string& error) {
  // T4a: restore physics FIRST into temporaries, so a torn physics payload
  // fails with doc_, physics_, and the map all untouched (same atomicity
  // the document restore already guarantees).
  ::warploom::physics::PhysicsWorld restored_physics;
  std::map<std::uint64_t, std::uint32_t> restored_map;
  std::string physics_json;
  if (!scrubber_.physics_for(frame, physics_json)) {
    error = "scrub: no checkpoint at frame " + std::to_string(frame);
    return false;
  }
  if (!physics_restore_json(restored_physics, restored_map, physics_json,
                             error)) {
    error = "scrub: checkpoint physics failed to restore: " + error;
    return false;
  }
  SceneDocument restored;
  if (!scrubber_.restore(frame, restored, error)) {
    return false;
  }
  doc_ = std::move(restored);
  physics_ = std::move(restored_physics);
  physics_bodies_ = std::move(restored_map);
  stack_ = CommandStack(doc_);  // undo cannot cross a time warp
  // Selection survives only when the restored document still has the object.
  if (selected_id_ != 0U && doc_.find(selected_id_) == nullptr) {
    selected_id_ = 0;
  }
  // G3: armed clip intent was aimed at the pre-warp timeline context; the
  // warp may also have replaced the clip definitions themselves.
  record_arms_.clear();
  playing_ = false;
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

//! Parses an optional scalar double field ("key":number). Missing or
//! malformed leaves `out` untouched and returns false — the caller decides
//! whether absence is an error (new field) or a default (old file).
[[nodiscard]] bool replay_find_scalar(const std::string& line, const char* key,
                                      double& out) {
  const std::string needle = "\"" + std::string(key) + "\":";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  try {
    std::size_t consumed = 0;
    out = std::stod(line.substr(key_pos + needle.size()), &consumed);
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
  // T4a: the fixed step the live run integrated with. Absent (old file) or
  // non-positive means "unknown — do not re-simulate physics" (status quo:
  // physics stays untouched and the closing check covers the document only).
  double replay_fixed_dt = 0.0;
  (void)replay_find_scalar(lines.front(), "fixed_dt", replay_fixed_dt);
  if (!(replay_fixed_dt > 0.0)) replay_fixed_dt = 0.0;
  // Truncation guard: the last line must be the end record.
  if (!replay_find_string(lines.back(), "record", record) ||
      record != "end") {
    error = "load_replay: truncated file (missing end record)";
    return false;
  }

  // Pass 1: collect command records and checkpoint pairs.
  struct LoggedCommand {
    std::uint64_t seq{0};
    ::warploom::core::ControlCommand command{};
  };
  std::vector<LoggedCommand> logged;
  struct HydratedCheckpoint {
    std::uint64_t frame{0};
    std::uint64_t hash{0};
    std::string json{};
    std::uint64_t physics_hash{0};
    std::string physics_json{};
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
      for (const auto& kn : ::warploom::core::ControlCommand::kind_names()) {
        if (name == kn.name) {
          entry.command.kind = kn.kind;
          break;
        }
      }
      if (entry.command.kind == ::warploom::core::ControlCommand::Kind::Unknown) {
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
      std::string p_hash_text;
      // p_hash is optional: files predating T4a checkpoints carry the
      // document line only, and hydrate with empty physics (defined to
      // restore an empty world — never stale state).
      const bool has_physics = replay_find_string(line, "p_hash",
                                                  p_hash_text);
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
      if (has_physics) {
        if (i + 1 >= lines.size()) {
          error = "load_replay: ckpt at line " + std::to_string(i + 1) +
                  " has no physics line";
          return false;
        }
        cp.physics_json = lines[++i];
        try {
          cp.physics_hash = std::stoull(p_hash_text);
        } catch (const std::exception&) {
          error = "load_replay: malformed p_hash at line " +
                  std::to_string(i);
          return false;
        }
        if (fnv1a64(cp.physics_json.data(), cp.physics_json.size()) !=
            cp.physics_hash) {
          error = "load_replay: checkpoint physics hash mismatch at line " +
                  std::to_string(i + 1);
          return false;
        }
      }
      hydrated.push_back(std::move(cp));
    }
    // "header"/"end" records: already validated.
  }

  // Hydrate the scrubber: the loaded session's timeline REPLACES the ring.
  scrubber_.clear();
  for (const auto& cp : hydrated) {
    scrubber_.insert(cp.frame, cp.hash, cp.json, cp.physics_hash,
                      cp.physics_json);
  }

  // Re-apply needs a defined start state: restore the EARLIEST checkpoint
  // (the capture-start snapshot; `start` embeds at the start frame before
  // any command exists, so earliest == opening) before replaying the log.
  // Restored from the hydrated BYTES, not the ring: opening and closing
  // can share a frame (a capture with no Step between start and stop),
  // and same-frame ring insertion replaces newest-wins, so a ring lookup
  // by frame can return the closing bytes and the log then re-applies on
  // top of them (duplicated objects + a closing-hash mismatch).
  if (!hydrated.empty()) {
    std::string restore_error;
    // T4a: the world and map restore into temporaries first (same
    // atomicity as the document: nothing commits on a torn payload).
    ::warploom::physics::PhysicsWorld opening_physics;
    std::map<std::uint64_t, std::uint32_t> opening_map;
    if (!physics_restore_json(opening_physics, opening_map,
                               hydrated.front().physics_json,
                               restore_error)) {
      error = "load_replay: opening physics failed to restore: " +
              restore_error;
      return false;
    }
    if (!scrubber_.restore_bytes(hydrated.front().json,
                                  hydrated.front().hash,
                                  hydrated.front().physics_json,
                                  hydrated.front().physics_hash, doc_,
                                  restore_error)) {
      error = "load_replay: opening checkpoint failed to restore: " +
              restore_error;
      return false;
    }
    physics_ = std::move(opening_physics);
    physics_bodies_ = std::move(opening_map);
    stack_ = CommandStack(doc_);
    selected_id_ = 0;
  }

  // Re-apply the command log in seq order through the normal mutation
  // authority. Re-applied commands are NOT re-recorded (spec): the guard
  // flag suppresses the on_control hook. A failed command aborts the load;
  // state is whatever the prefix produced and the hydrated checkpoints
  // remain available for scrub_to recovery.
  using CK = ::warploom::core::ControlCommand::Kind;
  // G3: logical sim frame driving the re-simulated session ticks (starts at
  // the opening checkpoint's frame, advances one per tick).
  std::uint64_t frame_source_frame =
      hydrated.empty() ? 0U : hydrated.front().frame;

  replaying_ = true;
  // T4a: pause gates physics re-simulation exactly like the live tick
  // (paused ticks advance neither). Files predating fixed_dt re-simulate
  // nothing (replay_fixed_dt == 0): physics stays as the opening
  // checkpoint restored it — the pre-T4a behaviour, bit for bit.
  bool replay_paused = false;
  for (const auto& entry : logged) {
    // G3: ONE full session tick per recorded step tick — the sim loop is
    // the re-applier (headless sessions have no free-running clock). This
    // is what makes timeline sample capture (armed clip_record armed via
    // the log) re-execute deterministically under load_replay; the file
    // format itself is unchanged (v1, readers never re-simulate).
    const auto ticks_to_run = entry.command.kind == CK::Step &&
                                      entry.command.number_count > 0U
                                  ? static_cast<std::uint64_t>(
                                        entry.command.numbers[0])
                                  : 1U;
    // T4b: the recorded fraction lands the span's LAST tick inside its
    // frame (smooth playback position). Absent on old Steps = 0 = the
    // step-hold path, bit for bit.
    const double frac =
        entry.command.kind == CK::Step && entry.command.number_count > 1U
            ? entry.command.numbers[1]
            : 0.0;
    for (std::uint64_t t = 0; t < ticks_to_run; ++t) {
      std::string sync_error;
      (void)sync_graph(sync_error);
      if (!replay_paused && replay_fixed_dt > 0.0) {
        (void)step_physics(replay_fixed_dt);
      }
      advance_timeline(frame_source_frame,
                       t + 1U == ticks_to_run ? frac : 0.0);
      frame_source_frame += 1;
    }
    const auto reply = on_control(entry.command);
    if (!reply.ok) {
      replaying_ = false;
      error = "load_replay: command seq " + std::to_string(entry.seq) +
              " (" +
              ::warploom::core::ControlCommand::kind_name(entry.command.kind) +
              ") failed: " + reply.error;
      return false;
    }
    if (entry.command.kind == CK::Pause) replay_paused = true;
    if (entry.command.kind == CK::Resume) replay_paused = false;
  }
  replaying_ = false;
  // End-state verification (W2 contract, docs/replay-format.md "Load
  // semantics"): re-applying the log from the opening checkpoint must
  // reproduce the file's closing checkpoint byte-for-byte. Anything the
  // live run did outside the log (free-run ticks the host never issued
  // as commands, unreplayable state) lands here instead of silently
  // diverging. The closing checkpoint is the LAST checkpoint record in
  // file order: stop() embeds it after every command and every earlier
  // checkpoint (a mid-file ScrubStart can target a frame AHEAD of the
  // final log position, so greatest-frame would pick the wrong record).
  if (!hydrated.empty()) {
    const std::size_t closing = hydrated.size() - 1U;
    const std::string now_json = doc_.to_json();
    const std::uint64_t now_hash = fnv1a64(now_json.data(), now_json.size());
    if (now_hash != hydrated[closing].hash) {
      error = "load_replay: re-applied state does not match the closing "
              "checkpoint at frame " +
              std::to_string(hydrated[closing].frame) + " (replay " +
              std::to_string(now_hash) + " vs file " +
              std::to_string(hydrated[closing].hash) +
              "): the capture contains effects outside the command log";
      return false;
    }
    // T4a: the closing physics must match too — but ONLY when the re-apply
    // could have moved it (new file with fixed_dt). Old files re-simulate
    // nothing, so their closing physics is the opening state by
    // construction... which the closing checkpoint also embeds only when
    // the file predates physics entirely (empty). Compare only when the
    // file both carries physics AND drove re-simulation; anything else is
    // the pre-T4a contract and must keep loading exactly as before.
    if (replay_fixed_dt > 0.0 && !hydrated[closing].physics_json.empty()) {
      const std::string now_physics = physics_bytes();
      const std::uint64_t now_phash =
          fnv1a64(now_physics.data(), now_physics.size());
      if (now_phash != hydrated[closing].physics_hash) {
        error = "load_replay: re-applied physics does not match the "
                "closing checkpoint at frame " +
                std::to_string(hydrated[closing].frame) +
                ": the capture contains simulation effects outside the "
                "command log";
        return false;
      }
    }
  }
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
  // C3: physics state travels with the snapshot, so a replay that resumes
  // from one continues the same simulation instead of restarting it. Emitted
  // in object-id order (the map's own order), never spawn order, so the bytes
  // are reproducible.
  out += ",\"physics\":{";
  out += "\"gravity\":";
  append_number(out, static_cast<double>(physics_.gravity()));
  out += ",\"bodies\":[";
  bool first_body = true;
  for (const auto& [object_id, body_id] : physics_bodies_) {
    if (!first_body) out += ",";
    first_body = false;
    const auto& body = physics_.body(body_id);
    out += "{\"oid\":" + std::to_string(object_id);
    out += ",\"pos\":[";
    append_float(out, body.position[0]);
    out += ",";
    append_float(out, body.position[1]);
    out += ",";
    append_float(out, body.position[2]);
    out += "]";
    out += ",\"vel\":[";
    append_float(out, body.velocity[0]);
    out += ",";
    append_float(out, body.velocity[1]);
    out += ",";
    append_float(out, body.velocity[2]);
    out += "]";
    out += ",\"radius\":";
    append_float(out, body.radius);
    out += ",\"inv_mass\":";
    append_float(out, body.inverse_mass);
    out += ",\"restitution\":";
    append_float(out, body.restitution);
    out += "}";
  }
  out += "]}";
  out += "}";
  return out;
}

}  // namespace warploom::editor
