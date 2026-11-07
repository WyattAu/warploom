//! @file control_server.cpp
//! @brief Unix-socket JSONL control server (see the header for protocol v1).
//!
//! Command parsing reuses the flat-JSON field-scan approach from the input
//! script loader: protocol lines are flat one-line objects, so targeted scans
//! stay dependency-free and strict about the accepted shape. Unknown fields
//! are ignored (forward compatibility); unknown commands are protocol errors.

#include "warploom/core/control_server.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "warploom/core/contract.hpp"
#include "warploom/core/numeric_cast.hpp"

namespace warploom::core {

// The single source of truth for command-name -> Kind. Public (see the
// header): tests walk this real table to enforce the protocol invariant.
const std::vector<ControlCommand::KindName>& ControlCommand::kind_names() {
  static const std::vector<ControlCommand::KindName> kTable = {
      // v1.0 session + camera + scene.
      {Kind::Ping, "ping"},           {Kind::Pause, "pause"},
      {Kind::Resume, "resume"},       {Kind::Step, "step"},
      {Kind::SetCamera, "set_camera"}, {Kind::SetSun, "set_sun"},
      {Kind::SpawnCube, "spawn_cube"}, {Kind::Capture, "capture"},
      // v1.1 document queries + edits.
      {Kind::ListObjects, "list_objects"},
      {Kind::GetObject, "get_object"},
      {Kind::SetProperty, "set_property"},
      {Kind::DestroyObject, "destroy_object"},
      {Kind::Undo, "undo"},           {Kind::Redo, "redo"},
      {Kind::Schema, "schema"},       {Kind::Select, "select"},
      // v1.3 node graph (M7).
      {Kind::NodeAdd, "add_node"},
      {Kind::NodeRemove, "remove_node"},
      {Kind::LinkNodes, "link_nodes"},
      {Kind::UnlinkNodes, "unlink_nodes"},
      {Kind::SetNodeParam, "set_node_param"},
      {Kind::SetNodePosition, "set_node_position"},
      {Kind::GetGraph, "get_graph"},
      {Kind::SaveDocument, "save_document"},
      {Kind::LoadDocument, "load_document"},
      // v1.20 scene management (D3).
      {Kind::NewDocument, "new_document"},
      // v1.5 graph->scene bindings (M10).
      {Kind::BindNodeProperty, "bind_node_property"},
      {Kind::UnbindNodeProperty, "unbind_node_property"},
      {Kind::ListBindings, "list_bindings"},
      // v1.6 replay scrubbing (W1).
      {Kind::ScrubStart, "scrub_start"},
      {Kind::ScrubTo, "scrub_to"},
      {Kind::ScrubInfo, "scrub_info"},
      // v1.7 record/replay (W2).
      {Kind::StartCapture, "start_capture"},
      {Kind::StopCapture, "stop_capture"},
      {Kind::CaptureStatus, "capture_status"},
      {Kind::LoadReplay, "load_replay"},
      // v1.8 timeline clips (G3).
      {Kind::ClipAdd, "clip_add"},
      {Kind::ClipRemove, "clip_remove"},
      {Kind::ClipMove, "clip_move"},
      {Kind::ClipRecord, "clip_record"},
      {Kind::ClipRecordStop, "clip_record_stop"},
      {Kind::ClipPlay, "clip_play"},
      {Kind::ClipStop, "clip_stop"},
      {Kind::ClipsInfo, "clips_info"},
      // v1.9 render mode (E1).
      {Kind::SetRenderMode, "set_render_mode"},
      {Kind::GetRenderMode, "get_render_mode"},
      {Kind::SetExposure, "set_exposure"},
      // v1.10 bloom toggle (E2/R4).
      {Kind::SetBloom, "set_bloom"},
      {Kind::GetBloom, "get_bloom"},
      // v1.12 RT sub-toggles (E2/R2).
      {Kind::SetRtReflections, "set_rt_reflections"},
      {Kind::GetRtReflections, "get_rt_reflections"},
      {Kind::SetRtAo, "set_rt_ao"},
      // v1.13 stepped PT (E2/R1).
      {Kind::SetPtSampleTarget, "set_pt_sample_target"},
      {Kind::GetPtSamples, "get_pt_samples"},
      {Kind::GetPtMean, "get_pt_mean"},
      {Kind::GetRtAo, "get_rt_ao"},
      // v1.11 viewport resize: drives the swapchain recreate path on demand.
      {Kind::SetViewportSize, "set_viewport_size"},
      // v1.14 subgraph copy/paste (G2).
      {Kind::CopySubgraph, "copy_subgraph"},
      {Kind::PasteSubgraph, "paste_subgraph"},
      // v1.15 atomic multi-property record (T5b).
      {Kind::ClipRecordAll, "clip_record_all"},
      // v1.16 render-channel tracks (T5c).
      {Kind::GetRenderConfig, "get_render_config"},
      // v1.17 PT adaptive sampling (T2).
      {Kind::SetAdaptive, "set_adaptive"},
      {Kind::GetAdaptive, "get_adaptive"},
      {Kind::SetDenoise, "set_denoise"},
      {Kind::GetDenoise, "get_denoise"},
      {Kind::GetDenoisedMean, "get_denoised_mean"},
      // v1.19 session physics (D1).
      {Kind::SpawnPhysicsBody, "spawn_physics_body"},
      {Kind::GetPhysicsBody, "get_physics_body"},
  };
  return kTable;
}

const char* ControlCommand::kind_name(Kind kind) noexcept {
  for (const auto& entry : kind_names()) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unknown";
}

namespace {

//! Flat-JSON scans (same approach as input_state.cpp's script parser).

//! Escape-aware string field: decodes \" \\ \n \t \r (exactly what the
//! server's own json_escape emits, plus tab/CR tolerance) and rejects any
//! other escape. Needed for payloads that legitimately contain quotes —
//! paste_subgraph's fragment is machine JSON. The plain scanner above is
//! untouched: changing it would alter every existing command's parse.
[[nodiscard]] bool find_escaped_string_field(const std::string& line,
                                             const char* key,
                                             std::string& out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  const std::size_t colon = line.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return false;
  std::size_t pos = line.find('\"', colon + 1);
  if (pos == std::string::npos) return false;
  ++pos;  // past the opening quote
  out.clear();
  while (pos < line.size()) {
    const char c = line[pos];
    if (c == '"') return true;  // closing quote
    if (c == '\\') {
      ++pos;
      if (pos >= line.size()) return false;
      const char e = line[pos];
      if (e == '"' || e == '\\') {
        out.push_back(e);
      } else if (e == 'n') {
        out.push_back('\n');
      } else if (e == 't') {
        out.push_back('\t');
      } else if (e == 'r') {
        out.push_back('\r');
      } else {
        return false;  // unsupported escape (incl. \u): reject loudly
      }
      ++pos;
      continue;
    }
    out.push_back(c);
    ++pos;
  }
  return false;  // unterminated
}

[[nodiscard]] bool find_string_field(const std::string& line, const char* key,
                                     std::string& out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  const std::size_t colon = line.find(':', key_pos + needle.size());
  if (colon == std::size_t(-1)) return false;
  const std::size_t open = line.find('"', colon + 1);
  if (open == std::string::npos) return false;
  const std::size_t close = line.find('"', open + 1);
  if (close == std::string::npos) return false;
  out = line.substr(open + 1, close - open - 1);
  return true;
}

[[nodiscard]] bool find_number_field(const std::string& line, const char* key,
                                     double& out) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_pos = line.find(needle);
  if (key_pos == std::string::npos) return false;
  const std::size_t colon = line.find(':', key_pos + needle.size());
  if (colon == std::size_t(-1)) return false;
  try {
    std::size_t consumed = 0;
    out = std::stod(line.substr(colon + 1), &consumed);
    return consumed > 0U;
  } catch (const std::exception&) {
    return false;
  }
}

[[nodiscard]] bool find_unsigned_field(const std::string& line, const char* key,
                                       std::uint64_t& out) {
  double value = 0.0;
  if (!find_number_field(line, key, value)) return false;
  // Rejects NaN, +-inf, negatives, fractions, and anything >= 2^53 -- the
  // previous hand-rolled form cast before range-checking, so "id": 1e300 hit
  // an undefined float->uint64 conversion.
  return ::warploom::core::checked_double_to_uint64(value, out);
}

//! Parses one protocol line. Returns false with `error` set for malformed
//! content; `Unknown` kind means well-formed but unrecognized command.
[[nodiscard]] bool parse_command(const std::string& line,
                                 ControlCommand& command, std::string& error) {
  std::string cmd;
  if (!find_string_field(line, "cmd", cmd)) {
    error = "missing \"cmd\" string field";
    return false;
  }
  (void)find_unsigned_field(line, "id", command.id);

  // The wire-name table is public (ControlCommand::kind_names) so the
  // wire-parity audit walks the real mapping, not a copy.
  command.kind = ControlCommand::Kind::Unknown;
  for (const auto& entry : ControlCommand::kind_names()) {
    if (cmd == entry.name) {
      command.kind = entry.kind;
      break;
    }
  }
  if (command.kind == ControlCommand::Kind::Unknown) {
    error = "unknown command \"" + cmd + "\"";
    return false;
  }
  command.number_count = 0;

  // Kind-specific numeric payloads.
  double numbers[8] = {};
  std::uint32_t count = 0;
  const auto take = [&](const char* key) {
    double v = 0.0;
    if (find_number_field(line, key, v)) {
      numbers[count++] = v;
    }
  };
  // v1.1 string payloads.
  const auto take_text = [&](const char* key, std::string& out) {
    std::string v;
    if (find_string_field(line, key, v)) {
      out = std::move(v);
    }
  };
  switch (command.kind) {
    case ControlCommand::Kind::Step: {
      std::uint64_t ticks = 1;
      if (!find_unsigned_field(line, "ticks", ticks)) ticks = 1;
      numbers[count++] = static_cast<double>(ticks);
      // T4b: optional sub-frame fraction for the LAST tick of the span
      // (smooth playback position inside the landing frame). Absent (all
      // existing captures) means exactly-on-frame. Validated HERE, at the
      // protocol boundary, not downstream: stod accepts "nan"/"inf",
      // and an alpha >= 1 would sample outside the landing frame. The
      // session clamps defensively too, but a hostile fraction must never
      // enter the command log — recorded garbage would replay as a
      // different take on a build with a different clamp.
      double frac = 0.0;
      if (find_number_field(line, "frac", frac)) {
        if (!(frac >= 0.0 && frac < 1.0)) {
          error = "\"frac\" out of range [0, 1)";
          return false;
        }
        numbers[count++] = frac;
      }
      break;
    }
    case ControlCommand::Kind::SetCamera:
      take("ex"); take("ey"); take("ez");
      take("tx"); take("ty"); take("tz");
      take("fov");
      break;
    case ControlCommand::Kind::SetSun:
      take("x"); take("y"); take("z");
      break;
    case ControlCommand::Kind::SpawnCube:
      take("x"); take("y"); take("z");
      take("size");
      break;
    case ControlCommand::Kind::Capture:
      take("frame");
      break;
    case ControlCommand::Kind::GetObject:
    case ControlCommand::Kind::DestroyObject:
    case ControlCommand::Kind::GetPhysicsBody:
    case ControlCommand::Kind::Select: {
      std::uint64_t oid = 0;
      if (!find_unsigned_field(line, "oid", oid)) {
        error = "missing \"oid\" unsigned field";
        return false;
      }
      numbers[count++] = static_cast<double>(oid);
      break;
    }
    case ControlCommand::Kind::SpawnPhysicsBody: {
      // v1.19: the object must exist (checked at dispatch); only the
      // dynamic parameters ride the wire, each defaulted like NodeAdd's x/y.
      std::uint64_t oid = 0;
      if (!find_unsigned_field(line, "oid", oid)) {
        error = "missing \"oid\" unsigned field";
        return false;
      }
      numbers[count++] = static_cast<double>(oid);
      double radius = 0.5;
      (void)find_number_field(line, "radius", radius);
      numbers[count++] = radius;
      double inverse_mass = 1.0;
      (void)find_number_field(line, "inverse_mass", inverse_mass);
      numbers[count++] = inverse_mass;
      double restitution = 0.4;
      (void)find_number_field(line, "restitution", restitution);
      numbers[count++] = restitution;
      double vx = 0.0, vy = 0.0, vz = 0.0;
      (void)find_number_field(line, "vx", vx);
      (void)find_number_field(line, "vy", vy);
      (void)find_number_field(line, "vz", vz);
      numbers[count++] = vx;
      numbers[count++] = vy;
      numbers[count++] = vz;
      break;
    }
    case ControlCommand::Kind::SetProperty:
      take_text("object", command.text);
      take_text("key", command.text2);
      take_text("value", command.text3);
      take("x"); take("y"); take("z");
      break;
    case ControlCommand::Kind::NodeAdd: {
      take_text("type", command.text);
      double x = 40.0;
      double y = 40.0;
      (void)find_number_field(line, "x", x);
      (void)find_number_field(line, "y", y);
      numbers[count++] = x;
      numbers[count++] = y;
      break;
    }
    case ControlCommand::Kind::NodeRemove:
    case ControlCommand::Kind::SetNodeParam:
    case ControlCommand::Kind::SetNodePosition: {
      std::uint64_t nid = 0;
      if (!find_unsigned_field(line, "nid", nid)) {
        error = "missing \"nid\" unsigned field";
        return false;
      }
      numbers[count++] = static_cast<double>(nid);
      if (command.kind == ControlCommand::Kind::SetNodeParam) {
        take_text("key", command.text);
        take_text("value", command.text2);
        take("x");
      } else if (command.kind == ControlCommand::Kind::SetNodePosition) {
        take("x");
        take("y");
      }
      break;
    }
    case ControlCommand::Kind::LinkNodes:
    case ControlCommand::Kind::UnlinkNodes: {
      std::uint64_t from = 0;
      std::uint64_t to = 0;
      const bool need_from =
          command.kind == ControlCommand::Kind::LinkNodes;
      if (need_from && !find_unsigned_field(line, "from", from)) {
        error = "missing \"from\" unsigned field";
        return false;
      }
      if (!find_unsigned_field(line, "to", to) &&
          !find_unsigned_field(line, "nid", to)) {
        error = "missing \"to\" unsigned field";
        return false;
      }
      if (need_from) numbers[count++] = static_cast<double>(from);
      numbers[count++] = static_cast<double>(to);
      take_text("out", command.text);
      take_text("in", command.text2);
      break;
    }
    case ControlCommand::Kind::GetGraph:
    case ControlCommand::Kind::ListBindings:
    case ControlCommand::Kind::ClipRecordStop:
    case ControlCommand::Kind::ClipStop:
    case ControlCommand::Kind::ClipsInfo:
      break;
    case ControlCommand::Kind::ClipAdd: {
      take_text("name", command.text);
      take("frame");  // start (optional, default 0)
      take("length");  // frames (optional, default 0)
      break;
    }
    case ControlCommand::Kind::ClipRemove:
    case ControlCommand::Kind::ClipRecord:
    case ControlCommand::Kind::ClipRecordAll:
    case ControlCommand::Kind::ClipPlay: {
      std::uint64_t clip = 0;
      if (!find_unsigned_field(line, "clip", clip)) {
        error = "missing \"clip\" unsigned field";
        return false;
      }
      numbers[count++] = static_cast<double>(clip);
      if (command.kind == ControlCommand::Kind::ClipRecordAll) {
        std::uint64_t oid = 0;
        if (!find_unsigned_field(line, "oid", oid)) {
          error = "clip_record_all needs \"oid\"";
          return false;
        }
        numbers[count++] = static_cast<double>(oid);
      } else if (command.kind == ControlCommand::Kind::ClipRecord) {
        std::uint64_t oid = 0;
        if (!find_unsigned_field(line, "oid", oid)) {
          error = "clip_record needs \"oid\"";
          return false;
        }
        numbers[count++] = static_cast<double>(oid);
        take_text("key", command.text);
      } else if (command.kind == ControlCommand::Kind::ClipPlay) {
        take("frame");  // playback start (optional; default = clip start)
      }
      break;
    }
    case ControlCommand::Kind::ClipMove: {
      std::uint64_t clip = 0;
      if (!find_unsigned_field(line, "clip", clip)) {
        error = "missing \"clip\" unsigned field";
        return false;
      }
      numbers[count++] = static_cast<double>(clip);
      take("frame");  // new start; session keeps current when absent
      break;
    }
    case ControlCommand::Kind::BindNodeProperty: {
      std::uint64_t nid = 0;
      std::uint64_t oid = 0;
      if (!find_unsigned_field(line, "nid", nid) ||
          !find_unsigned_field(line, "oid", oid)) {
        error = "bind_node_property needs \"nid\" and \"oid\"";
        return false;
      }
      numbers[count++] = static_cast<double>(nid);
      numbers[count++] = static_cast<double>(oid);
      take_text("out", command.text);
      take_text("property", command.text2);
      break;
    }
    case ControlCommand::Kind::UnbindNodeProperty: {
      std::uint64_t oid = 0;
      if (!find_unsigned_field(line, "oid", oid)) {
        error = "unbind_node_property needs \"oid\"";
        return false;
      }
      numbers[count++] = static_cast<double>(oid);
      take_text("property", command.text);
      break;
    }
    case ControlCommand::Kind::ScrubStart:
    case ControlCommand::Kind::ScrubTo:
    case ControlCommand::Kind::StartCapture: {
      // v1.6/v1.7: scrub_start and start_capture default to frame 0 when
      // "frame" is absent; scrub_to requires it.
      std::uint64_t frame = 0;
      const bool have_frame = find_unsigned_field(line, "frame", frame);
      if (command.kind == ControlCommand::Kind::ScrubTo && !have_frame) {
        error = "missing \"frame\" unsigned field";
        return false;
      }
      numbers[count++] = static_cast<double>(frame);
      break;
    }
    case ControlCommand::Kind::StopCapture:
    case ControlCommand::Kind::LoadReplay:
      take_text("path", command.text);
      break;
    case ControlCommand::Kind::SaveDocument:
    case ControlCommand::Kind::LoadDocument:
      take_text("path", command.text);
      break;
    case ControlCommand::Kind::SetRenderMode:
      take_text("mode", command.text);
      break;
    case ControlCommand::Kind::SetExposure:
      take("exposure");
      break;
    case ControlCommand::Kind::SetViewportSize: {
      // Both fields required: a resize with a missing dimension is a mistake,
      // not a request to keep the current one silently.
      std::uint64_t width = 0;
      std::uint64_t height = 0;
      if (!find_unsigned_field(line, "width", width)) {
        error = "missing \"width\" unsigned field";
        return false;
      }
      if (!find_unsigned_field(line, "height", height)) {
        error = "missing \"height\" unsigned field";
        return false;
      }
      // Clamp at PARSE time rather than letting a hostile client ask for a
      // 2^63 x 2^63 swapchain: the viewport would pass it to vkCreateSwapchain
      // and either exhaust memory or trip a VUID. The clamp is part of the
      // wire contract so every client gets the same behaviour.
      constexpr std::uint64_t kMinExtent = 64U;
      constexpr std::uint64_t kMaxExtent = 8192U;
      if (width < kMinExtent || width > kMaxExtent || height < kMinExtent ||
          height > kMaxExtent) {
        error = "width and height must be in [64, 8192]";
        return false;
      }
      numbers[count++] = static_cast<double>(width);
      numbers[count++] = static_cast<double>(height);
      break;
    }
    case ControlCommand::Kind::SetBloom:
      take("enabled");
      break;
    case ControlCommand::Kind::SetRtReflections:
      take("enabled");
      break;
    case ControlCommand::Kind::SetRtAo:
      take("enabled");
      break;
    case ControlCommand::Kind::SetPtSampleTarget:
      take("samples");
      break;
    case ControlCommand::Kind::SetDenoise: {
      // enabled is required (a missing toggle is a mistake, not an
      // implicit disable) -- the set_adaptive precedent, not the take()
      // default-zero shape, so the refusal holds on every host.
      double enabled = 0.0;
      if (!find_number_field(line, "enabled", enabled)) {
        error = "missing \"enabled\" number field";
        return false;
      }
      numbers[count++] = enabled;
      break;
    }
    case ControlCommand::Kind::SetAdaptive: {
      // enabled is required (a missing toggle is a mistake, not an
      // implicit disable); fraction defaults to 0.25 like the engine test.
      // Range-checking lives at the viewport dispatch, where the PT-state
      // refusal also lives — the parser only guarantees the keys exist.
      double enabled = 0.0;
      if (!find_number_field(line, "enabled", enabled)) {
        error = "missing \"enabled\" number field";
        return false;
      }
      double fraction = 0.25;
      (void)find_number_field(line, "fraction", fraction);
      numbers[count++] = enabled;
      numbers[count++] = fraction;
      break;
    }
    case ControlCommand::Kind::CopySubgraph:
      // Comma-separated ids ride in one string field (the flat wire format
      // has no arrays); the session parses them strictly.
      take_text("nids", command.text);
      break;
    case ControlCommand::Kind::PasteSubgraph:
      // Machine JSON with quotes: decoded escape-aware (the plain scanner
      // stops at the first interior quote).
      if (!find_escaped_string_field(line, "fragment", command.text)) {
        error = "missing \"fragment\" escaped-string field";
        return false;
      }
      break;
    default:
      break;
  }
  if (count > 8U) count = 8U;
  for (std::uint32_t i = 0; i < count; ++i) command.numbers[i] = numbers[i];
  command.number_count = count;
  return true;
}

//! Escapes a string for embedding in a JSON string literal (errors carry
//! client-supplied text like `unknown command "warp"`, whose quotes must
//! not break the line's JSON structure).
[[nodiscard]] std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8U);
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(const std::string& path, std::string& error) {
  OMNICPP_CONTRACT(!path.empty());
  if (listen_fd_ >= 0) {
    error = "already running";
    return false;
  }
  // Non-blocking at creation: poll() must never block, including accept4
  // on a listen socket with no pending connections.
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    error = std::string("socket(): ") + std::strerror(errno);
    return false;
  }
  ::sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    error = "socket path too long";
    ::close(fd);
    return false;
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  // A stale socket file from a crashed previous run must not block binding.
  ::unlink(path.c_str());
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    error = std::string("bind(): ") + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (::listen(fd, 8) != 0) {
    error = std::string("listen(): ") + std::strerror(errno);
    ::close(fd);
    ::unlink(path.c_str());
    return false;
  }
  listen_fd_ = fd;
  socket_path_ = path;
  return true;
}

