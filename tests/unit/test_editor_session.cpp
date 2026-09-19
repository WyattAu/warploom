//! @file test_editor_session.cpp
//! @brief M3 editor-session proofs: the document-backed ControlHost handles
//!        the full v1.1 protocol headlessly — seeded environment snapshot,
//!        every edit round-trips (execute + undo restores state), queries
//!        serialize exactly, validation rejects bad payloads, and the
//!        control-server end-to-end path drives the session over a real
//!        socket pair.

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "engine/core/control_server.hpp"
#include "engine/core/editor_session.hpp"

namespace {

namespace ed = omnicpp::editor;

using omnicpp::core::ControlCommand;
using omnicpp::core::ControlReply;
using omnicpp::editor::EditorSession;

//! Builds a session and spawns one cube at (1, 0.5, -1) via the protocol,
//! so edit tests start from a two-object document.
struct SessionFixture {
  EditorSession session;

  SessionFixture() { spawn_cube(1.0, 0.5, -1.0, 2.0); }

  [[nodiscard]] ControlReply send(ControlCommand::Kind kind,
                                  const double* numbers = nullptr,
                                  std::uint32_t count = 0,
                                  const std::string& t = {},
                                  const std::string& t2 = {},
                                  const std::string& t3 = {}) {
    ControlCommand c;
    c.kind = kind;
    c.id = 42;
    if (numbers != nullptr) {
      for (std::uint32_t i = 0; i < count && i < 8; ++i) {
        c.numbers[i] = numbers[i];
      }
      c.number_count = count;
    }
    c.text = t;
    c.text2 = t2;
    c.text3 = t3;
    return session.on_control(c);
  }

  [[nodiscard]] ControlReply spawn_cube(double x, double y, double z,
                                        double size) {
    const double n[4] = {x, y, z, size};
    return send(ControlCommand::Kind::SpawnCube, n, 4);
  }
};

// ============================================================================
// Snapshot / seeding
// ============================================================================

TEST(EditorSessionSnapshot, SeedsEnvironmentAndReportsDepths) {
  SessionFixture f;
  const std::string snap = f.session.snapshot_json();
  EXPECT_NE(snap.find("\"name\":\"environment\""), std::string::npos);
  EXPECT_NE(snap.find("\"camera_fov\":60"), std::string::npos);
  EXPECT_NE(snap.find("\"count\":2"), std::string::npos);  // env + spawned cube
  EXPECT_NE(snap.find("\"undo_depth\":1"), std::string::npos);
}

// ============================================================================
// Edits
// ============================================================================

TEST(EditorSessionEdits, SpawnCubeGrowsDocument) {
  SessionFixture f;
  const auto r = f.spawn_cube(3.0, 1.0, 4.0, 1.5);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().objects.size(), 3U);
  const auto* cube = f.session.document().find(3U);
  ASSERT_NE(cube, nullptr);
  EXPECT_EQ(cube->name, "cube_3");
  const auto& pos = cube->properties.at("position");
  EXPECT_DOUBLE_EQ(pos.vec[0], 3.0);
  const auto& scale = cube->properties.at("scale");
  EXPECT_DOUBLE_EQ(scale.vec[1], 1.5);
}

TEST(EditorSessionEdits, SetPropertyVec3ThenUndoRestores) {
  SessionFixture f;
  const std::string before = f.session.document().to_json();
  const double n[3] = {5.0, 6.0, 7.0};
  const auto r = f.send(ControlCommand::Kind::SetProperty, n, 3,
                                "environment", "sun_direction");
  ASSERT_TRUE(r.ok) << r.error;
  const auto* env = f.session.document().find(1U);
  EXPECT_DOUBLE_EQ(env->properties.at("sun_direction").vec[0], 5.0);
  std::string err;
  ASSERT_TRUE(f.session.stack().undo(err)) << err;
  EXPECT_EQ(f.session.document().to_json(), before);
}

TEST(EditorSessionEdits, SetCameraIsAtomicSingleUndoStep) {
  SessionFixture f;
  const std::string before = f.session.document().to_json();
  const double n[7] = {10, 4, 10, 0, 1, 0, 55};
  const auto r = f.send(ControlCommand::Kind::SetCamera, n, 7);
  ASSERT_TRUE(r.ok) << r.error;
  std::string err;
  ASSERT_TRUE(f.session.stack().undo(err)) << err;  // ONE undo for all three
  EXPECT_EQ(f.session.document().to_json(), before);
}

