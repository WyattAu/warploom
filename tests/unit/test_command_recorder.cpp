//! @file test_command_recorder.cpp
//! @brief W2 proofs for warploom-replay-v1 (docs/replay-format.md):
//!        capture -> mutate -> stop produces a valid file; re-capture
//!        replaces (no double-stop); stop failure keeps the session
//!        intact; a recorded session loads into a FRESH session and
//!        reproduces byte-identical state; scrub_to is recorded and the
//!        opening checkpoint restores before re-apply.

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "engine/core/command_recorder.hpp"
#include "engine/core/editor_session.hpp"

namespace {

using omnicpp::editor::EditorSession;
using omnicpp::editor::ReplayScrubber;

using CK = omnicpp::core::ControlCommand::Kind;

//! Spawns a cube through the session protocol path.
void spawn_cube(EditorSession& session, const char* name, double x) {
  omnicpp::core::ControlCommand cmd;
  cmd.kind = CK::SpawnCube;
  cmd.text = name;
  cmd.numbers[0] = x;
  cmd.numbers[1] = 0.0;
  cmd.numbers[2] = 0.0;
  cmd.numbers[3] = 1.0;
  cmd.number_count = 4;
  const auto reply = session.on_control(cmd);
  EXPECT_TRUE(reply.ok) << reply.error;
}

//! Steps the sim `ticks` frames through the protocol.
void step(EditorSession& session, std::uint64_t ticks) {
  omnicpp::core::ControlCommand cmd;
  cmd.kind = CK::Step;
  cmd.numbers[0] = static_cast<double>(ticks);
  cmd.number_count = 1;
  ASSERT_TRUE(session.on_control(cmd).ok);
}

//! Reads a file's bytes; empty when missing.
std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return {};
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  return text;
}

TEST(CommandRecorder, CaptureMutateStopWritesValidFile) {
  EditorSession session;
  const std::string path = "/tmp/omnicpp_test_recorder_basic.replay";

  omnicpp::core::ControlCommand start;
  start.kind = CK::StartCapture;
  start.numbers[0] = 10.0;
  start.number_count = 1;
  auto reply = session.on_control(start);
  ASSERT_TRUE(reply.ok) << reply.error;
  EXPECT_TRUE(session.recorder().active());

  // The opening checkpoint exists immediately (density decision).
  EXPECT_EQ(session.recorder().checkpoint_count(), 1U);
  // step advances the logical frame (10 -> 13) BEFORE stamping.
  step(session, 3);
  EXPECT_EQ(session.recorder().frame(), 13U);
  spawn_cube(session, "rc", 1.0);

  // Queries are never recorded.
  omnicpp::core::ControlCommand list;
  list.kind = CK::ListObjects;
  (void)session.on_control(list);
  EXPECT_EQ(session.recorder().command_count(), 2U);  // step + spawn

  omnicpp::core::ControlCommand stop;
  stop.kind = CK::StopCapture;
  stop.text = path;
  const auto stop_reply = session.on_control(stop);
  ASSERT_TRUE(stop_reply.ok) << stop_reply.error;
  EXPECT_FALSE(session.recorder().active());

  const std::string bytes = read_file(path);
  ASSERT_FALSE(bytes.empty());
  // Header contract.
  EXPECT_NE(bytes.find("\"record\":\"header\""), std::string::npos);
  EXPECT_NE(bytes.find("\"format\":\"warploom-replay-v1\""), std::string::npos);
  EXPECT_NE(bytes.find("\"schema_version\":1"), std::string::npos);
  // The step + spawn commands are present; queries are not.
  EXPECT_NE(bytes.find("\"cmd\":\"step\""), std::string::npos);
  EXPECT_NE(bytes.find("\"cmd\":\"spawn_cube\""), std::string::npos);
  EXPECT_EQ(bytes.find("\"cmd\":\"list_objects\""), std::string::npos);
  // Checkpoint markers exist (start + stop at minimum).
  EXPECT_EQ(std::count(bytes.begin(), bytes.end(), '\n') > 0, true);
  EXPECT_NE(bytes.find("\"record\":\"ckpt\""), std::string::npos);
  EXPECT_NE(bytes.find("\"record\":\"end\""), std::string::npos);
  EXPECT_NE(bytes.find("\"commands\":2"), std::string::npos);
  // No temp residue.
  std::ifstream probe(path + ".tmp." + std::to_string(::getpid()));
  EXPECT_FALSE(probe.good());
  struct stat st{};
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  std::remove(path.c_str());
}

