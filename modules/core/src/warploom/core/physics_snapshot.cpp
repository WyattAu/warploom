//! @file physics_snapshot.cpp
//! @brief T4a physics state codec (see the header for the format contract).

#include "warploom/core/physics_snapshot.hpp"

#include <charconv>
#include <cstring>
#include <system_error>
#include <vector>

namespace warploom::editor {

namespace {

//! Shortest round-trip float print (same discipline as snapshot_json:
//! exact float32 recovery, minimal bytes, no double widening).
void append_float(std::string& out, float value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc{}) {
    out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
  } else {
    out += "0";
  }
}

void append_float3(std::string& out, const float v[3]) {
  out += "[";
  append_float(out, v[0]);
  out += ",";
  append_float(out, v[1]);
  out += ",";
  append_float(out, v[2]);
  out += "]";
}

void append_float4(std::string& out, const float v[4]) {
  out += "[";
  append_float(out, v[0]);
  out += ",";
  append_float(out, v[1]);
  out += ",";
  append_float(out, v[2]);
  out += ",";
  append_float(out, v[3]);
  out += "]";
}

// --- Strict forward parser: the writer emits fixed key order, so the ---
// --- reader requires it. Only this codec writes the format.          ---

struct Cursor {
  const char* ptr{nullptr};
  const char* end{nullptr};
  std::string error{};
  [[nodiscard]] bool fail(const char* message) {
    error = message;
    return false;
  }
  [[nodiscard]] bool at_end() const noexcept { return ptr >= end; }
  [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : *ptr; }
  void skip() noexcept {
    if (!at_end()) ++ptr;
  }
};

[[nodiscard]] bool expect_char(Cursor& cur, char want) {
  if (cur.peek() != want) return cur.fail("physics snapshot: syntax error");
  cur.skip();
  return true;
}

[[nodiscard]] bool expect_literal(Cursor& cur, const char* text) {
  const std::size_t len = std::strlen(text);
  if (static_cast<std::size_t>(cur.end - cur.ptr) < len ||
      std::memcmp(cur.ptr, text, len) != 0) {
    return cur.fail("physics snapshot: expected key");
  }
  cur.ptr += len;
  return true;
}

[[nodiscard]] bool parse_uint(Cursor& cur, std::uint64_t& out) {
  if (cur.at_end() || *cur.ptr == '-' ||
      (*cur.ptr < '0' || *cur.ptr > '9')) {
    return cur.fail("physics snapshot: expected unsigned integer");
  }
  std::uint64_t value = 0;
  while (!cur.at_end() && *cur.ptr >= '0' && *cur.ptr <= '9') {
    const std::uint64_t digit = static_cast<std::uint64_t>(*cur.ptr - '0');
    if (value > (UINT64_MAX - digit) / 10U) {
      return cur.fail("physics snapshot: integer overflow");
    }
    value = value * 10U + digit;
    cur.skip();
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_int(Cursor& cur, std::int64_t& out) {
  bool negative = false;
  if (cur.peek() == '-') {
    negative = true;
    cur.skip();
  }
  std::uint64_t magnitude = 0;
  if (!parse_uint(cur, magnitude)) return false;
  if (!negative) {
    if (magnitude > static_cast<std::uint64_t>(INT64_MAX)) {
      return cur.fail("physics snapshot: integer overflow");
    }
    out = static_cast<std::int64_t>(magnitude);
  } else {
    if (magnitude > static_cast<std::uint64_t>(INT64_MAX) + 1U) {
      return cur.fail("physics snapshot: integer overflow");
    }
    out = magnitude == static_cast<std::uint64_t>(INT64_MAX) + 1U
              ? INT64_MIN
              : -static_cast<std::int64_t>(magnitude);
  }
  return true;
}

[[nodiscard]] bool parse_float(Cursor& cur, float& out) {
  // Float lexical form: [-]digits[.digits][e[+-]digits]. from_chars is
  // exact: the shortest round-trip text recovers the original float.
  const char* begin = cur.ptr;
  if (cur.peek() == '-') cur.skip();
  bool any = false;
  while (!cur.at_end() &&
         ((*cur.ptr >= '0' && *cur.ptr <= '9') || *cur.ptr == '.' ||
          *cur.ptr == 'e' || *cur.ptr == 'E' || *cur.ptr == '+' ||
          *cur.ptr == '-')) {
    // A sign is only legal right after e/E; to_chars never emits one
    // elsewhere, and the strict shape check below rejects the rest.
    any = true;
    cur.skip();
  }
  if (!any) return cur.fail("physics snapshot: expected number");
  const auto result =
      std::from_chars(begin, cur.ptr, out);
  if (result.ec != std::errc{} || result.ptr != cur.ptr) {
    return cur.fail("physics snapshot: malformed float");
  }
  return true;
}

[[nodiscard]] bool parse_float3(Cursor& cur, float out[3]) {
  return expect_char(cur, '[') && parse_float(cur, out[0]) &&
         expect_char(cur, ',') && parse_float(cur, out[1]) &&
         expect_char(cur, ',') && parse_float(cur, out[2]) &&
         expect_char(cur, ']');
}

[[nodiscard]] bool parse_float4(Cursor& cur, float out[4]) {
  return expect_char(cur, '[') && parse_float(cur, out[0]) &&
         expect_char(cur, ',') && parse_float(cur, out[1]) &&
         expect_char(cur, ',') && parse_float(cur, out[2]) &&
         expect_char(cur, ',') && parse_float(cur, out[3]) &&
         expect_char(cur, ']');
}

struct ParsedBody {
  std::uint32_t bid{0};
  std::int64_t oid{-1};
  ::warploom::physics::PhysicsBody body{};
};

struct ParsedSnapshot {
  float gravity{-9.81F};
  std::uint32_t substeps{1};
  std::uint32_t iterations{1};
  float compliance{0.0F};
  float rest_thresh{0.0F};
  float sleep_thresh{0.0F};
  bool vfp{false};
  std::vector<ParsedBody> bodies{};
  std::vector<::warploom::physics::PhysicsWorld::DistanceJoint> joints{};
};

[[nodiscard]] bool parse_body(Cursor& cur, ParsedBody& out) {
  std::uint64_t bid = 0;
  std::int64_t oid = -1;
  std::uint64_t shape = 0;
  if (!expect_char(cur, '{')) return false;
  if (!expect_literal(cur, "\"bid\":")) return false;
  if (!parse_uint(cur, bid) || bid > UINT32_MAX) {
    return cur.fail("physics snapshot: bad bid");
  }
  if (!expect_literal(cur, ",\"oid\":")) return false;
  if (!parse_int(cur, oid)) return false;
  if (!expect_literal(cur, ",\"pos\":")) return false;
  if (!parse_float3(cur, out.body.position)) return false;
  if (!expect_literal(cur, ",\"vel\":")) return false;
  if (!parse_float3(cur, out.body.velocity)) return false;
  if (!expect_literal(cur, ",\"ori\":")) return false;
  if (!parse_float4(cur, out.body.orientation)) return false;
  if (!expect_literal(cur, ",\"angvel\":")) return false;
  if (!parse_float3(cur, out.body.angular_velocity)) return false;
  if (!expect_literal(cur, ",\"radius\":")) return false;
  if (!parse_float(cur, out.body.radius)) return false;
  if (!expect_literal(cur, ",\"inv_mass\":")) return false;
  if (!parse_float(cur, out.body.inverse_mass)) return false;
  if (!expect_literal(cur, ",\"restitution\":")) return false;
  if (!parse_float(cur, out.body.restitution)) return false;
  if (!expect_literal(cur, ",\"shape\":")) return false;
  if (!parse_uint(cur, shape) || shape > 1U) {
    return cur.fail("physics snapshot: bad shape");
  }
  if (!expect_literal(cur, ",\"half_height\":")) return false;
  if (!parse_float(cur, out.body.half_height)) return false;
  if (!expect_char(cur, '}')) return false;
  out.bid = static_cast<std::uint32_t>(bid);
  out.oid = oid;
  out.body.shape = shape == 1U
                       ? ::warploom::physics::BodyShape::Capsule
                       : ::warploom::physics::BodyShape::Sphere;
  return true;
}

[[nodiscard]] bool parse_joint(
    Cursor& cur,
    ::warploom::physics::PhysicsWorld::DistanceJoint& out) {
  std::uint64_t a = 0, b = 0;
  if (!expect_char(cur, '{')) return false;
  if (!expect_literal(cur, "\"a\":")) return false;
  if (!parse_uint(cur, a) || a > UINT32_MAX) {
    return cur.fail("physics snapshot: bad joint endpoint");
  }
  if (!expect_literal(cur, ",\"b\":")) return false;
  if (!parse_uint(cur, b) || b > UINT32_MAX) {
    return cur.fail("physics snapshot: bad joint endpoint");
  }
  if (!expect_literal(cur, ",\"rest\":")) return false;
  if (!parse_float(cur, out.rest_length)) return false;
  if (!expect_literal(cur, ",\"compliance\":")) return false;
  if (!parse_float(cur, out.compliance)) return false;
  if (!expect_char(cur, '}')) return false;
  out.a = static_cast<std::uint32_t>(a);
  out.b = static_cast<std::uint32_t>(b);
  return true;
}

[[nodiscard]] bool parse_snapshot(Cursor& cur, ParsedSnapshot& out) {
  std::uint64_t u = 0;
  if (!expect_char(cur, '{')) return false;
  if (!expect_literal(cur, "\"gravity\":")) return false;
  if (!parse_float(cur, out.gravity)) return false;
  if (!expect_literal(cur, ",\"substeps\":")) return false;
  if (!parse_uint(cur, u) || u == 0U || u > UINT32_MAX) {
    return cur.fail("physics snapshot: bad substeps");
  }
  out.substeps = static_cast<std::uint32_t>(u);
  if (!expect_literal(cur, ",\"iterations\":")) return false;
  if (!parse_uint(cur, u) || u == 0U || u > UINT32_MAX) {
    return cur.fail("physics snapshot: bad iterations");
  }
  out.iterations = static_cast<std::uint32_t>(u);
  if (!expect_literal(cur, ",\"compliance\":")) return false;
  if (!parse_float(cur, out.compliance)) return false;
  if (!expect_literal(cur, ",\"rest_thresh\":")) return false;
  if (!parse_float(cur, out.rest_thresh)) return false;
  if (!expect_literal(cur, ",\"sleep_thresh\":")) return false;
  if (!parse_float(cur, out.sleep_thresh)) return false;
  if (!expect_literal(cur, ",\"vfp\":")) return false;
  if (!parse_uint(cur, u) || u > 1U) {
    return cur.fail("physics snapshot: bad vfp flag");
  }
  out.vfp = u == 1U;
  if (!expect_literal(cur, ",\"bodies\":[")) return false;
  if (cur.peek() != ']') {
    for (;;) {
      ParsedBody body;
      if (!parse_body(cur, body)) return false;
      out.bodies.push_back(body);
      if (cur.peek() == ',') {
        cur.skip();
        continue;
      }
      break;
    }
  }
  if (!expect_char(cur, ']')) return false;
  if (!expect_literal(cur, ",\"joints\":[")) return false;
  if (cur.peek() != ']') {
    for (;;) {
      ::warploom::physics::PhysicsWorld::DistanceJoint joint;
      if (!parse_joint(cur, joint)) return false;
      out.joints.push_back(joint);
      if (cur.peek() == ',') {
        cur.skip();
        continue;
      }
      break;
    }
  }
  if (!expect_char(cur, ']')) return false;
  if (!expect_char(cur, '}')) return false;
  return true;
}

}  // namespace

std::string physics_snapshot_json(
    const ::warploom::physics::PhysicsWorld& world,
    const std::map<std::uint64_t, std::uint32_t>& object_bodies) {
  // Reverse map once: body id -> object id (a body appears at most once;
  // first wins on a corrupt duplicate, which restore would reject anyway).
  std::vector<std::int64_t> bid_to_oid(world.body_count(), -1);
  for (const auto& [oid, bid] : object_bodies) {
    if (bid < bid_to_oid.size() && bid_to_oid[bid] < 0) {
      bid_to_oid[bid] = static_cast<std::int64_t>(oid);
    }
  }
  std::string out = "{\"gravity\":";
  append_float(out, world.gravity());
  out += ",\"substeps\":";
  out += std::to_string(world.substeps());
  out += ",\"iterations\":";
  out += std::to_string(world.solver_iterations());
  out += ",\"compliance\":";
  append_float(out, world.contact_compliance());
  out += ",\"rest_thresh\":";
  append_float(out, world.restitution_threshold());
  out += ",\"sleep_thresh\":";
  append_float(out, world.sleep_threshold());
  out += ",\"vfp\":";
  out += world.velocity_from_position() ? "1" : "0";
  out += ",\"bodies\":[";
  for (std::uint32_t bid = 0; bid < world.body_count(); ++bid) {
    if (bid > 0U) out += ",";
    const auto& body = world.body(bid);
    out += "{\"bid\":";
    out += std::to_string(bid);
    out += ",\"oid\":";
    out += std::to_string(bid_to_oid[bid]);
    out += ",\"pos\":";
    append_float3(out, body.position);
    out += ",\"vel\":";
    append_float3(out, body.velocity);
    out += ",\"ori\":";
    append_float4(out, body.orientation);
    out += ",\"angvel\":";
    append_float3(out, body.angular_velocity);
    out += ",\"radius\":";
    append_float(out, body.radius);
    out += ",\"inv_mass\":";
    append_float(out, body.inverse_mass);
    out += ",\"restitution\":";
    append_float(out, body.restitution);
    out += ",\"shape\":";
    out += body.shape == ::warploom::physics::BodyShape::Capsule ? "1" : "0";
    out += ",\"half_height\":";
    append_float(out, body.half_height);
    out += "}";
  }
  out += "],\"joints\":[";
  for (std::size_t j = 0; j < world.joint_count(); ++j) {
    if (j > 0U) out += ",";
    const auto& joint = world.joint(static_cast<std::uint32_t>(j));
    out += "{\"a\":";
    out += std::to_string(joint.a);
    out += ",\"b\":";
    out += std::to_string(joint.b);
    out += ",\"rest\":";
    append_float(out, joint.rest_length);
    out += ",\"compliance\":";
    append_float(out, joint.compliance);
    out += "}";
  }
  out += "]}";
  return out;
}

bool physics_restore_json(
    ::warploom::physics::PhysicsWorld& world,
    std::map<std::uint64_t, std::uint32_t>& object_bodies,
    const std::string& json, std::string& error) {
  // Empty snapshot = no physics state: defined to restore an EMPTY world
  // (deterministic, never stale). Old files predate physics checkpoints.
  if (json.empty()) {
    world.clear_bodies();
    world.clear_joints();
    object_bodies.clear();
    return true;
  }
  Cursor cur{json.data(), json.data() + json.size(), {}};
  ParsedSnapshot parsed;
  if (!parse_snapshot(cur, parsed)) {
    error = cur.error.empty() ? "physics snapshot: parse failed" : cur.error;
    return false;
  }
  if (!cur.at_end()) {
    error = "physics snapshot: trailing bytes";
    return false;
  }
  // Validate BEFORE touching the outputs (all-or-nothing restore).
  for (std::size_t i = 0; i < parsed.bodies.size(); ++i) {
    if (parsed.bodies[i].bid != i) {
      error = "physics snapshot: body ids not dense from 0";
      return false;
    }
    if (parsed.bodies[i].oid < -1) {
      error = "physics snapshot: bad oid";
      return false;
    }
  }
  const std::uint32_t n =
      static_cast<std::uint32_t>(parsed.bodies.size());
  for (const auto& joint : parsed.joints) {
    if (joint.a >= n || joint.b >= n) {
      error = "physics snapshot: joint endpoint out of range";
      return false;
    }
  }
  // Commit: rebuild the world in id order so restored ids match captured
  // ids, then the caller's map keys stay valid.
  world.clear_bodies();
  world.clear_joints();
  world.set_gravity(parsed.gravity);
  world.set_substeps(parsed.substeps);
  world.set_solver_iterations(parsed.iterations);
  world.set_contact_compliance(parsed.compliance);
  world.set_restitution_threshold(parsed.rest_thresh);
  world.set_sleep_threshold(parsed.sleep_thresh);
  world.set_velocity_from_position(parsed.vfp);
  std::map<std::uint64_t, std::uint32_t> rebuilt;
  for (const auto& entry : parsed.bodies) {
    const std::uint32_t id = world.add_body(entry.body);
    if (id != entry.bid) {
      // Unreachable by construction (clear + sequential add), but a
      // silent id remap would corrupt the session map — fail loudly.
      error = "physics snapshot: body id mismatch on rebuild";
      world.clear_bodies();
      world.clear_joints();
      object_bodies.clear();
      return false;
    }
    if (entry.oid >= 0) {
      rebuilt.emplace(static_cast<std::uint64_t>(entry.oid), id);
    }
  }
  for (const auto& joint : parsed.joints) {
    // Restore re-adds joints to a fresh world; the fresh ids are not
    // remapped (only bodies are), so the return is discarded deliberately.
    (void)world.add_distance_joint(joint.a, joint.b, joint.rest_length,
                                   joint.compliance);
  }
  object_bodies = std::move(rebuilt);
  return true;
}

}  // namespace warploom::editor
