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

namespace warploom::core {

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
    // v1.20: scene management (D3) — new empty document. No payload;
    // same load-boundary semantics as LoadDocument (history clears,
    // rejected during active capture).
    NewDocument,
    // v1.5: graph -> scene bindings (M10).
    //   BindNodeProperty:   numbers[0] = node id, numbers[1] = object id,
    //                       text = out pin, text2 = property (axis
    //                       suffix like "position.y" allowed)
    //   UnbindNodeProperty: numbers[0] = object id, text = property
    //   ListBindings:       no payload; detail = JSON array
    BindNodeProperty,
    UnbindNodeProperty,
    ListBindings,
    // v1.6: replay scrubbing (W1) — hash-verified checkpoints of the
    // document timeline (docs/roadmap.md track W).
    //   ScrubStart: wire key "frame" (optional, default 0) -> numbers[0];
    //               captures a checkpoint at that sim frame
    //   ScrubTo:    wire key "frame" (required) -> numbers[0]; sim frame to
    //               restore (must be checkpointed)
    //   ScrubInfo:  no payload; detail = JSON {checkpoints, capacity, start}
    ScrubStart,
    ScrubTo,
    ScrubInfo,
    // v1.7: protocol record/replay (W2) — warploom-replay-v1 files
    // (docs/replay-format.md).
    //   StartCapture:  wire key "frame" (optional, default 0) -> numbers[0];
    //                  begins recording, embeds the opening checkpoint
    //   StopCapture:   wire key "path" (required) -> text; writes the file
    //                  atomically (tmp+rename, 0600) and stops recording
    //   CaptureStatus: no payload; detail = JSON {recording, frame,
    //                  commands, checkpoints, path}
    //   LoadReplay:    wire key "path" (required) -> text; hash-verified
    //                  checkpoint hydration + command-log re-apply in seq
    //                  order (docs/replay-format.md, "Load semantics")
    StartCapture,
    StopCapture,
    CaptureStatus,
    LoadReplay,
    // v1.8: timeline clips (G3) — document entities (schema v3); edits are
    // undoable, recording/playback are session-side driven values ticked by
    // the host (docs/warploom-timeline-plan.md).
    //   ClipAdd:        text = name (required), numbers[0] = start frame
    //                   (optional, default 0), numbers[1] = length frames
    //                   (optional, default 0)
    //   ClipRemove:     numbers[0] = clip id ("clip", required)
    //   ClipMove:       numbers[0] = clip id ("clip", required),
    //                   numbers[1] = new start frame ("frame", optional;
    //                   absent = move to 0? NO — required in practice:
    //                   absent keeps the current start)
    //   ClipRecord:     numbers[0] = clip id ("clip"), numbers[1] = object
    //                   id ("oid"), text = property — arms sample capture
    //   ClipRecordStop: no payload — disarms sample capture
    //   ClipPlay:       numbers[0] = clip id ("clip"), numbers[1] = start
    //                   frame ("frame", optional; default = clip's start)
    //   ClipStop:       no payload — disarms playback
    //   ClipsInfo:      no payload; detail = JSON array
    ClipAdd,
    ClipRemove,
    ClipMove,
    ClipRecord,
    ClipRecordStop,
    ClipPlay,
    ClipStop,
    ClipsInfo,
    // v1.9: render mode (E1) — host-owned visual stack selection. The
    // session stores nothing; the host builds/uses its stacks on demand and
    // reports the mode in its welcome snapshot.
    //   SetRenderMode: text = mode name ("forward" | "rt"), required.
    //   GetRenderMode: no payload; detail = JSON {"mode": "..."}
    SetRenderMode,
    GetRenderMode,
    // v1.9: exposure (E2/R4) — host-owned tonemap control, live.
    //   SetExposure: numbers[0] = exposure multiplier ("exposure", required,
    //                clamped by the host to [0.01, 100])
    SetExposure,
    // v1.10: bloom (E2/R4) — host-owned post-process toggle, live. All bloom
    // resources exist from initialize(), so this changes which passes record,
    // never what is built (docs/research/rendering-and-graphs.md, R7).
    // v1.11 viewport resize: numbers[0] = width, numbers[1] = height in
    // pixels ("width"/"height", both required, clamped to [64, 8192] at parse
    // time). Resizes the window, which exercises the full swapchain recreate
    // path on demand rather than only when a window manager happens to deliver
    // a ConfigureNotify -- that is what makes the path verifiable headlessly
    // instead of only on a desktop.
    SetViewportSize,
    //   SetBloom: numbers[0] = 0/1 ("enabled", required)
    //   GetBloom: no payload; detail = JSON {"enabled": true|false,
    //                                        "available": true|false}
    SetBloom,
    GetBloom,
    // v1.16: render-channel tracks (T5c). The reserved render_settings
    // object's numeric channels, plus which of them a playing clip drove on
    // the last tick. A host applies ONLY driven channels, so an untracked
    // exposure never overwrites a socket-set one.
    //   GetRenderConfig: no payload;
    //     detail = JSON {"exposure":N,"bloom":N,"render_mode":N,
    //                    "tone_map":N,"driven":["exposure",...]}
    //   A visual host appends "applied_*" for the values it actually set.
    GetRenderConfig,
    // v1.12: RT sub-toggles (E2/R2) — host-owned ray-query flags, live. No
    // pipeline rebuild: the RT shaders read push pad1 (bit 0 reflections,
    // bit 1 AO), so a toggle only changes the next recorded frame.
    //   SetRtReflections/SetRtAo: numbers[0] = 0/1 ("enabled", required)
    //   GetRtReflections/GetRtAo: no payload; detail = JSON {"enabled": ...}
    SetRtReflections,
    GetRtReflections,
    SetRtAo,
    GetRtAo,
    // v1.13: stepped PT (E2/R1) — deterministic "accumulate K frames".
    // The tracer holds at the target (further rounds no-op), so a stepped
    // run always converges to exactly K samples no matter the wall clock.
    //   SetPtSampleTarget: numbers[0] = K ("samples", required, 0 = unbounded)
    //   GetPtSamples: no payload; detail = JSON {"samples": N}
    //   GetPtMean: no payload; detail = JSON {"mean": overall mean luminance}
    SetPtSampleTarget,
    GetPtSamples,
    GetPtMean,
    // v1.14: subgraph copy/paste (G2) — engine half of the editor UI.
    //   CopySubgraph: text = comma-separated node ids ("nids", required,
    //                 e.g. "3,7,9"); detail = the fragment JSON (a strict
    //                 scanner parses it — hand-edits are rejected).
    //   PasteSubgraph: text = fragment JSON ("fragment", required, sent
    //                  JSON-escaped); undoable (removes the pasted nodes +
    //                  restores the id cursor); detail = JSON {"ids":[...]}.
    CopySubgraph,
    PasteSubgraph,
    // v1.15: atomic multi-property record (T5b) — one clip+object pair,
    // every recordable property as its own track.
    //   ClipRecordAll: numbers[0] = clip id ("clip"), numbers[1] = object
    //                  id ("oid"); arms one RecordArm per Number property
    //                  and per Vec3 axis (T5d state model).
    ClipRecordAll,
    // v1.17: PT adaptive sampling (T2) — host-owned live-PT refinement.
    // The tracer already keeps first + second moments and per-texel counts;
    // these commands only reach that machinery from the app. Enabling
    // snapshots ONE mask from the current accumulation (top `fraction` of
    // texels by luma variance, rank-selected so the budget is exact) and
    // masked-in texels keep refining while held texels keep their uniform
    // mean bit-exactly. Leaving PT disables it, so a re-entered stack is
    // uniform again instead of resuming a stale mask.
    //   SetAdaptive: numbers[0] = enabled 0/1 ("enabled", required),
    //                numbers[1] = top fraction ("fraction", optional,
    //                default 0.25, must lie in (0,1])
    //   GetAdaptive: no payload; detail = JSON {"enabled":bool,
    //                "fraction":N,"selected":N,"texels":N,
    //                "threshold":N}
    SetAdaptive,
    GetAdaptive,
    // v1.18: PT render-path denoiser (T2) -- the count-sourced edge-stopped
    // a-trous filter over (sums, second moment, G-buffer). Enabling points
    // the tonemap at the filtered MEAN (inverse_samples becomes 1) instead
    // of the raw sums; disabling returns to the raw accumulation. Leaving
    // PT disables it with the stack.
    //   SetDenoise: numbers[0] = enabled 0/1 ("enabled", required)
    //   GetDenoise: no payload; detail = JSON {"enabled":bool}
    //   GetDenoisedMean: no payload; detail = JSON {"mean":...} over the
    //                    filtered mean (vs GetPtMean over the raw sums)
    SetDenoise,
    GetDenoise,
    GetDenoisedMean,
    // v1.19: session physics over the wire (D1 demo story). Bodies live in
    // the session's PhysicsWorld (capture-recorded, scrub-restored); creation
    // is session-side intent like clip arms, not an undoable document edit.
    // Position is the object's authored transform; only the dynamic
    // parameters ride the wire.
    //   SpawnPhysicsBody: numbers[0] = object id ("oid", required),
    //                     numbers[1] = radius ("radius", default 0.5),
    //                     numbers[2] = inverse mass ("inverse_mass",
    //                     default 1; 0 = static),
    //                     numbers[3] = restitution ("restitution",
    //                     default 0.4),
    //                     numbers[4..6] = velocity ("vx","vy","vz",
    //                     default 0).
    //   GetPhysicsBody:   numbers[0] = object id ("oid", required);
    //                     detail = JSON {oid,pos,vel,radius,restitution}.
    SpawnPhysicsBody,
    GetPhysicsBody,
  };

  //! One entry of the public command-name table: every `Kind` maps to
  //! exactly one wire name (v1.7 exposes the table itself so tests can
  //! enforce the protocol invariant — every kind parses and round-trips —
  //! against the REAL table, not a copy). Ordered by protocol version.
  struct KindName {
    Kind kind;
    const char* name;
  };
  [[nodiscard]] static const std::vector<KindName>& kind_names();
  //! Reverse lookup: the wire name for `kind` ("unknown" for Unknown).
  //! Used by the W2 recorder to log command names generically.
  [[nodiscard]] static const char* kind_name(Kind kind) noexcept;

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
  // v1.14 subgraph payloads:
  //   CopySubgraph:  text = comma-separated node ids ("nids")
  //   PasteSubgraph: text = fragment JSON ("fragment", escape-decoded)
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

}  // namespace warploom::core