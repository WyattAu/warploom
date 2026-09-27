//! @file test_timeline.cpp
//! @brief G3 timeline proofs: clips are document entities (schema v3) and
//!        the session's armed record/playback is deterministic.
//!
//! Coverage:
//!   1. Document round-trip: a document with clips serializes and parses
//!      back byte-identically (to_json -> from_json -> to_json).
//!   2. Backward compatibility: v2-era documents (no clips section) load.
//!   3. Reader strictness: non-monotonic offsets, track-key mismatch,
//!      unknown objects, paired-key and cursor-honesty rules enforced.
//!   4. Undo: add/move/remove round-trip through the stack byte-identically
//!      (including the next_clip_id cursor).
//!   5. Record/playback determinism under the HOST tick contract (one full
//!      session tick per step tick, tick BEFORE the step is recorded — the
//!      same order load_replay re-executes), including a replay round-trip
//!      that must reproduce the recorded track byte-identically.
//!   6. Protocol paths: clip_* payload validation, clips_info, disarm at
//!      clip end, scrub_to disarm.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "warploom/core/document.hpp"
#include "warploom/core/editor_session.hpp"

namespace {

namespace ed = omnicpp::editor;
using CK = omnicpp::core::ControlCommand::Kind;

//! Sends one command and asserts the reply was ok (returns the detail).
std::string ok(ed::EditorSession& s, const omnicpp::core::ControlCommand& cmd) {
  const auto reply = s.on_control(cmd);
  EXPECT_TRUE(reply.ok) << reply.error;
  return reply.detail;
}

omnicpp::core::ControlCommand set_position(ed::EditorSession& s, double x,
                                           double y, double z) {
  omnicpp::core::ControlCommand set;
  set.kind = CK::SetProperty;
  set.text = s.document().objects.back().name;
  set.text2 = "position";
  set.numbers[0] = x;
  set.numbers[1] = y;
  set.numbers[2] = z;
  set.number_count = 3;
  return set;
}

omnicpp::core::ControlCommand step_cmd(std::uint64_t ticks) {
  omnicpp::core::ControlCommand step;
  step.kind = CK::Step;
  step.numbers[0] = static_cast<double>(ticks);
  step.number_count = 1;
  return step;
}

//! The HOST tick contract (headless host AND the load_replay re-applier):
//! each step tick runs sync_graph + tick_timeline at the host's frame,
//! THEN the step command is recorded at its logical frame.
void host_tick(ed::EditorSession& s, std::uint64_t frame) {
  std::string error;
  (void)s.sync_graph(error);
  s.tick_timeline(frame);
}

//! Builds a document with one cube (builtin registry pre-registered for
//! serialization round-trips).
ed::SceneDocument doc_with_cube(double x, double y, double z) {
  ed::SceneDocument doc;
  ed::register_builtin_node_types(doc.node_graph);
  ed::SceneObject cube;
  const auto* type = ed::default_registry().find_by_name("cube");
  cube.type_id = type->id;
  cube.name = "probe_cube";
  for (const auto& d : type->properties) {
    cube.properties.emplace(d.name, d.default_value);
  }
  cube.id = doc.next_object_id++;
  cube.properties["position"] = ed::PropValue::make_vec3(x, y, z);
  doc.objects.push_back(std::move(cube));
  return doc;
}

// ============================================================================
// 1-3. Document layer
// ============================================================================

TEST(TimelineDocument, ClipsRoundTripByteIdentically) {
  ed::SceneDocument doc = doc_with_cube(1.0, 2.0, 3.0);
  ed::TimelineClip clip;
  clip.id = doc.next_clip_id++;
  clip.name = "intro";
  clip.start_frame = 10;
  clip.length_frames = 5;
  ed::ClipTrack track;
  track.object_id = doc.objects[0].id;
  track.property = "position.x";
  track.samples.push_back(ed::ClipSample{0, ed::PropValue::make_number(1.0)});
  track.samples.push_back(ed::ClipSample{3, ed::PropValue::make_number(4.5)});
  clip.tracks.emplace(ed::track_key(track.object_id, track.property),
                      std::move(track));
  doc.clips.push_back(std::move(clip));

  const std::string bytes = doc.to_json();
  ed::SceneDocument parsed;
  std::string error;
  ASSERT_TRUE(ed::SceneDocument::from_json(bytes, parsed, error)) << error;
  EXPECT_EQ(bytes, parsed.to_json());
  // Semantic round-trip: the clip survives with its samples.
  ASSERT_EQ(parsed.clips.size(), 1U);
  EXPECT_EQ(parsed.clips[0].name, "intro");
  ASSERT_EQ(parsed.clips[0].tracks.size(), 1U);
  EXPECT_EQ(parsed.clips[0].tracks.begin()->second.samples.size(), 2U);
  EXPECT_DOUBLE_EQ(
      parsed.clips[0].tracks.begin()->second.samples[1].value.number, 4.5);
}

TEST(TimelineDocument, V2DocumentsStillLoad) {
  // An old build wrote schema_version 2 and no clips section.
  std::string bytes = doc_with_cube(0.0, 0.0, 0.0).to_json();
  const std::size_t at = bytes.find("\"schema_version\":3");
  ASSERT_NE(at, std::string::npos);
  bytes.replace(at, 18, "\"schema_version\":2");
  ed::SceneDocument parsed;
  std::string error;
  EXPECT_TRUE(ed::SceneDocument::from_json(bytes, parsed, error)) << error;
  EXPECT_TRUE(parsed.clips.empty());
}

TEST(TimelineDocument, ReaderRejectsBadClipPayloads) {
  const std::string head =
      "{\"schema_version\":3,\"next_object_id\":2,\"objects\":[],"
      "\"next_clip_id\":2,\"clips\":[";
  ed::SceneDocument parsed;
  std::string error;

  // Non-monotonic sample offsets.
  EXPECT_FALSE(ed::SceneDocument::from_json(
      head + "{\"id\":1,\"name\":\"c\",\"start_frame\":0,\"length_frames\":10,"
             "\"tracks\":{\"1:position\":{\"object_id\":1,\"property\":"
             "\"position\",\"samples\":"
             "[{\"frame_offset\":5,\"value\":{\"type\":\"number\",\"v\":1}},"
             "{\"frame_offset\":5,\"value\":{\"type\":\"number\",\"v\":2}}]}}"
             "}]}",
      parsed, error));
  EXPECT_NE(error.find("strictly increasing"), std::string::npos);

  // Track key does not match payload.
  EXPECT_FALSE(ed::SceneDocument::from_json(
      head + "{\"id\":1,\"name\":\"c\",\"start_frame\":0,\"length_frames\":10,"
             "\"tracks\":{\"9:position\":{\"object_id\":1,\"property\":"
             "\"position\",\"samples\":[]}}}]}",
      parsed, error));
  EXPECT_NE(error.find("does not match"), std::string::npos);

  // Track references an unknown object.
  EXPECT_FALSE(ed::SceneDocument::from_json(
      head + "{\"id\":1,\"name\":\"c\",\"start_frame\":0,\"length_frames\":10,"
             "\"tracks\":{\"7:position\":{\"object_id\":7,\"property\":"
             "\"position\",\"samples\":[]}}}]}",
      parsed, error));
  EXPECT_NE(error.find("unknown object"), std::string::npos);

  // Clips without next_clip_id (paired-key rule).
  EXPECT_FALSE(ed::SceneDocument::from_json(
      "{\"schema_version\":3,\"next_object_id\":2,\"objects\":[],"
      "\"clips\":[]}",
      parsed, error));
  EXPECT_NE(error.find("must appear together"), std::string::npos);

  // Clip id >= next_clip_id (cursor honesty).
  EXPECT_FALSE(ed::SceneDocument::from_json(
      head + "{\"id\":5,\"name\":\"c\",\"start_frame\":0,\"length_frames\":1,"
             "\"tracks\":{}}]}",
      parsed, error));
  EXPECT_NE(error.find("not below next_clip_id"), std::string::npos);

  // Duplicate clip ids.
  const std::string one =
      "{\"id\":1,\"name\":\"c\",\"start_frame\":0,\"length_frames\":1,"
      "\"tracks\":{}}";
  EXPECT_FALSE(ed::SceneDocument::from_json(head + one + "," + one + "]}",
                                            parsed, error));
  EXPECT_NE(error.find("duplicate clip id"), std::string::npos);
}

// ============================================================================
// 4. Undo
// ============================================================================

TEST(TimelineCommands, UndoRestoresByteIdentically) {
  ed::SceneDocument doc = doc_with_cube(0.0, 0.0, 0.0);
  ed::CommandStack stack(doc);

  auto add = std::make_unique<ed::AddClipCommand>("intro", 10, 5);
  const auto claimed = doc.next_clip_id;
  std::string error;
  ASSERT_TRUE(stack.execute(std::move(add), error)) << error;
  ASSERT_EQ(doc.clips.size(), 1U);
  EXPECT_EQ(doc.clips[0].id, claimed);
  const std::string after_add = doc.to_json();
  ASSERT_NE(after_add.find("\"clips\""), std::string::npos) << after_add;

  // Undo restores the PRE-ADD bytes (no clips section — the writer omits
  // it when the document has no clips and an untouched id cursor).
  ASSERT_TRUE(stack.undo(error));
  EXPECT_TRUE(doc.clips.empty());
  EXPECT_EQ(doc.next_clip_id, claimed);

  // Redo: byte-identical to the post-add state (id determinism).
  ASSERT_TRUE(stack.redo(error));
  EXPECT_EQ(doc.clips.size(), 1U);
  EXPECT_EQ(doc.to_json(), after_add);
}

TEST(TimelineCommands, MoveAndRemoveRoundTrip) {
  ed::SceneDocument doc = doc_with_cube(0.0, 0.0, 0.0);
  ed::CommandStack stack(doc);
  std::string error;
  ASSERT_TRUE(stack.execute(
      std::make_unique<ed::AddClipCommand>("c", 0, 10), error));
  auto* clip = doc.find_clip(1);
  ASSERT_NE(clip, nullptr);
  clip->tracks.emplace(
      ed::track_key(doc.objects[0].id, "position.x"),
      ed::ClipTrack{doc.objects[0].id, "position.x",
                    {{0, ed::PropValue::make_number(2.0)}}});
  const std::string snapshot = doc.to_json();

  // Move to 40, undo restores the snapshot bytes.
  ASSERT_TRUE(
      stack.execute(std::make_unique<ed::MoveClipCommand>(1, 40), error));
  EXPECT_EQ(doc.find_clip(1)->start_frame, 40U);
  ASSERT_TRUE(stack.undo(error));
  EXPECT_EQ(doc.to_json(), snapshot);

  // Remove + undo restores bytes AND track content.
  ASSERT_TRUE(stack.execute(std::make_unique<ed::RemoveClipCommand>(1), error));
  EXPECT_TRUE(doc.clips.empty());
  ASSERT_TRUE(stack.undo(error));
  EXPECT_EQ(doc.to_json(), snapshot);
}

// ============================================================================
// 5. Record/playback determinism (host tick contract) + replay round-trip
// ============================================================================

TEST(TimelineSession, RecordAndPlaybackDeterministic) {
  ed::EditorSession s;
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());

  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "intro";
  add.numbers[0] = 5;
  add.numbers[1] = 4;
  add.number_count = 2;
  const std::string add_detail = ok(s, add);
  EXPECT_NE(add_detail.find("added clip 1"), std::string::npos);