void ControlServer::stop() {
  for (auto& client : clients_) {
    if (client.fd >= 0) ::close(client.fd);
  }
  clients_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (!socket_path_.empty()) {
    ::unlink(socket_path_.c_str());
    socket_path_.clear();
  }
}

void ControlServer::queue_welcome(Client& client, ControlHost& host) {
  client.out_buffer += "{\"event\":\"welcome\",\"protocol\":1,\"snapshot\":";
  client.out_buffer += host.snapshot_json();
  client.out_buffer += "}\n";
}

void ControlServer::queue_reply(Client& client, const ControlReply& reply) const {
  char header[64];
  std::snprintf(header, sizeof(header), "{\"id\":%llu,\"ok\":%s,",
                static_cast<unsigned long long>(reply.id),
                reply.ok ? "true" : "false");
  client.out_buffer += header;
  if (reply.ok) {
    client.out_buffer += "\"detail\":\"" + json_escape(reply.detail) + "\"";
  } else {
    client.out_buffer += "\"error\":\"" + json_escape(reply.error) + "\"";
  }
  client.out_buffer += "}\n";
}

void ControlServer::drop_client(std::size_t index) {
  OMNICPP_CONTRACT(index < clients_.size());
  if (clients_[index].fd >= 0) ::close(clients_[index].fd);
  clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

void ControlServer::flush(Client& client) {
  while (!client.out_buffer.empty()) {
    const ssize_t sent =
        ::send(client.fd, client.out_buffer.data(), client.out_buffer.size(),
               MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
      client.out_buffer.erase(0, static_cast<std::size_t>(sent));
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    // Hard send error: force the socket closed so poll() drops the client.
    ::close(client.fd);
    client.fd = -1;
    break;
  }
}

std::size_t ControlServer::poll(ControlHost& host) {
  if (listen_fd_ < 0) return 0;

  // 1. Accept pending connections (welcome + snapshot on connect).
  for (;;) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
    if (fd < 0) break;  // EAGAIN or real error; both handled by epoll-free loop
    Client client;
    client.fd = fd;
    queue_welcome(client, host);
    clients_.push_back(std::move(client));
  }

  std::size_t processed = 0;
  // 2. Read + dispatch. Iterate by index so drops keep the loop valid.
  for (std::size_t i = 0; i < clients_.size();) {
    Client& client = clients_[i];
    bool dead = client.fd < 0;

    if (!dead) {
      char chunk[4096];
      for (;;) {
        const ssize_t got =
            ::recv(client.fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (got > 0) {
          client.in_buffer.append(chunk, static_cast<std::size_t>(got));
          if (client.in_buffer.size() > (1U << 20U)) {
            dead = true;  // runaway client
            break;
          }
          if (static_cast<std::size_t>(got) < sizeof(chunk)) break;
          continue;
        }
        if (got == 0) {
          dead = true;  // orderly disconnect
        }
        break;  // EAGAIN or error: leave to dead/flush handling
      }
    }

    if (!dead) {
      // Dispatch every complete line in the buffer.
      std::size_t newline;
      while ((newline = client.in_buffer.find('\n')) != std::string::npos) {
        const std::string line = client.in_buffer.substr(0, newline);
        client.in_buffer.erase(0, newline + 1);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;

        ControlCommand command;
        std::string parse_error;
        if (!parse_command(line, command, parse_error)) {
          ControlReply bad;
          bad.ok = false;
          bad.error = parse_error;
          queue_reply(client, bad);
          continue;
        }
        ++processed;
        ControlReply reply = host.on_control(command);
        reply.id = command.id;
        queue_reply(client, reply);
      }
    }

    if (!dead) flush(client);
    if (dead || client.fd < 0) {
      drop_client(i);
      continue;
    }
    ++i;
  }
  return processed;
}

}  // namespace warploom::core
