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

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
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
  rec.numbers[1] = static_cast<double>(cube_id);
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

// T4b: the sub-frame fraction is session intent, not presentation smoothing.
// The take below records a Step carrying frac=0.5 and plays a clip through
// it; load_replay must reproduce the interpolated value bit-for-bit. The
// file's closing-hash check makes this fail-closed: a fraction dropped at
// parse, record, file-write, or re-apply replays a different take and the
// load is rejected instead of silently diverging.
TEST(TimelineSession, SubFrameFractionRecordsAndReplaysDeterministically) {
  ed::EditorSession s;
  std::string error;
  ASSERT_TRUE(s.capture_start(0, "frac", error)) << error;
  static constexpr double kDt = 1.0 / 60.0;
  std::uint64_t tick_frame = 0;
  // Canonical host rhythm (mirrors LiveDriver AND the load_replay re-apply
  // loop): one tick per command arrival, then the command; N ticks for a
  // Step(N). A take driven any other way is unreplayable by construction —
  // the log fixes the tick count AND the tick frames, so the live frames
  // must be the same numbers. The unlogged-ticks test pins the refusal.
  auto tick_once = [&](double sub = 0.0) {
    (void)s.tick({tick_frame++, kDt, sub, false});
  };

  tick_once();
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  const auto cube_id = s.document().objects.back().id;
  tick_once();
  ok(s, set_position(s, 7.0, 0.0, 0.0));

  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "frac";
  add.numbers[0] = 4;  // start frame
  add.numbers[1] = 4;  // length
  add.number_count = 2;
  tick_once();
  ok(s, add);
  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  rec.numbers[0] = 1;
  rec.numbers[1] = static_cast<double>(cube_id);
  rec.text = "position.x";
  rec.number_count = 2;
  tick_once();
  ok(s, rec);
  // Record span is [4, 8): the four setup commands consumed ticks 0..3, so
  // the clip must cover the ticks that run while armed. The mid-take edit
  // arrives AFTER its tick (tick-then-command), so it lands in the next
  // sample: 7, 7, 7, 9.25 at offsets 0..3.
  for (std::uint64_t i = 0; i < 4; ++i) {
    tick_once();
    if (i == 2) {
      ok(s, set_position(s, 9.25, 0.0, 0.0));
    } else {
      ok(s, step_cmd(1));
    }
  }
  tick_once();
  ok(s, step_cmd(1));  // frame 8: past clip end, disarms, samples nothing

  // Move the recorded span to [10, 14) and play it back. Replay applies
  // the move after tick 9 and the play after tick 10, so playback ticks
  // are 11 (offset 1, holds 7.0) and 12.5 (offset 2.5, the interpolated
  // midpoint 8.125 with frac = 0.5). Same numbers live and on replay —
  // the frame source is the accumulated tick count, not the wall clock.
  omnicpp::core::ControlCommand move;
  move.kind = CK::ClipMove;
  move.numbers[0] = 1;
  move.numbers[1] = 10;
  move.number_count = 2;
  tick_once();
  ok(s, move);
  omnicpp::core::ControlCommand play;
  play.kind = CK::ClipPlay;
  play.numbers[0] = 1;
  play.number_count = 1;
  tick_once();
  ok(s, play);
  tick_once();
  ok(s, step_cmd(1));
  const auto* held = s.document().find(cube_id);
  ASSERT_NE(held, nullptr);
  EXPECT_DOUBLE_EQ(held->properties.at("position").vec[0], 7.0);
  (void)s.tick({tick_frame++, kDt, 0.5, false});
  omnicpp::core::ControlCommand frac_step;
  frac_step.kind = CK::Step;
  frac_step.numbers[0] = 1.0;
  frac_step.numbers[1] = 0.5;
  frac_step.number_count = 2;
  ok(s, frac_step);

  // Linear midpoint between the offset-2 (7.0) and offset-3 (9.25) samples.
  const auto* cube = s.document().find(cube_id);
  ASSERT_NE(cube, nullptr);
  EXPECT_DOUBLE_EQ(cube->properties.at("position").vec[0], 8.125);

  const std::string path = "/tmp/g3_frac_roundtrip.replay";
  ASSERT_TRUE(s.capture_stop(path, error)) << error;

  // The fraction reached the FILE, not just the live session: the step's
  // positional payload carries [ticks, frac]. (%.17g prints 0.5 exactly.)
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  EXPECT_NE(bytes.find("\"n\":[1,0.5]"), std::string::npos) << bytes;

  ed::EditorSession fresh;
  std::string load_error;
  ASSERT_TRUE(fresh.load_replay(path, load_error)) << load_error;
  const auto* recube = fresh.document().find(cube_id);
  ASSERT_NE(recube, nullptr);
  EXPECT_DOUBLE_EQ(recube->properties.at("position").vec[0], 8.125);
  // The recorded track itself re-simulated identically.
  const auto* live_clip = s.document().find_clip(1);
  const auto* re_clip = fresh.document().find_clip(1);
  ASSERT_NE(live_clip, nullptr);
  ASSERT_NE(re_clip, nullptr);
  ASSERT_EQ(live_clip->tracks.size(), 1U);
  ASSERT_EQ(re_clip->tracks.size(), 1U);
  const auto& live_samples = live_clip->tracks.begin()->second.samples;
  const auto& re_samples = re_clip->tracks.begin()->second.samples;
  ASSERT_EQ(re_samples.size(), live_samples.size());
  for (std::size_t i = 0; i < re_samples.size(); ++i) {
    EXPECT_EQ(re_samples[i].frame_offset, live_samples[i].frame_offset);
    EXPECT_DOUBLE_EQ(re_samples[i].value.number, live_samples[i].value.number);
  }
  std::remove(path.c_str());
}

