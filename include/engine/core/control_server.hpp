#pragma once

//! @file control_server.hpp
//! @brief M0 editor/automation control channel: unix-socket JSONL protocol.
//!
//! Topology: the engine hosts `ControlServer` (authoritative, deterministic);
//! editor or test clients connect, receive a `welcome` + full `snapshot`,
//! then drive the simulation with commands. Every reply is a single JSON
//! line. The engine never blocks: the server is polled once per frame (or
//! from tests directly) and tolerates partial reads, multiple clients, and
//! abrupt disconnects.
//!
//! Protocol v1 (one JSON object per line):
//!   client -> host:
//!     {"cmd":"ping","id":1}
//!     {"cmd":"pause","id":2} / {"cmd":"resume","id":3}
//!     {"cmd":"step","id":4,"ticks":5}
//!     {"cmd":"set_camera","id":5,"ex":8,"ey":3,"ez":0,"tx":0,"ty":1,"tz":0,"fov":60}
//!     {"cmd":"set_sun","id":6,"x":0.3,"y":0.65,"z":0.7}
//!     {"cmd":"spawn_cube","id":7,"x":0,"y":0.5,"z":0,"size":1}
//!     {"cmd":"capture","id":8,"frame":5}
//!   host -> client:
//!     {"event":"welcome","protocol":1,"snapshot":<host JSON>}
//!     {"id":1,"ok":true,"detail":"..."} | {"id":1,"ok":false,"error":"..."}

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omnicpp::core {

//! Generic numeric payload for commands (camera/sun/cube/step counts).
struct ControlCommand final {
  enum class Kind : std::uint8_t {
    Unknown,
    Ping,
    Pause,
    Resume,
    Step,
    SetCamera,
    SetSun,
    SpawnCube,
    Capture,
  };

  Kind kind{Kind::Unknown};
  std::uint64_t id{0};  // client-assigned, echoed in the reply
  double numbers[8]{};  // see kind-specific meaning in control_server.cpp
  std::uint32_t number_count{0};
};

//! One protocol reply (serialized to a single JSON line).
struct ControlReply final {
  std::uint64_t id{0};
  bool ok{false};
  std::string error;   // when !ok
  std::string detail;  // when ok
};

//! The engine-facing surface. The viewport (or a test fake) implements this;
//! the server stays engine-agnostic.
class ControlHost {
 public:
  virtual ~ControlHost() = default;
  //! Apply a command and produce its reply. Must stay non-blocking.
  [[nodiscard]] virtual ControlReply on_control(
      const ControlCommand& command) = 0;
  //! Authoritative state dump sent on connect (single JSON object, no
  //! trailing newline). Must stay non-blocking and small (< 1 MiB).
  [[nodiscard]] virtual std::string snapshot_json() const = 0;
};

//! Poll-based, non-blocking unix-socket JSONL server.
class ControlServer final {
 public:
  ControlServer() = default;
  ~ControlServer();
  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  //! Binds `path` (an existing stale socket file is unlinked) and starts
  //! listening. Returns false + `error` on failure.
  [[nodiscard]] bool start(const std::string& path, std::string& error);
  //! Closes all connections and removes the socket file.
  void stop();
  [[nodiscard]] bool is_running() const noexcept { return listen_fd_ >= 0; }
  [[nodiscard]] std::size_t client_count() const noexcept {
    return clients_.size();
  }
  [[nodiscard]] const std::string& socket_path() const noexcept {
    return socket_path_;
  }

  //! Accepts new clients (sends welcome + snapshot), reads and dispatches
  //! pending command lines, flushes queued replies. Returns the number of
  //! commands processed. Never blocks.
  std::size_t poll(ControlHost& host);

 private:
  struct Client {
    int fd;
    std::string in_buffer;   // partial command line accumulation
    std::string out_buffer;  // unsent reply bytes
  };

  void queue_reply(Client& client, const ControlReply& reply) const;
  void queue_welcome(Client& client, ControlHost& host);
  void drop_client(std::size_t index);
  void flush(Client& client);

  int listen_fd_{-1};
  std::string socket_path_{};
  std::vector<Client> clients_{};
};

}  // namespace omnicpp::core