TEST(EditorSessionEdits, DestroyThenUndoReinserts) {
  SessionFixture f;
  const std::string before = f.session.document().to_json();
  const double oid[1] = {2.0};
  const auto r = f.send(ControlCommand::Kind::DestroyObject, oid, 1);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().find(2U), nullptr);
  std::string err;
  ASSERT_TRUE(f.session.stack().undo(err)) << err;
  EXPECT_EQ(f.session.document().to_json(), before);
}

TEST(EditorSessionEdits, RedoReappliesDestroy) {
  SessionFixture f;
  const double oid[1] = {2.0};
  ASSERT_TRUE(f.send(ControlCommand::Kind::DestroyObject, oid, 1).ok);
  std::string err;
  ASSERT_TRUE(f.session.stack().undo(err)) << err;
  ASSERT_TRUE(f.session.stack().redo(err)) << err;
  EXPECT_EQ(f.session.document().find(2U), nullptr);
  // A second redo is rejected (nothing left).
  const auto r = f.send(ControlCommand::Kind::Redo);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error, "nothing to redo");
}

TEST(EditorSessionEdits, RejectsInvalidPayloads) {
  SessionFixture f;
  // Unknown object.
  auto r = f.send(ControlCommand::Kind::SetProperty, nullptr, 0,
                          "nope", "position");
  EXPECT_FALSE(r.ok);
  // Unknown key.
  r = f.send(ControlCommand::Kind::SetProperty, nullptr, 0,
                     "environment", "not_a_key");
  EXPECT_FALSE(r.ok);
  // Type mismatch: 3 numbers into a number property.
  const double three[3] = {1, 2, 3};
  r = f.send(ControlCommand::Kind::SetProperty, three, 3,
                     "environment", "camera_fov");
  EXPECT_FALSE(r.ok);
  // Destroy missing object.
  const double oid[1] = {999.0};
  r = f.send(ControlCommand::Kind::DestroyObject, oid, 1);
  EXPECT_FALSE(r.ok);
  // Spawn with negative size.
  const double bad[4] = {0, 0, 0, -1};
  r = f.spawn_cube(bad[0], bad[1], bad[2], bad[3]);
  EXPECT_FALSE(r.ok);
}

// ============================================================================
// Queries
// ============================================================================

TEST(EditorSessionQueries, ListObjectsSerializesAll) {
  SessionFixture f;
  const auto r = f.send(ControlCommand::Kind::ListObjects);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_NE(r.detail.find("\"name\":\"environment\""), std::string::npos);
  EXPECT_NE(r.detail.find("\"name\":\"cube_2\""), std::string::npos);
  EXPECT_NE(r.detail.find("\"count\":2"), std::string::npos);
}

TEST(EditorSessionQueries, GetObjectReturnsExactObject) {
  SessionFixture f;
  const double oid[1] = {2.0};
  const auto r = f.send(ControlCommand::Kind::GetObject, oid, 1);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_NE(r.detail.find("\"id\":2"), std::string::npos);
  EXPECT_NE(r.detail.find("\"type\":\"cube\""), std::string::npos);
  // Missing object is a clean error.
  const double missing[1] = {77.0};
  const auto r2 = f.send(ControlCommand::Kind::GetObject, missing, 1);
  EXPECT_FALSE(r2.ok);
}

TEST(EditorSessionQueries, SchemaDescribesRegistry) {
  SessionFixture f;
  const auto r = f.send(ControlCommand::Kind::Schema);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_NE(r.detail.find("\"name\":\"cube\""), std::string::npos);
  EXPECT_NE(r.detail.find("\"position\":{\"type\":\"vec3\""),
            std::string::npos);
  EXPECT_NE(r.detail.find("\"radius\":{\"type\":\"number\""),
            std::string::npos);
  EXPECT_NE(r.detail.find("\"schema_version\":" +
                          std::to_string(
                              omnicpp::editor::kDocumentSchemaVersion)),
            std::string::npos);
}

// ============================================================================
// Selection (v1.2)
// ============================================================================