// ============================================================================
// 5b. T5a: concurrent arms (record armed while play is armed)
// ============================================================================

// The tick order is the whole contract (docs/warploom-t5-tracks-plan.md §2):
// playback applies to the document, THEN recording samples the current
// value. Consequences pinned here:
//   - a record arm chained on the playing clip's track captures the PLAYED
//     values (sample-before-playback would capture the stale ones -- the
//     sabotage direction);
//   - arming order within a frame is irrelevant (both are state);
//   - clip-end disarm is per-arm: playback ending does not stop recording.
TEST(TimelineSession, ConcurrentRecordAndPlayArmsBoth) {
  ed::EditorSession s;
  auto tick = [&](std::uint64_t frame) { (void)s.tick({frame, 1.0 / 60.0, 0.0, false}); };
  tick(0);
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  const auto cube_id = s.document().objects.back().id;

  // Source clip [10, 14): record position.x = 5, 6, 7, 8 at offsets 0..3.
  // Samples land only inside the clip's authored span (frame >= start and
  // frame < start + length), so the ticks that sample are exactly 10..13.
  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "src";
  add.numbers[0] = 10;
  add.numbers[1] = 4;
  add.number_count = 2;
  tick(1);
  ok(s, add);
  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  rec.numbers[0] = 1;
  rec.numbers[1] = static_cast<double>(cube_id);
  rec.text = "position.x";
  rec.number_count = 2;
  tick(2);
  ok(s, rec);
  ok(s, set_position(s, 5.0, 0.0, 0.0));
  tick(10);
  ok(s, set_position(s, 6.0, 0.0, 0.0));
  tick(11);
  ok(s, set_position(s, 7.0, 0.0, 0.0));
  tick(12);
  ok(s, set_position(s, 8.0, 0.0, 0.0));
  tick(13);
  tick(14);  // past the span: recording auto-disarms, no sample
  const auto* src = s.document().find_clip(1);
  ASSERT_NE(src, nullptr);
  ASSERT_EQ(src->tracks.size(), 1U);
  const auto& src_samples = src->tracks.begin()->second.samples;
  ASSERT_EQ(src_samples.size(), 4U);
  EXPECT_DOUBLE_EQ(src_samples[0].value.number, 5.0);
  EXPECT_DOUBLE_EQ(src_samples[3].value.number, 8.0);

  // Destination clip [10, 16): armed for record WHILE src plays. Stale value
  // is 0.0, so any sample equal to 0.0 means recording ran first. Arms land
  // on pre-span ticks: arming on a tick past the clip's end would trip the
  // (correct) end-of-clip auto-disarm before the take loop even starts.
  ok(s, set_position(s, 0.0, 0.0, 0.0));
  omnicpp::core::ControlCommand add_dst;
  add_dst.kind = CK::ClipAdd;
  add_dst.text = "take";
  add_dst.numbers[0] = 10;
  add_dst.numbers[1] = 6;
  add_dst.number_count = 2;
  tick(7);
  ok(s, add_dst);
  omnicpp::core::ControlCommand play;
  play.kind = CK::ClipPlay;
  play.numbers[0] = 1;
  play.number_count = 1;
  tick(8);
  ok(s, play);
  omnicpp::core::ControlCommand rec_dst = rec;
  rec_dst.numbers[0] = 2;
  tick(9);
  ok(s, rec_dst);
  EXPECT_TRUE(s.recording_target() != nullptr);
  EXPECT_TRUE(s.playing_clip() != nullptr);

  // Frames 10..15: src plays 5,6,7,8 then ENDS at frame 14; the take keeps
  // recording (per-arm disarm) and step-holds 8 for offsets 4 and 5.
  for (std::uint64_t f = 10; f < 16; ++f) tick(f);
  const auto* take = s.document().find_clip(2);
  ASSERT_NE(take, nullptr);
  ASSERT_EQ(take->tracks.size(), 1U);
  const auto& take_samples = take->tracks.begin()->second.samples;
  ASSERT_EQ(take_samples.size(), 6U)
      << "recording stopped when playback ended (arms are not independent)";
  const double expected[6] = {5.0, 6.0, 7.0, 8.0, 8.0, 8.0};
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(take_samples[i].frame_offset, i) << "offset " << i;
    EXPECT_DOUBLE_EQ(take_samples[i].value.number, expected[i])
        << "sample " << i << ": recording sampled before playback applied "
        << "(tick order reversed)";
  }
  EXPECT_FALSE(s.playing_clip()) << "playback did not auto-disarm at clip end";
  EXPECT_TRUE(s.recording_target()) << "recording disarmed before its own end";

  // Same-track self-chain: play AND record the take itself. Every sample
  // re-records the value playback just wrote, so the track must be
  // unchanged (no duplicate offsets, no drift). T5d: the phase-2 arm is
  // still live (take span [10,16) was never left), so it is stopped first
  // — re-arming an identical track is rejected, not duplicated.
  ok(s, set_position(s, 0.0, 0.0, 0.0));
  omnicpp::core::ControlCommand stop;
  stop.kind = CK::ClipRecordStop;
  stop.number_count = 0;
  omnicpp::core::ControlCommand rec_take = rec_dst;
  tick(18);
  ok(s, stop);
  ok(s, rec_take);
  omnicpp::core::ControlCommand play_take;
  play_take.kind = CK::ClipPlay;
  play_take.numbers[0] = 2;
  play_take.number_count = 1;
  tick(19);
  ok(s, play_take);
  for (std::uint64_t f = 10; f < 16; ++f) tick(f);
  const auto* again = s.document().find_clip(2);
  ASSERT_NE(again, nullptr);
  const auto& again_samples = again->tracks.begin()->second.samples;
  ASSERT_EQ(again_samples.size(), 6U) << "self-chained record appended samples";
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(again_samples[i].frame_offset, take_samples[i].frame_offset);
    EXPECT_DOUBLE_EQ(again_samples[i].value.number, expected[i])
        << "self-chain diverged at sample " << i;
  }
}