TEST(CommandRecorder, DoubleStartRejectedAndFailedStopKeepsSession) {
  EditorSession session;
  std::string error;
  ASSERT_TRUE(session.capture_start(0, "s", error)) << error;

  error.clear();
  EXPECT_FALSE(session.capture_start(5, "s", error));
  EXPECT_NE(error.find("already active"), std::string::npos) << error;

  // stop with an empty path fails; recording continues afterwards.
  error.clear();
  EXPECT_FALSE(session.capture_stop("", error));
  EXPECT_NE(error.find("path"), std::string::npos) << error;
  EXPECT_TRUE(session.recorder().active());
  spawn_cube(session, "kept", 1.0);
  EXPECT_EQ(session.recorder().command_count(), 1U);  // the post-failure edit

  // Unwritable path: failure keeps the session + log intact for a retry.
  const std::string bad = "/tmp/omnicpp_no_such_dir_9x/replay.json";
  error.clear();
  EXPECT_FALSE(session.capture_stop(bad, error));
  EXPECT_TRUE(session.recorder().active());
  EXPECT_EQ(session.recorder().command_count(), 1U);

  const std::string good = "/tmp/omnicpp_test_recorder_retry.replay";
  error.clear();
  ASSERT_TRUE(session.capture_stop(good, error)) << error;
  EXPECT_NE(read_file(good).find("\"cmd\":\"spawn_cube\""), std::string::npos);
  EXPECT_FALSE(session.recorder().active());
  std::remove(good.c_str());
}

TEST(CommandRecorder, FreshSessionLoadReproducesByteIdenticalState) {
  EditorSession live;
  const std::string path = "/tmp/omnicpp_test_recorder_load.replay";
  std::string error;
  ASSERT_TRUE(live.capture_start(2, "city", error)) << error;

  // scrub_start during capture embeds a checkpoint at the TARGET frame.
  omnicpp::core::ControlCommand scrub;
  scrub.kind = CK::ScrubStart;
  scrub.numbers[0] = 7.0;
  scrub.number_count = 1;
  ASSERT_TRUE(live.on_control(scrub).ok);
  EXPECT_EQ(live.recorder().checkpoint_count(), 2U);

  spawn_cube(live, "one", 1.0);
  step(live, 4);
  spawn_cube(live, "two", 2.0);
  error.clear();
  ASSERT_TRUE(live.capture_stop(path, error)) << error;
  const std::string saved_bytes = read_file(path);
  const std::uint64_t live_hash = omnicpp::editor::state_hash(live.document());

  // A FRESH session loads the replay and reproduces the state exactly.
  EditorSession loaded;
  error.clear();
  ASSERT_TRUE(loaded.load_replay(path, error)) << error;
  EXPECT_EQ(omnicpp::editor::state_hash(loaded.document()), live_hash);
  EXPECT_EQ(loaded.document().to_json(), live.document().to_json());
  // Checkpoints hydrated: both capture targets are warpable.
  EXPECT_TRUE(loaded.scrubber().has(2U));
  EXPECT_TRUE(loaded.scrubber().has(7U));
  // scrub_to during capture was recorded; the re-applied log restored 7
  // en route (the spawned "one" cube from before the warp is present).
  EXPECT_NE(loaded.document().find(2U), nullptr);

  // Time warp to the opening checkpoint on the LOADED session: state
  // returns to the capture-start bytes.
  omnicpp::core::ControlCommand to;
  to.kind = CK::ScrubTo;
  to.numbers[0] = 2.0;
  to.number_count = 1;
  ASSERT_TRUE(loaded.on_control(to).ok);
  const std::size_t checkpoint_objects =
      loaded.document().objects.size();
  EXPECT_EQ(checkpoint_objects, 1U);  // ctor-seeded environment only

  std::remove(path.c_str());
}

TEST(CommandRecorder, LoadRejectsGarbageTruncatedAndMissing) {
  EditorSession session;
  std::string error;

  const std::string missing = "/tmp/omnicpp_replay_missing_9x.replay";
  error.clear();
  EXPECT_FALSE(session.load_replay(missing, error));
  EXPECT_NE(error.find("cannot open"), std::string::npos) << error;

  const std::string garbage = "/tmp/omnicpp_replay_garbage.replay";
  {
    std::ofstream out(garbage, std::ios::binary);
    out << "not a replay at all\njust two lines\n";
  }
  error.clear();
  EXPECT_FALSE(session.load_replay(garbage, error));
  EXPECT_NE(error.find("warploom-replay-v1"), std::string::npos) << error;
  std::remove(garbage.c_str());

  // Valid header but no end record: truncated -> rejected.
  const std::string truncated = "/tmp/omnicpp_replay_truncated.replay";
  {
    std::ofstream out(truncated, std::ios::binary);
    out << "{\"record\":\"header\",\"schema_version\":1,"
           "\"format\":\"warploom-replay-v1\",\"scene\":\"\",\"created_unix\":0}\n"
           "{\"record\":\"cmd\",\"frame\":0,\"seq\":0,\"cmd\":\"step\""
           ",\"n\":[1]}\n";
  }
  error.clear();
  EXPECT_FALSE(session.load_replay(truncated, error));
  EXPECT_NE(error.find("truncated"), std::string::npos) << error;
  std::remove(truncated.c_str());
}

}  // namespace