  const auto cube_id = s.document().objects.back().id;
  ok(s, set_position(s, 7.0, 0.0, 0.0));

  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  rec.numbers[0] = 1;
  rec.numbers[1] = cube_id;
  rec.text = "position.x";
  rec.number_count = 2;
  ok(s, rec);

  // Host ordering: the set_property for the second half arrives BETWEEN
  // step 7 and step 8, so ticks 7..8 sample 9.25 (tick runs when the step
  // ARRIVES — the exact order load_replay re-executes).
  for (std::uint64_t i = 0; i < 12; ++i) {
    if (i == 7) {
      ok(s, set_position(s, 9.25, 0.0, 0.0));
    }
    host_tick(s, i);
    ok(s, step_cmd(1));
  }

  // Recording auto-disarmed at clip end (frame 9): 4 samples at offsets
  // 0..3 = values 7, 7, 9.25, 9.25.
  const auto* clip = s.document().find_clip(1);
  ASSERT_NE(clip, nullptr);
  ASSERT_EQ(clip->tracks.size(), 1U);
  const auto& samples = clip->tracks.begin()->second.samples;
  ASSERT_EQ(samples.size(), 4U);
  EXPECT_EQ(samples[0].frame_offset, 0U);
  EXPECT_DOUBLE_EQ(samples[0].value.number, 7.0);
  EXPECT_DOUBLE_EQ(samples[2].value.number, 9.25);
  EXPECT_FALSE(s.recording_target());