// The same two-arm run must replay: capture_start, arm both, tick, then
// load_replay in a fresh session and compare the recorded take
// byte-for-byte. A tick-order change or an unrecorded arm would either
// fail the closing hash or produce a different track.
//
// Replay mechanics constrain the shape: load_replay re-ticks once per
// logged entry at replay frames 0,1,2... (Step entries tick N times), so
// the live run must be monotonic 1:1 — every live tick paired with exactly
// one step_cmd(1), capture at frame 0, live absolute frames counting from
// 0 (the host_tick+step_cmd pattern). A rewind to re-play a span is
// unrepresentable in the log, so the spans are staged sequentially: src
// records first (then its arm stops, keeping exactly 4 samples), and the
// take records src's step-held playback afterwards.
TEST(TimelineSession, ConcurrentArmsSurviveReplay) {
  ed::EditorSession s;
  std::string error;
  ASSERT_TRUE(s.capture_start(0, "t5a", error)) << error;
  static constexpr double kDt = 1.0 / 60.0;
  std::uint64_t live_frame = 0;
  // Strict tick-then-command alternation: every live tick is followed by
  // exactly one logged command, which is exactly the order load_replay
  // re-executes (one tick per entry, then the entry). A set therefore
  // lands between the same two ticks live and on replay, so sampled
  // values agree. Any other shape (rewound frames, unticked commands,
  // unlogged ticks) is unreplayable by construction.
  auto ring = [&](omnicpp::core::ControlCommand cmd) {
    (void)s.tick({live_frame++, kDt, 0.0, false});
    ok(s, cmd);
  };
  auto spawn_cube = [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  };

  ring(spawn_cube());
  const auto cube_id = s.document().objects.back().id;
  ring(set_position(s, 5.0, 0.0, 0.0));
  // Source clip [10, 20): long enough to stay alive through the take loop;
  // its arm stops after offset 3 so it keeps exactly {5,6,7,8}.
  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "src";
  add.numbers[0] = 10;
  add.numbers[1] = 10;
  add.number_count = 2;
  ring(add);
  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  rec.numbers[0] = 1;
  rec.numbers[1] = static_cast<double>(cube_id);
  rec.text = "position.x";
  rec.number_count = 2;
  ring(rec);
  // Frames 4..9: nothing in span, no samples.
  for (int i = 0; i < 6; ++i) ring(step_cmd(1));
  // Ticks sample offsets 0..3 = 5, 6, 7, 8: each set lands after the tick
  // that must see the old value (tick-then-command on both sides).
  ring(set_position(s, 6.0, 0.0, 0.0));  // tick@10 samples 5.0
  ring(set_position(s, 7.0, 0.0, 0.0));  // tick@11 samples 6.0
  ring(set_position(s, 8.0, 0.0, 0.0));  // tick@12 samples 7.0
  // Stop the src arm: offsets 4+ would otherwise keep sampling 8s.
  omnicpp::core::ControlCommand stop;
  stop.kind = CK::ClipRecordStop;
  stop.number_count = 0;
  ring(stop);  // tick@13 samples 8.0, then disarms
  const auto* live_src = s.document().find_clip(1);
  ASSERT_NE(live_src, nullptr);
  ASSERT_EQ(live_src->tracks.begin()->second.samples.size(), 4U);
  ring(set_position(s, 0.0, 0.0, 0.0));  // tick@14
  // Take clip [18, 26): armed for record WHILE src plays (src stays alive
  // through frame 19). Playback step-holds 8 past src's last sample, so
  // the take captures 8s; the take outlives src's end, which pins
  // per-arm disarm (recording continues after playback ends).
  omnicpp::core::ControlCommand add_dst;
  add_dst.kind = CK::ClipAdd;
  add_dst.text = "take";
  add_dst.numbers[0] = 18;
  add_dst.numbers[1] = 8;
  add_dst.number_count = 2;
  ring(add_dst);  // tick@15
  // Arm both, one after the other: playback then record, every tick.
  omnicpp::core::ControlCommand play;
  play.kind = CK::ClipPlay;
  play.numbers[0] = 1;
  play.number_count = 1;
  ring(play);  // tick@16
  omnicpp::core::ControlCommand rec_dst = rec;
  rec_dst.numbers[0] = 2;
  ring(rec_dst);  // tick@17: play applies 8.0, take not yet armed
  // Frames 18..25 give 8 samples, all step-held 8 (play dies at 20).
  for (int i = 0; i < 8; ++i) ring(step_cmd(1));
  // Per-arm disarm: playback ended at frame 20, recording ran to 25.
  EXPECT_FALSE(s.playing_clip());
  EXPECT_TRUE(s.recording_target());

  const auto* live_take = s.document().find_clip(2);
  ASSERT_NE(live_take, nullptr);
  ASSERT_EQ(live_take->tracks.size(), 1U);
  const std::vector<ed::ClipSample> live_samples =
      live_take->tracks.begin()->second.samples;
  ASSERT_EQ(live_samples.size(), 8U);
  for (std::size_t i = 0; i < 8U; ++i) {
    EXPECT_EQ(live_samples[i].frame_offset, i);
    EXPECT_DOUBLE_EQ(live_samples[i].value.number, 8.0);
  }
  const std::string path = "/tmp/t5a_concurrent_arms.replay";
  ASSERT_TRUE(s.capture_stop(path, error)) << error;

  ed::EditorSession fresh;
  std::string load_error;
  ASSERT_TRUE(fresh.load_replay(path, load_error)) << load_error;
  const auto* re_take = fresh.document().find_clip(2);
  ASSERT_NE(re_take, nullptr);
  ASSERT_EQ(re_take->tracks.size(), 1U);
  const auto& re_samples = re_take->tracks.begin()->second.samples;
  ASSERT_EQ(re_samples.size(), live_samples.size());
  for (std::size_t i = 0; i < re_samples.size(); ++i) {
    EXPECT_EQ(re_samples[i].frame_offset, live_samples[i].frame_offset);
    EXPECT_DOUBLE_EQ(re_samples[i].value.number, live_samples[i].value.number);
  }
  const auto* re_cube = fresh.document().find(cube_id);
  ASSERT_NE(re_cube, nullptr);
  EXPECT_DOUBLE_EQ(
      re_cube->properties.at("position").vec[0],
      s.document().find(cube_id)->properties.at("position").vec[0]);
  std::remove(path.c_str());
}