TEST(EditorSessionSelect, SetClearAndValidate) {
  SessionFixture f;
  // Select the spawned cube (oid 2).
  const double oid[1] = {2.0};
  auto r = f.send(ControlCommand::Kind::Select, oid, 1);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.selected_id(), 2U);
  // Snapshot mirrors it.
  EXPECT_NE(f.session.snapshot_json().find("\"selected\":2"),
            std::string::npos);
  // Unknown id is rejected.
  const double missing[1] = {99.0};
  r = f.send(ControlCommand::Kind::Select, missing, 1);
  EXPECT_FALSE(r.ok);
  // Deselect (oid 0) always valid.
  const double none[1] = {0.0};
  r = f.send(ControlCommand::Kind::Select, none, 1);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.selected_id(), 0U);
}

TEST(EditorSessionSelect, SelectIsNotDocumentState) {
  SessionFixture f;
  const std::string before = f.session.document().to_json();
  const double oid[1] = {2.0};
  ASSERT_TRUE(f.send(ControlCommand::Kind::Select, oid, 1).ok);
  // Document bytes unchanged and undo stack untouched.
  EXPECT_EQ(f.session.document().to_json(), before);
  EXPECT_EQ(f.session.stack().undo_count(), 1U);  // only the fixture spawn
}

// ============================================================================
// End-to-end through the real control server
// ============================================================================

TEST(EditorSessionE2E, ControlServerDrivesSession) {
  SessionFixture f;
  omnicpp::core::ControlServer server;
  std::string error;
  const std::string path = "/tmp/omnicpp_test_editor_session.sock";
  ASSERT_TRUE(server.start(path, error)) << error;

  int client = -1;
  ASSERT_GE(client = socket(AF_UNIX, SOCK_STREAM, 0), 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
            0);

  // Drain the welcome snapshot.
  server.poll(f.session);
  char buf[4096];
  const auto n = recv(client, buf, sizeof(buf) - 1, 0);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  EXPECT_NE(std::string(buf).find("\"event\":\"welcome\""),
            std::string::npos);

  // Send list_objects; the session must answer through the server.
  const std::string line = "{\"cmd\":\"list_objects\",\"id\":7}\n";
  ASSERT_EQ(send(client, line.data(), line.size(), 0),
            static_cast<ssize_t>(line.size()));
  server.poll(f.session);
  const auto n2 = recv(client, buf, sizeof(buf) - 1, MSG_DONTWAIT);
  ASSERT_GT(n2, 0);
  buf[n2] = '\0';
  EXPECT_NE(std::string(buf).find("\"id\":7,\"ok\":true"),
            std::string::npos);
  EXPECT_NE(std::string(buf).find("cube_2"), std::string::npos);

  close(client);
  server.stop();
}

// ============================================================================
// v1.3 node-graph protocol (M7)
// ============================================================================

