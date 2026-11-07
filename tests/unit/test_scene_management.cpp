//! @file test_scene_management.cpp
//! @brief D3 scene-management proofs (loop75): current-path tracking, dirty
//!        state, new_document, and save-to-current-path.
//!
//! Dirtiness is hash-derived (state_hash over byte-deterministic to_json),
//! so the tests pin the property a flag cannot give: undoing back to the
//! saved bytes reads clean, and no per-command bookkeeping exists to drift.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <unistd.h>

#include "warploom/core/control_server.hpp"
#include "warploom/core/editor_session.hpp"

namespace {

namespace ed = omnicpp::editor;

using omnicpp::core::ControlCommand;
using omnicpp::core::ControlReply;
using omnicpp::editor::EditorSession;

using CK = ControlCommand::Kind;

[[nodiscard]] ControlReply send(EditorSession& session, CK kind,
                                const std::string& text = {}) {
  ControlCommand c;
  c.kind = kind;
  c.text = text;
  return session.on_control(c);
}

[[nodiscard]] ControlReply spawn_cube(EditorSession& session) {
  ControlCommand c;
  c.kind = CK::SpawnCube;
  c.numbers[0] = 1.0;
  c.numbers[1] = 0.5;
  c.numbers[2] = -1.0;
  c.numbers[3] = 2.0;
  c.number_count = 4;
  return session.on_control(c);
}

[[nodiscard]] std::string unique_path(const char* stem) {
  return std::string("/tmp/warploom_test_") + stem + "_" +
         std::to_string(::getpid()) + ".json";
}

TEST(SceneManagement, FreshSessionIsCleanAndUntitled) {
  EditorSession session;
  EXPECT_FALSE(session.is_dirty());
  EXPECT_TRUE(session.current_path().empty());
}

TEST(SceneManagement, EditMarksDirty) {
  EditorSession session;
  ASSERT_TRUE(spawn_cube(session).ok);
  EXPECT_TRUE(session.is_dirty());
}

TEST(SceneManagement, SaveClearsDirtyAndSetsPath) {
  EditorSession session;
  ASSERT_TRUE(spawn_cube(session).ok);
  const std::string path = unique_path("scene_save");
  ::remove(path.c_str());
  const ControlReply saved = send(session, CK::SaveDocument, path);
  ASSERT_TRUE(saved.ok) << saved.error;
  EXPECT_FALSE(session.is_dirty());
  EXPECT_EQ(session.current_path(), path);
  ::remove(path.c_str());
}

TEST(SceneManagement, BareSaveWritesToCurrentPath) {
  EditorSession session;
  ASSERT_TRUE(spawn_cube(session).ok);
  const std::string path = unique_path("scene_bare");
  ::remove(path.c_str());
  ASSERT_TRUE(send(session, CK::SaveDocument, path).ok);
  // A second edit, then a save with no path: must land on the same file.
  ASSERT_TRUE(spawn_cube(session).ok);
  EXPECT_TRUE(session.is_dirty());
  const ControlReply saved = send(session, CK::SaveDocument);
  ASSERT_TRUE(saved.ok) << saved.error;
  EXPECT_FALSE(session.is_dirty());
  EXPECT_EQ(session.current_path(), path);
  ::remove(path.c_str());
}

TEST(SceneManagement, BareSaveUntitledFailsLoudly) {
  EditorSession session;
  ASSERT_TRUE(spawn_cube(session).ok);
  const ControlReply saved = send(session, CK::SaveDocument);
  EXPECT_FALSE(saved.ok);
  EXPECT_TRUE(session.is_dirty());
  EXPECT_TRUE(session.current_path().empty());
}

TEST(SceneManagement, SaveFailureKeepsPathAndDirtiness) {
  EditorSession session;
  const std::string path = unique_path("scene_keep");
  ::remove(path.c_str());
  ASSERT_TRUE(send(session, CK::SaveDocument, path).ok);
  ASSERT_TRUE(spawn_cube(session).ok);
  // Unwritable directory: the save must fail AND leave state untouched.
  const ControlReply bad =
      send(session, CK::SaveDocument, "/tmp/no_such_dir_9x/file.json");
  EXPECT_FALSE(bad.ok);
  EXPECT_EQ(session.current_path(), path);
  EXPECT_TRUE(session.is_dirty());
  ::remove(path.c_str());
}

TEST(SceneManagement, LoadClearsDirtyAndSetsPath) {
  EditorSession session;
  ASSERT_TRUE(spawn_cube(session).ok);
  const std::string path = unique_path("scene_load");
  ::remove(path.c_str());
  ASSERT_TRUE(send(session, CK::SaveDocument, path).ok);
  ASSERT_TRUE(spawn_cube(session).ok);
  EXPECT_TRUE(session.is_dirty());
  const ControlReply loaded = send(session, CK::LoadDocument, path);
  ASSERT_TRUE(loaded.ok) << loaded.error;
  EXPECT_FALSE(session.is_dirty());
  EXPECT_EQ(session.current_path(), path);
  EXPECT_EQ(session.document().objects.size(), 2U);  // env + saved cube
  ::remove(path.c_str());
}

TEST(SceneManagement, UndoToSavedBytesReadsClean) {
  EditorSession session;
  const std::string path = unique_path("scene_undo");
  ::remove(path.c_str());
  ASSERT_TRUE(send(session, CK::SaveDocument, path).ok);
  ASSERT_TRUE(spawn_cube(session).ok);
  EXPECT_TRUE(session.is_dirty());
  // Undo removes the cube: the bytes are the saved bytes again, so a
  // flag-maintained dirty bit would lie here and the hash tells the truth.
  ASSERT_TRUE(send(session, CK::Undo).ok);
  EXPECT_FALSE(session.is_dirty());
  ::remove(path.c_str());
}

TEST(SceneManagement, NewDocumentResetsToFresh) {
  EditorSession session;
  ASSERT_TRUE(spawn_cube(session).ok);
  const std::string path = unique_path("scene_new");
  ::remove(path.c_str());
  ASSERT_TRUE(send(session, CK::SaveDocument, path).ok);
  const ControlReply fresh = send(session, CK::NewDocument);
  ASSERT_TRUE(fresh.ok) << fresh.error;
  EXPECT_FALSE(session.is_dirty());
  EXPECT_TRUE(session.current_path().empty());
  // Identical to a fresh session's document by construction: compare the
  // authoritative serializations, not just object counts.
  const EditorSession pristine;
  EXPECT_EQ(session.document().to_json(), pristine.document().to_json());
  ::remove(path.c_str());
}

TEST(SceneManagement, NewDocumentDuringCaptureIsRejected) {
  EditorSession session;
  ControlCommand start;
  start.kind = CK::StartCapture;
  start.numbers[0] = 0.0;
  start.number_count = 1;
  ASSERT_TRUE(session.on_control(start).ok);
  ASSERT_TRUE(spawn_cube(session).ok);
  const ControlReply rejected = send(session, CK::NewDocument);
  EXPECT_FALSE(rejected.ok);
  // The rejection must not have touched the document.
  EXPECT_TRUE(session.is_dirty());
  EXPECT_EQ(session.document().objects.size(), 2U);
}

TEST(SceneManagement, NewDocumentWireNameParsesAndRoundTrips) {
  // The kind_names table is the single source: parse the wire spelling and
  // require the reverse lookup to agree (the protocol-invariant test covers
  // the table generically; this pins the D3 command's presence on the wire).
  const auto& names = ControlCommand::kind_names();
  bool found = false;
  for (const auto& entry : names) {
    if (entry.kind == CK::NewDocument) {
      EXPECT_STREQ(entry.name, "new_document");
      found = true;
    }
  }
  EXPECT_TRUE(found);
  EXPECT_STREQ(ControlCommand::kind_name(CK::NewDocument), "new_document");
}

}  // namespace