// ============================================================================
// 5c. T5b: atomic multi-property record (clip_record_all, protocol v1.15)
// ============================================================================

// One arm records a camera object's eye/target (vec3) and fov (number)
// across a clip: 3 + 3 + 1 + 3 (sun_direction) = 10 tracks. Playback
// reproduces the pose bit-identically and the take survives save/load
// byte-identically (acceptance 2).
omnicpp::core::ControlCommand set_camera_cmd(double ex, double ey, double ez,
                                             double tx, double ty, double tz,
                                             double fov) {
  omnicpp::core::ControlCommand c;
  c.kind = CK::SetCamera;
  c.numbers[0] = ex;
  c.numbers[1] = ey;
  c.numbers[2] = ez;
  c.numbers[3] = tx;
  c.numbers[4] = ty;
  c.numbers[5] = tz;
  c.numbers[6] = fov;
  c.number_count = 7;
  return c;
}

TEST(TimelineSession, RecordAllCapturesCameraPoseAtomically) {
  ed::EditorSession s;
  auto tick = [&](std::uint64_t frame) { (void)s.tick({frame, 1.0 / 60.0, 0.0, false}); };
  static constexpr std::uint64_t kEnv = 1;  // seeded environment object
  ASSERT_NE(s.document().find(kEnv), nullptr);

  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "cam";
  add.numbers[0] = 10;
  add.numbers[1] = 4;
  add.number_count = 2;
  tick(0);
  ok(s, add);
  omnicpp::core::ControlCommand all;
  all.kind = CK::ClipRecordAll;
  all.numbers[0] = 1;
  all.numbers[1] = static_cast<double>(kEnv);
  all.number_count = 2;
  tick(1);
  const auto all_reply = s.on_control(all);
  ASSERT_TRUE(all_reply.ok) << all_reply.error;
  EXPECT_NE(all_reply.detail.find("10 tracks"), std::string::npos)
      << all_reply.detail;
  EXPECT_EQ(s.record_arm_count(), 10U);
  // Atomic duplicate: the whole command rejects when any track is armed.
  EXPECT_FALSE(s.on_control(all).ok);
  EXPECT_EQ(s.record_arm_count(), 10U);

  // Drive the pose: eye.x sweeps 10..13, fov 50..53, the rest held.
  for (std::uint64_t f = 10; f < 14; ++f) {
    ok(s, set_camera_cmd(static_cast<double>(f), 3.0, 8.0, 0.0, 1.0, 0.0,
                         50.0 + static_cast<double>(f)));
    tick(f);
  }
  tick(14);  // past the end: per-arm disarm, no sample
  EXPECT_EQ(s.record_arm_count(), 0U);
  const auto* cam = s.document().find_clip(1);
  ASSERT_NE(cam, nullptr);
  ASSERT_EQ(cam->tracks.size(), 10U);
  const auto expect_track = [&](const std::string& key,
                                std::array<double, 4> want) {
    const auto it = cam->tracks.find(std::to_string(kEnv) + ":" + key);
    ASSERT_TRUE(it != cam->tracks.end()) << key;
    ASSERT_EQ(it->second.samples.size(), 4U) << key;
    for (std::size_t i = 0; i < 4U; ++i) {
      EXPECT_EQ(it->second.samples[i].frame_offset, i) << key;
      EXPECT_DOUBLE_EQ(it->second.samples[i].value.number, want[i])
          << key << " sample " << i;
    }
  };
  // eye.x sweeps 10..13 and fov 60..63; every other channel holds.
  expect_track("camera_eye.x", {10.0, 11.0, 12.0, 13.0});
  expect_track("camera_eye.y", {3.0, 3.0, 3.0, 3.0});
  expect_track("camera_eye.z", {8.0, 8.0, 8.0, 8.0});
  expect_track("camera_target.x", {0.0, 0.0, 0.0, 0.0});
  expect_track("camera_target.y", {1.0, 1.0, 1.0, 1.0});
  expect_track("camera_target.z", {0.0, 0.0, 0.0, 0.0});
  expect_track("camera_fov", {60.0, 61.0, 62.0, 63.0});
  expect_track("sun_direction.x", {0.3, 0.3, 0.3, 0.3});
  expect_track("sun_direction.y", {0.65, 0.65, 0.65, 0.65});
  expect_track("sun_direction.z", {0.7, 0.7, 0.7, 0.7});

  // Playback reproduces the pose bit-identically, tick by tick.
  ok(s, set_camera_cmd(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 10.0));
  omnicpp::core::ControlCommand play;
  play.kind = CK::ClipPlay;
  play.numbers[0] = 1;
  play.number_count = 1;
  tick(2);
  ok(s, play);
  for (std::uint64_t f = 10; f < 14; ++f) {
    tick(f);
    const auto* env = s.document().find(kEnv);
    ASSERT_NE(env, nullptr);
    EXPECT_DOUBLE_EQ(env->properties.at("camera_eye").vec[0],
                     static_cast<double>(f));
    EXPECT_DOUBLE_EQ(env->properties.at("camera_fov").number,
                     50.0 + static_cast<double>(f));
  }

  // Save/load round-trips byte-identically (document incl. all 10 tracks).
  const std::string bytes_before = s.document().to_json();
  omnicpp::core::ControlCommand save;
  save.kind = CK::SaveDocument;
  save.text = "/tmp/t5b_record_all.json";
  ASSERT_TRUE(s.on_control(save).ok) << s.on_control(save).error;
  ed::EditorSession fresh;
  omnicpp::core::ControlCommand load;
  load.kind = CK::LoadDocument;
  load.text = "/tmp/t5b_record_all.json";
  const auto load_reply = fresh.on_control(load);
  ASSERT_TRUE(load_reply.ok) << load_reply.error;
  EXPECT_EQ(fresh.document().to_json(), bytes_before);
  std::remove("/tmp/t5b_record_all.json");
}

