//! @file test_physics_scrub.cpp
//! @brief T4a proofs: scrub checkpoints restore physics bit-identically,
//!        and replay files round-trip it.
//!
//! The gap this closes (roadmap C3 honest limit): checkpoints captured the
//! document only, so a time-warp scrub left physics simulating from its
//! pre-warp state while the document jumped back. These tests pin the fix
//! from three directions:
//!
//! 1. Ring scrub: simulate, checkpoint, simulate more, scrub back — state
//!    fingerprints and snapshot bytes return to the captured values, and
//!    continuing to tick reproduces the uninterrupted trajectory exactly.
//! 2. Tamper: corrupted physics bytes (hash or schema) fail the scrub with
//!    document AND physics untouched (atomicity, both layers).
//! 3. File replay: a capture with live physics loads into a fresh session
//!    and reproduces bit-identical physics — including a paused span,
//!    which pins the re-apply loop's pause gate (removing the gate fails
//!    the paused-span case while the unpaused case still passes).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include "warploom/core/document_projection.hpp"
#include "warploom/core/editor_session.hpp"
#include "warploom/core/physics_snapshot.hpp"
#include "warploom/core/replay_scrubber.hpp"

namespace {

using namespace omnicpp::editor;

constexpr double kDt = 1.0 / 60.0;

using CK = omnicpp::core::ControlCommand::Kind;

//! One cube with a full transform, filled in place (sessions are
//! non-copyable). Mirrors test_session_tick's seeder.
void seed_cube(::omnicpp::editor::EditorSession& session) {
  SceneObject cube{};
  cube.id = 1U;
  cube.type_id = 1U;
  cube.name = "cube";
  cube.properties["position"] = PropValue::make_vec3(1.0, 2.0, 3.0);
  cube.properties["rotation"] = PropValue::make_vec3(0.0, 0.0, 0.0);
  cube.properties["scale"] = PropValue::make_vec3(1.0, 1.0, 1.0);
  SceneDocument document{};
  document.objects.push_back(cube);
  document.next_object_id = 2U;
  session.reset_from(std::move(document));
}

//! A body high above the ground: every tick moves it, so fingerprints
//! distinguish integrated from frozen state.
::omnicpp::physics::PhysicsBody falling_body() {
  ::omnicpp::physics::PhysicsBody body{};
  body.position[1] = 10.0F;
  body.radius = 0.5F;
  body.inverse_mass = 1.0F;
  body.restitution = 0.4F;
  return body;
}

//! Live driver mirroring the host contract AND the re-apply loop's model:
//! one tick per command arrival (then the command), N ticks for Step(N).
//! Pause/Resume flip the tick gate after their own tick — the same order
//! load_replay re-applies in, so a capture driven this way must reload
//! bit-identically when the plumbing is correct.
struct LiveDriver {
  EditorSession* session{nullptr};
  std::uint64_t tick_frame{0};
  bool paused{false};

  void tick_once() {
    (void)session->tick({tick_frame++, kDt, 0.0, paused});
  }

  void send(CK kind, double number = 0.0, bool has_number = false) {
    tick_once();
    omnicpp::core::ControlCommand cmd;
    cmd.kind = kind;
    if (has_number) {
      cmd.numbers[0] = number;
      cmd.number_count = 1;
    }
    ASSERT_TRUE(session->on_control(cmd).ok) << session->on_control(cmd).error;
    if (kind == CK::Pause) paused = true;
    if (kind == CK::Resume) paused = false;
  }