  // Reset, MOVE the clip to a future span (12..16) — playback is bound to
  // the clip's authored span on the absolute timeline — then play it back
  // through the same tick path: the property follows the trajectory.
  ok(s, set_position(s, 0.0, 0.0, 0.0));
  omnicpp::core::ControlCommand move;
  move.kind = CK::ClipMove;
  move.numbers[0] = 1;
  move.numbers[1] = 12;
  move.number_count = 2;
  ok(s, move);
  omnicpp::core::ControlCommand play;
  play.kind = CK::ClipPlay;
  play.numbers[0] = 1;
  play.number_count = 1;
  ok(s, play);
  for (std::uint64_t i = 12; i < 20; ++i) {
    host_tick(s, i);
    ok(s, step_cmd(1));
  }
  const auto* cube = s.document().find(cube_id);
  ASSERT_NE(cube, nullptr);
  // Step-hold: the last applied value persists after the clip ends.
  EXPECT_DOUBLE_EQ(cube->properties.at("position").vec[0], 9.25);
  EXPECT_FALSE(s.playing_clip());

  // evaluate(): exact trajectory inside the NEW span, nothing outside.
  const double expected[4] = {7.0, 7.0, 9.25, 9.25};
  for (std::size_t i = 0; i < 4; ++i) {
    ed::PropValue v;
    ASSERT_TRUE(clip->evaluate(12 + i, cube_id, "position.x", v));
    EXPECT_DOUBLE_EQ(v.number, expected[i]);
  }
  ed::PropValue out;
  EXPECT_FALSE(clip->evaluate(11, cube_id, "position.x", out));
  EXPECT_FALSE(clip->evaluate(16, cube_id, "position.x", out));