// T5b validation: record_all needs clip+oid, rejects unknown ids and
// objects with no recordable property.
TEST(TimelineSession, RecordAllRejectsBadPayloads) {
  ed::EditorSession s;
  omnicpp::core::ControlCommand all;
  all.kind = CK::ClipRecordAll;
  EXPECT_FALSE(s.on_control(all).ok);  // needs clip, oid
  all.numbers[0] = 99;
  all.numbers[1] = 1;
  all.number_count = 2;
  EXPECT_FALSE(s.on_control(all).ok);  // no clip 99
}

// ============================================================================
// 5e. T5c: render-setting tracks (acceptance 3)
// ============================================================================

// Render channels live on the reserved render_settings object (T5c): oid 0
// + a bare channel resolves to it, creating it on demand through an
// undoable spawn. Playback applies onto render_config(), which is what
// hosts map to exposure/bloom/mode each frame.
TEST(TimelineSession, RenderChannelsRecordAndPlayOntoRenderConfig) {
  ed::EditorSession s;
  auto tick = [&](std::uint64_t frame) { (void)s.tick({frame, 1.0 / 60.0, 0.0, false}); };
  // Defaults are behavior-neutral with no object present.
  EXPECT_DOUBLE_EQ(s.render_config().exposure, 1.0);
  EXPECT_DOUBLE_EQ(s.render_config().bloom, 0.0);
  EXPECT_DOUBLE_EQ(s.render_config().render_mode, 0.0);

  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "grade";
  add.numbers[0] = 10;
  add.numbers[1] = 4;
  add.number_count = 2;
  tick(0);
  ok(s, add);
  // oid 0 + bare channel: creates the reserved object.
  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  rec.numbers[0] = 1;
  rec.numbers[1] = 0.0;
  rec.text = "exposure";
  rec.number_count = 2;
  tick(1);
  const auto rec_reply = s.on_control(rec);
  ASSERT_TRUE(rec_reply.ok) << rec_reply.error;
  omnicpp::core::ControlCommand rec_bloom;
  rec_bloom.kind = CK::ClipRecord;
  rec_bloom.numbers[0] = 1;
  rec_bloom.numbers[1] = 0.0;  // resolves to the SAME reserved object
  rec_bloom.text = "bloom";
  rec_bloom.number_count = 2;
  tick(2);
  ASSERT_TRUE(s.on_control(rec_bloom).ok);
  EXPECT_EQ(s.record_arm_count(), 2U);
  // clip_record_all with oid 0 arms every render channel (dupes rejected
  // atomically here because exposure+bloom are already armed).
  omnicpp::core::ControlCommand all;
  all.kind = CK::ClipRecordAll;
  all.numbers[0] = 1;
  all.numbers[1] = 0.0;
  all.number_count = 2;
  EXPECT_FALSE(s.on_control(all).ok);

  // Drive exposure 1..4 and bloom 0,1,0,1 across the span.
  for (std::uint64_t f = 10; f < 14; ++f) {
    ok(s, [&] {
      omnicpp::core::ControlCommand c;
      c.kind = CK::SetProperty;
      c.text = "render_settings";
      c.text2 = "exposure";
      c.numbers[0] = 1.0 + static_cast<double>(f - 10U);
      c.number_count = 1;
      return c;
    }());
    ok(s, [&] {
      omnicpp::core::ControlCommand c;
      c.kind = CK::SetProperty;
      c.text = "render_settings";
      c.text2 = "bloom";
      c.numbers[0] = static_cast<double>((f - 10U) % 2U);
      c.number_count = 1;
      return c;
    }());
    tick(f);
  }
  tick(14);  // past the end: both arms disarm
  EXPECT_EQ(s.record_arm_count(), 0U);
  const auto* grade = s.document().find_clip(1);
  ASSERT_NE(grade, nullptr);
  ASSERT_EQ(grade->tracks.size(), 2U);

  // Playback lands on render_config tick by tick.
  omnicpp::core::ControlCommand play;
  play.kind = CK::ClipPlay;
  play.numbers[0] = 1;
  play.number_count = 1;
  tick(3);
  ok(s, play);
  const double want_exp[4] = {1.0, 2.0, 3.0, 4.0};
  const double want_bloom[4] = {0.0, 1.0, 0.0, 1.0};
  for (std::size_t i = 0; i < 4U; ++i) {
    tick(10 + i);
    const auto cfg = s.render_config();
    EXPECT_DOUBLE_EQ(cfg.exposure, want_exp[i]) << "frame " << i;
    EXPECT_DOUBLE_EQ(cfg.bloom, want_bloom[i]) << "frame " << i;
    // Both tracked channels are driven while the clip plays...
    EXPECT_TRUE(cfg.exposure_driven);
    EXPECT_TRUE(cfg.bloom_driven);
    EXPECT_FALSE(cfg.render_mode_driven);  // ...and only those.
    EXPECT_FALSE(cfg.tone_map_driven);
  }
  tick(20);  // past the span: nothing driven, values hold.
  const auto idle = s.render_config();
  EXPECT_FALSE(idle.exposure_driven);
  EXPECT_FALSE(idle.bloom_driven);
  EXPECT_FALSE(idle.render_mode_driven);
}