  void step(std::uint64_t ticks) {
    for (std::uint64_t i = 0; i < ticks; ++i) tick_once();
    omnicpp::core::ControlCommand cmd;
    cmd.kind = CK::Step;
    cmd.numbers[0] = static_cast<double>(ticks);
    cmd.number_count = 1;
    ASSERT_TRUE(session->on_control(cmd).ok);
  }
};

[[nodiscard]] std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST(PhysicsScrub, RingScrubRestoresPhysicsBitIdentically) {
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(session.spawn_physics_body(1U, falling_body()));

  for (std::uint64_t frame = 1; frame <= 20; ++frame) {
    (void)session.tick({frame, kDt, 0.0, false});
  }
  const auto fp20 = session.physics().state_fingerprint();
  const auto snap20 = session.snapshot_json();

  std::string error;
  ASSERT_TRUE(session.scrub_start(20U, error)) << error;

  for (std::uint64_t frame = 21; frame <= 40; ++frame) {
    (void)session.tick({frame, kDt, 0.0, false});
  }
  const auto fp40 = session.physics().state_fingerprint();
  const auto snap40 = session.snapshot_json();
  EXPECT_NE(fp40, fp20) << "the post-checkpoint run must actually move";

  ASSERT_TRUE(session.scrub_to(20U, error)) << error;
  EXPECT_EQ(session.physics().state_fingerprint(), fp20);
  EXPECT_EQ(session.snapshot_json(), snap20);

  // Continuing from the restored state reproduces the uninterrupted run.
  for (std::uint64_t frame = 21; frame <= 40; ++frame) {
    (void)session.tick({frame, kDt, 0.0, false});
  }
  EXPECT_EQ(session.physics().state_fingerprint(), fp40);
  EXPECT_EQ(session.snapshot_json(), snap40);
}

TEST(PhysicsScrub, CorruptedPhysicsCheckpointFailsWithStateUntouched) {
  // Ring layer (owns the bytes): torn physics never restores, and the
  // document the caller passed in is untouched on failure.
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(session.spawn_physics_body(1U, falling_body()));
  for (std::uint64_t frame = 1; frame <= 5; ++frame) {
    (void)session.tick({frame, kDt, 0.0, false});
  }
  std::string error;
  ASSERT_TRUE(session.scrub_start(5U, error)) << error;
  const auto doc_before = session.document().to_json();
  const auto fp_before = session.physics().state_fingerprint();
  std::string valid_physics;
  ASSERT_TRUE(session.scrubber().physics_for(5U, valid_physics));
  ASSERT_FALSE(valid_physics.empty());

  // Ring layer: the ring is byte-opaque (it never parses physics — the
  // session validates through the codec BEFORE committing). Truncated
  // bytes with a matching hash restore the DOCUMENT half fine...
  const std::string torn = valid_physics.substr(
      0, valid_physics.size() / 2U);
  const std::uint64_t torn_hash = fnv1a64(torn.data(), torn.size());
  ReplayScrubber ring;
  ASSERT_TRUE(ring.capture(5U, session.document(), valid_physics, error))
      << error;
  ring.insert(5U, state_hash(session.document()),
              session.document().to_json(), torn_hash, torn);
  SceneDocument out;
  ASSERT_TRUE(ring.restore(5U, out, error)) << error;
  EXPECT_EQ(out.to_json(), doc_before);
  // ...but a WRONG hash fails the restore with the output untouched.
  ring.insert(5U, state_hash(session.document()),
              session.document().to_json(), torn_hash + 1U, torn);
  SceneDocument out2;
  const std::string sentinel = out2.to_json();
  EXPECT_FALSE(ring.restore(5U, out2, error));
  EXPECT_EQ(out2.to_json(), sentinel);

  // Codec layer (owns the schema): all-or-nothing commit — a failed
  // restore leaves the world AND the map exactly as they were.
  ::omnicpp::physics::PhysicsWorld world;
  world.add_body(falling_body());
  std::map<std::uint64_t, std::uint32_t> map{{1U, 0U}};
  const std::uint64_t n_before = world.body_count();
  EXPECT_FALSE(physics_restore_json(world, map, torn, error));
  EXPECT_EQ(world.body_count(), n_before);
  EXPECT_EQ(map.size(), 1U);
  EXPECT_EQ(map.at(1U), 0U);

  // The live session is unaffected by all of the above: its checkpoint
  // still restores cleanly.
  error.clear();
  ASSERT_TRUE(session.scrub_to(5U, error)) << error;
  EXPECT_EQ(session.document().to_json(), doc_before);
  EXPECT_EQ(session.physics().state_fingerprint(), fp_before);

  // File layer, two torn variants. Variant H (shown): a capture whose
  // physics line is tampered with must fail to load with a clean protocol
  // error, not a silent divergence.
  const std::string path = "/tmp/omnicpp_test_physics_tamper.replay";
  {
    EditorSession live{};
    seed_cube(live);
    (void)live.tick({0U, kDt, 0.0, false});
    ASSERT_TRUE(live.spawn_physics_body(1U, falling_body()));
    ASSERT_TRUE(live.capture_start(0U, "tamper", error)) << error;
    LiveDriver driver{&live, 1U, false};
    driver.step(5U);
    ASSERT_TRUE(live.capture_stop(path, error)) << error;
  }
  const std::string pristine = read_file(path);
  ASSERT_FALSE(pristine.empty());
  std::string bytes = pristine;
  // Flip one byte in the first physics line (a line starting with the
  // physics object marker right after a document line).
  // Checkpoint record = marker line, document line, physics line.
  const std::string marker = "\"p_hash\":\"";
  const std::size_t at = bytes.find(marker);
  ASSERT_NE(at, std::string::npos);
  const std::size_t marker_end = bytes.find('\n', at);
  ASSERT_NE(marker_end, std::string::npos);
  const std::size_t doc_end = bytes.find('\n', marker_end + 1U);
  ASSERT_NE(doc_end, std::string::npos);
  const std::size_t phys_start = doc_end + 1U;
  const std::size_t phys_end = bytes.find('\n', phys_start);
  ASSERT_NE(phys_end, std::string::npos);
  ASSERT_GT(phys_end, phys_start + 10U);
  // Corrupt a key character (guaranteed not a digit flip that stays
  // schema-valid): the load must fail loudly, never diverge silently.
  ASSERT_EQ(bytes[phys_start], '{');
  bytes[phys_start + 2U] = 'X';
  {
    std::ofstream tampered(path, std::ios::binary | std::ios::trunc);
    tampered.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  EditorSession loaded{};
  error.clear();
  EXPECT_FALSE(loaded.load_replay(path, error));
  EXPECT_NE(error.find("physics"), std::string::npos) << error;

  // Variant S: truncated physics with a RECOMPUTED hash passes the hash
  // layer, so the load must fail in the opening-physics schema restore —
  // the session path that scrub_to relies on for its atomicity.
  std::string bytes2 = pristine;
  {
    const std::string marker2 = "\"p_hash\":\"";
    const std::size_t at2 = bytes2.find(marker2);
    ASSERT_NE(at2, std::string::npos);
    const std::size_t marker_end2 = bytes2.find('\n', at2);
    const std::size_t doc_end2 = bytes2.find('\n', marker_end2 + 1U);
    const std::size_t phys_start2 = doc_end2 + 1U;
    const std::size_t phys_end2 = bytes2.find('\n', phys_start2);
    ASSERT_NE(phys_end2, std::string::npos);
    const std::string truncated = bytes2.substr(
        phys_start2, (phys_end2 - phys_start2) / 2U);
    const std::uint64_t fixed_hash =
        fnv1a64(truncated.data(), truncated.size());
    // Splice: new physics line + patched marker hash.
    bytes2.replace(phys_start2, phys_end2 - phys_start2, truncated);
    const std::size_t hash_val_at =
        at2 + std::string("\"p_hash\":\"").size();
    const std::size_t hash_val_end = bytes2.find('"', hash_val_at);
    bytes2.replace(hash_val_at, hash_val_end - hash_val_at,
                    std::to_string(fixed_hash));
  }
  {
    std::ofstream rewritten(path, std::ios::binary | std::ios::trunc);
    rewritten.write(bytes2.data(),
                    static_cast<std::streamsize>(bytes2.size()));
  }
  EditorSession loaded2{};
  error.clear();
  EXPECT_FALSE(loaded2.load_replay(path, error));
  EXPECT_NE(error.find("opening physics"), std::string::npos) << error;
  std::remove(path.c_str());
}

TEST(PhysicsScrub, CodecRoundTripsKnobsJointsAndCapsules) {
  ::omnicpp::physics::PhysicsWorld world(-3.7F);
  world.set_substeps(4U);
  world.set_solver_iterations(8U);
  world.set_contact_compliance(0.001F);
  world.set_restitution_threshold(0.2F);
  world.set_sleep_threshold(0.05F);
  world.set_velocity_from_position(true);
  ::omnicpp::physics::PhysicsBody sphere{};
  sphere.position[1] = 5.0F;
  sphere.velocity[0] = 1.0F;
  const std::uint32_t s0 = world.add_body(sphere);
  ::omnicpp::physics::PhysicsBody capsule{};
  capsule.shape = ::omnicpp::physics::BodyShape::Capsule;
  capsule.half_height = 1.5F;
  capsule.orientation[0] = 0.5F;
  capsule.orientation[3] = 0.8660254F;
  capsule.angular_velocity[2] = 2.0F;
  const std::uint32_t c1 = world.add_body(capsule);
  world.add_distance_joint(s0, c1, 3.0F, 0.01F);
  std::map<std::uint64_t, std::uint32_t> map{{7U, s0}, {9U, c1}};

  const std::string bytes = physics_snapshot_json(world, map);
  EXPECT_FALSE(bytes.empty());

  ::omnicpp::physics::PhysicsWorld restored;
  std::map<std::uint64_t, std::uint32_t> restored_map{{1U, 1U}};  // poison
  std::string restore_error;
  ASSERT_TRUE(
      physics_restore_json(restored, restored_map, bytes, restore_error))
      << restore_error;
  // Same state serializes to identical bytes (determinism of the codec).
  EXPECT_EQ(physics_snapshot_json(restored, restored_map), bytes);
  // Knobs, bodies, joints, and the map all came back.
  EXPECT_EQ(restored_map.size(), 2U);
  EXPECT_EQ(restored_map.at(7U), 0U);
  EXPECT_EQ(restored_map.at(9U), 1U);
  EXPECT_EQ(restored.joint_count(), 1U);
  EXPECT_FLOAT_EQ(restored.gravity(), -3.7F);
  EXPECT_EQ(restored.substeps(), 4U);
  EXPECT_EQ(restored.solver_iterations(), 8U);
  EXPECT_TRUE(restored.velocity_from_position());

  // Empty snapshot restores an empty world (old-file contract).
  ASSERT_TRUE(physics_restore_json(restored, restored_map, {}, restore_error));
  EXPECT_EQ(restored.body_count(), 0U);
  EXPECT_TRUE(restored_map.empty());
}

TEST(PhysicsScrub, ReplayFileRoundTripsPhysicsBitIdentically) {
  const std::string path = "/tmp/omnicpp_test_physics_roundtrip.replay";
  EditorSession live{};
  seed_cube(live);
  (void)live.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(live.spawn_physics_body(1U, falling_body()));

  std::string error;
  ASSERT_TRUE(live.capture_start(0U, "phys", error)) << error;
  LiveDriver driver{&live, 1U, false};
  driver.step(30U);
  ASSERT_TRUE(live.capture_stop(path, error)) << error;

  const std::string bytes = read_file(path);
  ASSERT_FALSE(bytes.empty());
  EXPECT_NE(bytes.find("\"p_hash\""), std::string::npos);
  EXPECT_NE(bytes.find("\"fixed_dt\""), std::string::npos);
  const auto live_fp = live.physics().state_fingerprint();
  const auto live_snap = live.snapshot_json();

  EditorSession loaded{};
  error.clear();
  ASSERT_TRUE(loaded.load_replay(path, error)) << error;
  EXPECT_EQ(loaded.physics().state_fingerprint(), live_fp);
  EXPECT_EQ(loaded.snapshot_json(), live_snap);
  EXPECT_EQ(loaded.document().to_json(), live.document().to_json());
  std::remove(path.c_str());
}

TEST(PhysicsScrub, ReplayPausesFreezePhysicsOnReload) {
  const std::string path = "/tmp/omnicpp_test_physics_pause.replay";
  EditorSession live{};
  seed_cube(live);
  (void)live.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(live.spawn_physics_body(1U, falling_body()));

  std::string error;
  ASSERT_TRUE(live.capture_start(0U, "phys", error)) << error;
  LiveDriver driver{&live, 1U, false};
  driver.step(10U);
  driver.send(CK::Pause);
  for (int i = 0; i < 5; ++i) driver.tick_once();  // frozen span
  driver.send(CK::Resume);
  driver.step(10U);
  const auto frozen_fp = live.physics().state_fingerprint();
  ASSERT_TRUE(live.capture_stop(path, error)) << error;
  (void)frozen_fp;

  EditorSession loaded{};
  error.clear();
  ASSERT_TRUE(loaded.load_replay(path, error)) << error;
  EXPECT_EQ(loaded.physics().state_fingerprint(),
            live.physics().state_fingerprint())
      << "paused spans must re-simulate frozen, not integrated";
  EXPECT_EQ(loaded.snapshot_json(), live.snapshot_json());
  std::remove(path.c_str());
}