TEST(EditorSessionNodes, ProtocolRoundTripAllCommands) {
  SessionFixture f;

  // add const -> add graph (positions ride in the numbers payload as
  // x=40,y=20 and x=60,y=80).
  const double pos0[2] = {40.0, 20.0};
  auto r = f.send(ControlCommand::Kind::NodeAdd, pos0, 2, "const_number");
  ASSERT_TRUE(r.ok) << r.error;
  const double pos1[2] = {60.0, 80.0};
  r = f.send(ControlCommand::Kind::NodeAdd, pos1, 2, "add");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().node_graph.node_count(), 2U);

  // Every add bumps undo depth; both are undoable.
  EXPECT_EQ(f.session.stack().undo_count(), 3U);  // fixture cube + 2 nodes

  // link const.value -> add.a
  const double ends[2] = {1.0, 2.0};
  r = f.send(ControlCommand::Kind::LinkNodes, ends, 2, "value", "a");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().node_graph.link_count(), 1U);

  // set_node_param on the const (x = 7.5).
  const double val[2] = {1.0, 7.5};
  r = f.send(ControlCommand::Kind::SetNodeParam, val, 2, "value");
  ASSERT_TRUE(r.ok) << r.error;
  const auto* node1 = f.session.document().node_graph.find(1U);
  ASSERT_NE(node1, nullptr);
  EXPECT_DOUBLE_EQ(node1->params.at("value").number, 7.5);

  // get_graph returns machine JSON containing the link.
  r = f.send(ControlCommand::Kind::GetGraph);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_NE(r.detail.find("\"links\""), std::string::npos);
  EXPECT_NE(r.detail.find("const_number"), std::string::npos);

  // set_node_position (undoable view state).
  const double move[3] = {2.0, 300.0, 120.0};
  r = f.send(ControlCommand::Kind::SetNodePosition, move, 3);
  ASSERT_TRUE(r.ok) << r.error;
  const auto& layout = f.session.document().node_layout;
  ASSERT_EQ(layout.count(2U), 1U);
  EXPECT_DOUBLE_EQ(layout.at(2U).first, 300.0);

  // unlink via the target pin (the INPUT lives on node 2).
  const double target[1] = {2.0};
  r = f.send(ControlCommand::Kind::UnlinkNodes, target, 1, {}, "a");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().node_graph.link_count(), 0U);

  // Undo the unlink → link restored.
  r = f.send(ControlCommand::Kind::Undo);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().node_graph.link_count(), 1U);

  // remove_node 2 (drops the link), then undo restores it.
  const double nid[1] = {2.0};
  r = f.send(ControlCommand::Kind::NodeRemove, nid, 1);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().node_graph.node_count(), 1U);
  EXPECT_EQ(f.session.document().node_graph.link_count(), 0U);
  r = f.send(ControlCommand::Kind::Undo);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(f.session.document().node_graph.node_count(), 2U);
  EXPECT_EQ(f.session.document().node_graph.link_count(), 1U);
  EXPECT_EQ(f.session.document().node_graph.find(2U)->id, 2U);

  // Schema validation: unknown type rejected with a precise error.
  r = f.send(ControlCommand::Kind::NodeAdd, nullptr, 0, "not_a_type");
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("unknown type"), std::string::npos) << r.error;

  // Link validation: type mismatch (string out -> number in impossible here;
  // use an unknown pin).
  const double bad_ends[2] = {1.0, 2.0};
  r = f.send(ControlCommand::Kind::LinkNodes, bad_ends, 2, "value", "zzz");
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("unknown input pin"), std::string::npos)
      << r.error;

  // Snapshot exposes graph counters.
  const std::string snap = f.session.snapshot_json();
  EXPECT_NE(snap.find("\"nodes\":2"), std::string::npos);
  EXPECT_NE(snap.find("\"links\":1"), std::string::npos);

  // Whole-document persistence: bytes contain the graph and reload.
  const std::string bytes = f.session.document().to_json();
  ed::SceneDocument reloaded;
  ed::register_builtin_node_types(reloaded.node_graph);
  std::string parse_error;
  ASSERT_TRUE(ed::SceneDocument::from_json(bytes, reloaded, parse_error))
      << parse_error;
  EXPECT_EQ(reloaded.node_graph.node_count(), 2U);
  EXPECT_EQ(reloaded.node_graph.link_count(), 1U);
}

TEST(EditorSessionNodes, SocketDrivenNodeCommands) {
  SessionFixture f;
  omnicpp::core::ControlServer server;
  const std::string path = "/tmp/omnicpp_test_editor_nodes.sock";
  std::string error;
  ASSERT_TRUE(server.start(path, error)) << error;

  const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(client, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
            0);

  // Drain welcome.
  server.poll(f.session);
  char buf[4096];
  ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
  ASSERT_GT(n, 0);

  // Drive two node adds + a link as raw JSONL.
  const std::string lines =
      std::string("{\"cmd\":\"add_node\",\"id\":1,\"type\":\"const_number\",") +
      "\"x\":10,\"y\":20}\n"
      "{\"cmd\":\"add_node\",\"id\":2,\"type\":\"add\",\"x\":200,"
      "\"y\":20}\n"
      "{\"cmd\":\"link_nodes\",\"id\":3,\"from\":1,\"to\":2,"
      "\"out\":\"value\",\"in\":\"a\"}\n"
      "{\"cmd\":\"get_graph\",\"id\":4}\n";
  ASSERT_EQ(send(client, lines.data(), lines.size(), 0),
            static_cast<ssize_t>(lines.size()));
  server.poll(f.session);
  n = recv(client, buf, sizeof(buf) - 1, MSG_DONTWAIT);
  ASSERT_GT(n, 0);
  buf[n] = '\0';
  const std::string replies(buf);
  EXPECT_NE(replies.find("\"id\":1,\"ok\":true"), std::string::npos);
  EXPECT_NE(replies.find("\"id\":3,\"ok\":true"), std::string::npos);
  EXPECT_NE(replies.find("\"id\":4,\"ok\":true"), std::string::npos);
  EXPECT_EQ(f.session.document().node_graph.node_count(), 2U);
  EXPECT_EQ(f.session.document().node_graph.link_count(), 1U);

  close(client);
  server.stop();
}

}  // namespace
