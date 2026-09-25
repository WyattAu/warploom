//! @file control_server.cpp
//! @brief Unix-socket JSONL control server (see the header for protocol v1).
//!
//! Command parsing reuses the flat-JSON field-scan approach from the input
//! script loader: protocol lines are flat one-line objects, so targeted scans
//! stay dependency-free and strict about the accepted shape. Unknown fields
//! are ignored (forward compatibility); unknown commands are protocol errors.

#include "engine/core/control_server.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "engine/core/contract.hpp"

namespace omnicpp::core {

namespace {

//! Flat-JSON scans (same approach as input_state.cpp's script parser).

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
  if (value < 0.0 || value != static_cast<double>(static_cast<std::uint64_t>(value))) {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
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

  struct Mapping {
    const char* name;
    ControlCommand::Kind kind;
  };
  constexpr Mapping kMappings[] = {
      {"ping", ControlCommand::Kind::Ping},
      {"pause", ControlCommand::Kind::Pause},
      {"resume", ControlCommand::Kind::Resume},
      {"step", ControlCommand::Kind::Step},
      {"set_camera", ControlCommand::Kind::SetCamera},
      {"set_sun", ControlCommand::Kind::SetSun},
      {"spawn_cube", ControlCommand::Kind::SpawnCube},
      {"capture", ControlCommand::Kind::Capture},
      {"list_objects", ControlCommand::Kind::ListObjects},
      {"get_object", ControlCommand::Kind::GetObject},
      {"set_property", ControlCommand::Kind::SetProperty},
      {"destroy_object", ControlCommand::Kind::DestroyObject},
      {"undo", ControlCommand::Kind::Undo},
      {"redo", ControlCommand::Kind::Redo},
      {"schema", ControlCommand::Kind::Schema},
      {"select", ControlCommand::Kind::Select},
      // v1.3 node graph (M7).
      {"add_node", ControlCommand::Kind::NodeAdd},
      {"remove_node", ControlCommand::Kind::NodeRemove},
      {"link_nodes", ControlCommand::Kind::LinkNodes},
      {"unlink_nodes", ControlCommand::Kind::UnlinkNodes},
      {"set_node_param", ControlCommand::Kind::SetNodeParam},
      {"set_node_position", ControlCommand::Kind::SetNodePosition},
      {"get_graph", ControlCommand::Kind::GetGraph},
      {"save_document", ControlCommand::Kind::SaveDocument},
      {"load_document", ControlCommand::Kind::LoadDocument},
      // v1.5 graph->scene bindings (M10).
      {"bind_node_property", ControlCommand::Kind::BindNodeProperty},
      {"unbind_node_property", ControlCommand::Kind::UnbindNodeProperty},
      {"list_bindings", ControlCommand::Kind::ListBindings},
  };
  command.kind = ControlCommand::Kind::Unknown;
  command.number_count = 0;
  for (const auto& mapping : kMappings) {
    if (cmd == mapping.name) {
      command.kind = mapping.kind;
      break;
    }
  }
  if (command.kind == ControlCommand::Kind::Unknown) {
    error = "unknown command \"" + cmd + "\"";
    return false;
  }

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
    case ControlCommand::Kind::Select: {
      std::uint64_t oid = 0;
      if (!find_unsigned_field(line, "oid", oid)) {
        error = "missing \"oid\" unsigned field";
        return false;
      }
      numbers[count++] = static_cast<double>(oid);
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
      break;
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
    case ControlCommand::Kind::SaveDocument:
    case ControlCommand::Kind::LoadDocument:
      take_text("path", command.text);
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

}  // namespace omnicpp::core