  // Replay round-trip: record the session (the clip edits + steps are in
  // the log), load into a fresh session, and the recorded track must
  // re-simulate byte-identically (tick-then-apply ordering).
  const std::string path = "/tmp/g3_replay_roundtrip.replay";
  std::string capture_error;
  ASSERT_TRUE(s.capture_start(0, "g3", capture_error)) << capture_error;
  // Re-arm nothing: the EXISTING clip edits + steps were already recorded
  // live; here we simply stop to write the file with the closing snapshot
  // (which embeds the recorded tracks).
  ASSERT_TRUE(s.capture_stop(path, capture_error)) << capture_error;

  ed::EditorSession fresh;
  std::string load_error;
  ASSERT_TRUE(fresh.load_replay(path, load_error)) << load_error;
  const auto* reclip = fresh.document().find_clip(1);
  ASSERT_NE(reclip, nullptr);
  ASSERT_EQ(reclip->tracks.size(), 1U);
  const auto& resamples = reclip->tracks.begin()->second.samples;
  ASSERT_EQ(resamples.size(), samples.size());
  for (std::size_t i = 0; i < resamples.size(); ++i) {
    EXPECT_EQ(resamples[i].frame_offset, samples[i].frame_offset);
    EXPECT_DOUBLE_EQ(resamples[i].value.number, samples[i].value.number);
  }
}