// Guard rails: the reserved object cannot be destroyed while tracks
// reference it; render_mode round-trips as a plain number; a document
// without the object is untouched by the feature.
TEST(TimelineSession, RenderSettingsGuardsAndDefaults) {
  ed::EditorSession s;
  auto tick = [&](std::uint64_t frame) { (void)s.tick({frame, 1.0 / 60.0, 0.0, false}); };
  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "mode";
  add.numbers[0] = 0;
  add.numbers[1] = 2;
  add.number_count = 2;
  tick(0);
  ok(s, add);
  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  rec.numbers[0] = 1;
  rec.numbers[1] = 0.0;
  rec.text = "render_mode";
  rec.number_count = 2;
  tick(1);
  ASSERT_TRUE(s.on_control(rec).ok);
  tick(0);  // sample render_mode once, so a track references the object
  tick(2);  // past the span end: arm disarms, the sample stays
  // Reserved id is dynamic; resolve it for the destroy attempt.
  std::uint64_t rs_id = 0;
  for (const auto& o : s.document().objects) {
    if (o.name == "render_settings") rs_id = o.id;
  }
  ASSERT_NE(rs_id, 0U);
  ASSERT_EQ(s.document().find_clip(1)->tracks.size(), 1U);
  omnicpp::core::ControlCommand destroy;
  destroy.kind = CK::DestroyObject;
  destroy.numbers[0] = static_cast<double>(rs_id);
  destroy.number_count = 1;
  const auto destroy_reply = s.on_control(destroy);
  EXPECT_FALSE(destroy_reply.ok) << "referenced reserved object deleted silently";
  EXPECT_NE(destroy_reply.error.find("referenced by"), std::string::npos)
      << destroy_reply.error;
  // The reserved store is not scene content: not selectable, and the
  // projection never turns it into an entity (it would otherwise be a
  // document object with no transform, indistinguishable from a broken
  // cube in the report).
  omnicpp::core::ControlCommand select;
  select.kind = CK::Select;
  select.numbers[0] = static_cast<double>(rs_id);
  select.number_count = 1;
  const auto select_reply = s.on_control(select);
  EXPECT_FALSE(select_reply.ok) << "reserved store was selectable";
  EXPECT_NE(select_reply.error.find("render_settings"), std::string::npos)
      << select_reply.error;
  tick(2);  // projection stage
  EXPECT_FALSE(s.projection().projected(rs_id));

  // Unknown oid + bare channel still rejects (0 is only special for a
  // render channel name; and a non-channel with oid 0 rejects too).
  omnicpp::core::ControlCommand bad;
  bad.kind = CK::ClipRecord;
  bad.numbers[0] = 1;
  bad.numbers[1] = 999.0;
  bad.text = "exposure";
  bad.number_count = 2;
  EXPECT_FALSE(s.on_control(bad).ok);
  omnicpp::core::ControlCommand bad_key = bad;
  bad_key.numbers[1] = 0.0;
  bad_key.text = "position.x";  // oid 0 + non-channel: no object 0
  EXPECT_FALSE(s.on_control(bad_key).ok);
}

// ============================================================================
// 5d. T5d: concurrent multi-arm recording (acceptance 4)
// ============================================================================

