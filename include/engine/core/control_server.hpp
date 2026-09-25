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
//!   v1.1 (M3 document query/edit):
//!     {"cmd":"list_objects","id":9}
//!     {"cmd":"get_object","id":10,"id":3}          (object id as "oid"? no: "id" collision — uses "oid")
//!     {"cmd":"set_property","id":11,"object":"cube_2","key":"position","x":1,"y":2,"z":3}
//!       numeric values: x/y/z (vec3 needs all three, number uses x)
//!       bool/string values: "value":"true" | "value":"<text>"
//!     {"cmd":"destroy_object","id":12,"oid":3}
//!     {"cmd":"undo","id":13} / {"cmd":"redo","id":14}
//!     {"cmd":"schema","id":15}
//!   v1.3 (M7 node graph):
//!     {"cmd":"add_node","id":16,"type":"const_number","x":40,"y":80}
//!     {"cmd":"remove_node","id":17,"nid":3}
//!     {"cmd":"link_nodes","id":18,"from":1,"to":2,"out":"value","in":"a"}
//!     {"cmd":"unlink_nodes","id":19,"nid":2,"in":"a"}
//!     {"cmd":"set_node_param","id":20,"nid":1,"key":"value","x":7}
//!     {"cmd":"set_node_position","id":21,"nid":2,"x":200,"y":120}
//!     {"cmd":"get_graph","id":22}
//!   v1.4 (M8 document persistence):
//!     {"cmd":"save_document","id":23,"path":"scene.json"}
//!     {"cmd":"load_document","id":24,"path":"scene.json"}
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
    // v1.1: document query/edit (M3 editor session).
    ListObjects,
    GetObject,
    SetProperty,
    DestroyObject,
    Undo,
    Redo,
    Schema,
    // v1.2: editor selection (the host mirrors it onto editor state).
    Select,
    // v1.3: node-graph editing (M7) — all undoable in the session.
    NodeAdd,
    NodeRemove,
    LinkNodes,
    UnlinkNodes,
    SetNodeParam,
    SetNodePosition,
    // v1.3 queries.
    GetGraph,
    // v1.4: document persistence (M8) — path rides in `text`.
    SaveDocument,
    LoadDocument,
    // v1.5: graph -> scene bindings (M10).
    //   BindNodeProperty:   numbers[0] = node id, numbers[1] = object id,
    //                       text = out pin, text2 = property (axis
    //                       suffix like "position.y" allowed)
    //   UnbindNodeProperty: numbers[0] = object id, text = property
    //   ListBindings:       no payload; detail = JSON array
    BindNodeProperty,
    UnbindNodeProperty,
    ListBindings,
  };

  Kind kind{Kind::Unknown};
  std::uint64_t id{0};  // client-assigned, echoed in the reply
  double numbers[8]{};  // see kind-specific meaning in control_server.cpp
  std::uint32_t number_count{0};
  // v1.1 string payloads (empty when unused):
  //   GetObject/DestroyObject/Select: numbers[0] = object id (key "oid";
  //   Select uses oid=0 to deselect)
  //   SetProperty: text = object name, text2 = property key,
  //                text3 = bool/string value (numeric values use x/y/z)
  // v1.3 node payloads:
  //   NodeAdd:        text = node type, numbers[0..1] = x,y (default 40,40)
  //   NodeRemove:     numbers[0] = node id ("nid")
  //   LinkNodes:      numbers[0] = from id, numbers[1] = to id,
  //                   text = out pin, text2 = in pin
  //   UnlinkNodes:    numbers[0] = to id, text = in pin
  //   SetNodeParam:   numbers[0] = node id, text = param key, text2/text3 =
  //                   bare value (string form; "true"/"false" = bool,
  //                   numeric value uses x)
  //   SetNodePosition: numbers[0] = node id, numbers[1..2] = x,y
  std::string text;
  std::string text2;
  std::string text3;
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