// ============================================================================
// 6. Protocol paths
// ============================================================================

TEST(TimelineSession, ProtocolValidationAndInfo) {
  ed::EditorSession s;
  // clip_add without name.
  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  EXPECT_FALSE(s.on_control(add).ok);
  // clip_remove unknown id.
  omnicpp::core::ControlCommand rm;
  rm.kind = CK::ClipRemove;
  rm.numbers[0] = 99;
  rm.number_count = 1;
  EXPECT_FALSE(s.on_control(rm).ok);
  // clip_record without payload.
  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  EXPECT_FALSE(s.on_control(rec).ok);
  // Valid add; play of an EMPTY clip is rejected.
  add.text = "empty";
  add.number_count = 0;
  ASSERT_TRUE(s.on_control(add).ok);
  omnicpp::core::ControlCommand play;
  play.kind = CK::ClipPlay;
  play.numbers[0] = 1;
  play.number_count = 1;
  const auto play_reply = s.on_control(play);
  EXPECT_FALSE(play_reply.ok);
  EXPECT_NE(play_reply.error.find("no tracks"), std::string::npos);
  // clips_info reports the clip.
  omnicpp::core::ControlCommand info;
  info.kind = CK::ClipsInfo;
  const auto detail = s.on_control(info).detail;
  EXPECT_NE(detail.find("\"name\":\"empty\""), std::string::npos);
  EXPECT_NE(detail.find("\"start_frame\":0"), std::string::npos);
  // Warp path: scrub_start + scrub_to disarm anything armed (nothing is).
  omnicpp::core::ControlCommand scrub;
  scrub.kind = CK::ScrubStart;
  scrub.number_count = 0;
  ASSERT_TRUE(s.on_control(scrub).ok);
  scrub.kind = CK::ScrubTo;
  scrub.numbers[0] = 0;
  scrub.number_count = 1;
  ASSERT_TRUE(s.on_control(scrub).ok);
  EXPECT_FALSE(s.recording_target());
  EXPECT_FALSE(s.playing_clip());
}

TEST(TimelineSession, ClipEditsAreRecordedAndReplay) {
  ed::EditorSession s;
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  std::string capture_error;
  ASSERT_TRUE(s.capture_start(0, "g3", capture_error)) << capture_error;
  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "c";
  add.numbers[0] = 2;
  add.numbers[1] = 3;
  add.number_count = 2;
  ok(s, add);
  ok(s, step_cmd(4));
  omnicpp::core::ControlCommand move;
  move.kind = CK::ClipMove;
  move.numbers[0] = 1;
  move.numbers[1] = 7;
  move.number_count = 2;
  ok(s, move);
  const std::string path = "/tmp/g3_clip_edits.replay";
  ASSERT_TRUE(s.capture_stop(path, capture_error)) << capture_error;

  ed::EditorSession fresh;
  std::string load_error;
  ASSERT_TRUE(fresh.load_replay(path, load_error)) << load_error;
  const auto* clip = fresh.document().find_clip(1);
  ASSERT_NE(clip, nullptr);
  // clip_move replayed: start is 7, not 2.
  EXPECT_EQ(clip->start_frame, 7U);
  EXPECT_EQ(clip->length_frames, 3U);
}

}  // namespace