// Two arms, two objects, one tick — each track gets its own value; a
// same-track double arm is rejected; disarm-by-clip removes only the
// matching arms; clips_info reports the arm set sorted by (object, key).
TEST(TimelineSession, ConcurrentRecordArmsAreIndependent) {
  ed::EditorSession s;
  auto tick = [&](std::uint64_t frame) { (void)s.tick({frame, 1.0 / 60.0, 0.0, false}); };
  tick(0);
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  const std::string name_a = s.document().objects[1].name;
  const std::string name_b = s.document().objects[2].name;
  const auto id_a = s.document().objects[1].id;
  const auto id_b = s.document().objects[2].id;
  auto set_named = [&](const std::string& name, double x, double y, double z) {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SetProperty;
    c.text = name;
    c.text2 = "position";
    c.numbers[0] = x;
    c.numbers[1] = y;
    c.numbers[2] = z;
    c.number_count = 3;
    return c;
  };

  for (std::uint64_t cid : {1U, 2U}) {
    omnicpp::core::ControlCommand add;
    add.kind = CK::ClipAdd;
    add.text = cid == 1U ? "a" : "b";
    add.numbers[0] = 10;
    add.numbers[1] = 4;
    add.number_count = 2;
    tick(cid);
    ok(s, add);
  }
  // Arm A.x on clip 1, B.y on clip 2.
  omnicpp::core::ControlCommand rec_a;
  rec_a.kind = CK::ClipRecord;
  rec_a.numbers[0] = 1;
  rec_a.numbers[1] = static_cast<double>(id_a);
  rec_a.text = "position.x";
  rec_a.number_count = 2;
  tick(3);
  ok(s, rec_a);
  omnicpp::core::ControlCommand rec_b;
  rec_b.kind = CK::ClipRecord;
  rec_b.numbers[0] = 2;
  rec_b.numbers[1] = static_cast<double>(id_b);
  rec_b.text = "position.y";
  rec_b.number_count = 2;
  tick(4);
  ok(s, rec_b);
  EXPECT_EQ(s.record_arm_count(), 2U);
  // Same-track double arm is rejected, not duplicated.
  const auto dup = s.on_control(rec_a);
  EXPECT_FALSE(dup.ok);
  EXPECT_NE(dup.error.find("already armed"), std::string::npos) << dup.error;
  EXPECT_EQ(s.record_arm_count(), 2U);
  // One tick samples both arms with their own values.
  ok(s, set_named(name_a, 1.0, 0.0, 0.0));
  ok(s, set_named(name_b, 0.0, 4.0, 0.0));
  tick(10);
  ok(s, set_named(name_a, 2.0, 0.0, 0.0));
  ok(s, set_named(name_b, 0.0, 5.0, 0.0));
  tick(11);
  const auto* clip_a = s.document().find_clip(1);
  const auto* clip_b = s.document().find_clip(2);
  ASSERT_NE(clip_a, nullptr);
  ASSERT_NE(clip_b, nullptr);
  ASSERT_EQ(clip_a->tracks.size(), 1U);
  ASSERT_EQ(clip_b->tracks.size(), 1U);
  EXPECT_DOUBLE_EQ(clip_a->tracks.begin()->second.samples[0].value.number, 1.0);
  EXPECT_DOUBLE_EQ(clip_a->tracks.begin()->second.samples[1].value.number, 2.0);
  EXPECT_DOUBLE_EQ(clip_b->tracks.begin()->second.samples[0].value.number, 4.0);
  EXPECT_DOUBLE_EQ(clip_b->tracks.begin()->second.samples[1].value.number, 5.0);
  // clips_info reports the arm set sorted by (object, property).
  omnicpp::core::ControlCommand info;
  info.kind = CK::ClipsInfo;
  const std::string detail = s.on_control(info).detail;
  const std::string want_armed = "\"armed\":[\"" +
                                  std::to_string(id_a) + ":position.x\"]";
  EXPECT_NE(detail.find(want_armed), std::string::npos) << detail;
  // Disarm-by-clip removes only the matching arms.
  omnicpp::core::ControlCommand rm;
  rm.kind = CK::ClipRemove;
  rm.numbers[0] = 1;
  rm.number_count = 1;
  tick(5);
  ok(s, rm);
  EXPECT_EQ(s.record_arm_count(), 1U);
  EXPECT_NE(s.recording_target(), nullptr);
  EXPECT_EQ(s.recording_target()->id, 2U);
  // The surviving arm keeps sampling on later ticks.
  ok(s, set_named(name_b, 0.0, 6.0, 0.0));
  tick(12);
  ASSERT_EQ(s.document().find_clip(2)->tracks.size(), 1U);
  EXPECT_EQ(s.document().find_clip(2)->tracks.begin()->second.samples.size(), 3U);
  EXPECT_DOUBLE_EQ(
      s.document().find_clip(2)->tracks.begin()->second.samples[2].value.number,
      6.0);
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

// ============================================================================
// 7. Capture integrity: wholesale document replaces are unrepresentable
//    mid-capture; warp targets must travel in the file
// ============================================================================

TEST(TimelineSession, LoadDocumentDuringCaptureIsRejected) {
  // A mid-capture load replaces the whole document with bytes no command
  // in the file could reproduce (the loader replays from the single
  // opening checkpoint plus the log). The old code silently dropped the
  // load from the record — a file that diverges on replay. Now loud.
  ed::EditorSession s;
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  const std::string before = s.document().to_json();
  std::string capture_error;
  ASSERT_TRUE(s.capture_start(0, "g3", capture_error)) << capture_error;
  omnicpp::core::ControlCommand load;
  load.kind = CK::LoadDocument;
  load.text = "/nonexistent-doc.warploom";
  const auto reply = s.on_control(load);
  EXPECT_FALSE(reply.ok);
  EXPECT_NE(reply.error.find("capture"), std::string::npos);
  // Untouched: same document, capture still active and stoppable.
  EXPECT_EQ(s.document().to_json(), before);
  EXPECT_TRUE(s.capture_stop("/tmp/g3_load_rejected.replay", capture_error))
      << capture_error;
}

TEST(TimelineSession, LoadReplayDuringCaptureIsRejected) {
  // Same unrepresentability as LoadDocument: a nested replay resets the
  // document and the stack with no trace the format could re-apply
  // (re-applied commands are deliberately NOT re-recorded).
  ed::EditorSession s;
  std::string capture_error;
  ASSERT_TRUE(s.capture_start(0, "g3", capture_error)) << capture_error;
  omnicpp::core::ControlCommand load;
  load.kind = CK::LoadReplay;
  load.text = "/nonexistent.replay";
  const auto reply = s.on_control(load);
  EXPECT_FALSE(reply.ok);
  EXPECT_NE(reply.error.find("capture"), std::string::npos);
  EXPECT_TRUE(s.capture_stop("/tmp/g3_loadrep_rejected.replay",
                              capture_error))
      << capture_error;
}

TEST(TimelineSession, ScrubToDuringCaptureCarriesTargetBytes) {
  // Warp to a PRE-capture checkpoint mid-capture: the target bytes are in
  // the session ring but not in the recorder. Without the session
  // embedding them at the warp, the file loads only until the replayed
  // scrub_to finds no checkpoint — a capture that cannot replay itself.
  ed::EditorSession s;
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  omnicpp::core::ControlCommand scrub_start;
  scrub_start.kind = CK::ScrubStart;
  scrub_start.numbers[0] = 5;
  scrub_start.number_count = 1;
  ok(s, scrub_start);
  const std::string state_at_5 = s.document().to_json();
  // Diverge the document, checkpoint the new state, then capture.
  ok(s, set_position(s, 11.0, 0.0, 0.0));
  scrub_start.numbers[0] = 15;
  ok(s, scrub_start);
  std::string capture_error;
  ASSERT_TRUE(s.capture_start(0, "g3", capture_error)) << capture_error;
  // Warp back to the pre-capture checkpoint: live restore must succeed
  // AND the bytes must be embedded for the file.
  omnicpp::core::ControlCommand scrub_to;
  scrub_to.kind = CK::ScrubTo;
  scrub_to.numbers[0] = 5;
  scrub_to.number_count = 1;
  ok(s, scrub_to);
  EXPECT_EQ(s.document().to_json(), state_at_5);
  const std::string path = "/tmp/g3_scrub_carry.replay";
  ASSERT_TRUE(s.capture_stop(path, capture_error)) << capture_error;
  // The file replays end to end and lands on the warped state.
  ed::EditorSession fresh;
  std::string load_error;
  ASSERT_TRUE(fresh.load_replay(path, load_error)) << load_error;
  EXPECT_EQ(fresh.document().to_json(), state_at_5);
}

TEST(TimelineSession, LoadReplayVerifiesEndStateAgainstClosingCheckpoint) {
  // End-state verification (W2 contract): the file's closing checkpoint
  // must be exactly what the log reproduces. Five host ticks run WITHOUT
  // recording steps, so the live track holds five samples while the
  // three-entry log can only re-simulate three. The per-record hashes
  // are all valid — only the re-applied END state betrays the unlogged
  // ticks. Without the check the load succeeds on divergent bytes.
  ed::EditorSession s;
  ok(s, [] {
    omnicpp::core::ControlCommand c;
    c.kind = CK::SpawnCube;
    c.number_count = 0;
    return c;
  }());
  const auto cube_id = s.document().objects.back().id;
  ok(s, set_position(s, 7.0, 0.0, 0.0));
  std::string capture_error;
  ASSERT_TRUE(s.capture_start(0, "g3", capture_error)) << capture_error;
  omnicpp::core::ControlCommand add;
  add.kind = CK::ClipAdd;
  add.text = "intro";
  add.numbers[0] = 0;
  add.numbers[1] = 8;
  add.number_count = 2;
  ok(s, add);
  omnicpp::core::ControlCommand rec;
  rec.kind = CK::ClipRecord;
  rec.numbers[0] = 1;
  rec.numbers[1] = static_cast<double>(cube_id);
  rec.text = "position.x";
  rec.number_count = 2;
  ok(s, rec);
  // Five live ticks, zero of them recorded as steps.
  for (std::uint64_t i = 0; i < 5; ++i) {
    host_tick(s, i);
  }
  ok(s, step_cmd(1));
  const std::string path = "/tmp/g3_unlogged_ticks.replay";
  ASSERT_TRUE(s.capture_stop(path, capture_error)) << capture_error;
  const auto* clip = s.document().find_clip(1);
  ASSERT_NE(clip, nullptr);
  ASSERT_EQ(clip->tracks.size(), 1U);
  EXPECT_EQ(clip->tracks.begin()->second.samples.size(), 5U);
  // The load must refuse: the log reproduces three samples, not five.
  ed::EditorSession fresh;
  std::string load_error;
  EXPECT_FALSE(fresh.load_replay(path, load_error));
  EXPECT_NE(load_error.find("closing checkpoint"), std::string::npos)
      << load_error;
}

}  // namespace
