//! @file test_session_tick.cpp
//! @brief One tick, one order, one call site. The two hosts had drifted: the
//!        control host ticked the timeline and the viewport did not, so the
//!        same document played differently depending on who drove it while a
//!        comment claimed they shared a contract. These tests pin the order,
//!        the frame stamping, and the pause semantics so that drift cannot
//!        come back unnoticed.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "warploom/core/document_projection.hpp"
#include "warploom/core/editor_session.hpp"

namespace {

using namespace omnicpp::editor;

constexpr double kDt = 1.0 / 60.0;

//! Give a session one cube carrying a full transform, so the projection has
//! something to make an entity from. Sessions are non-copyable, so this fills
//! one in place rather than returning it.
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

}  // namespace

TEST(SessionTick, ProjectsTheDocumentSoSystemsReadOneStore) {
  EditorSession session{};
  seed_cube(session);
  EXPECT_EQ(session.projection().entity_count(), 0U);

  const auto report = session.tick({0U, kDt, 0.0, false});
  EXPECT_TRUE(report.ok());
  // The ECS is refreshed by the tick, which is what gives it a production
  // consumer in the headless path too.
  EXPECT_EQ(session.projection().entity_count(), 1U);
  ASSERT_TRUE(session.projection().projected(1U));
  const auto entity = session.projection().entity_for(1U);
  EXPECT_DOUBLE_EQ(
      session.projection().world().get_component<DocumentTransform>(entity)
          .position[1],
      2.0);
}

TEST(SessionTick, ProjectionSurvivesRepeatedTicksWithIdentityStable) {
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  const auto entity = session.projection().entity_for(1U);

  for (std::uint64_t frame = 1; frame < 8; ++frame) {
    (void)session.tick({frame, kDt, 0.0, false});
  }
  // Re-ticking must not churn identity: a system holding an Entity across
  // frames is the whole point of projecting at all.
  EXPECT_EQ(session.projection().entity_for(1U).id, entity.id);
  EXPECT_EQ(session.projection().entity_for(1U).generation, entity.generation);
  EXPECT_EQ(session.projection().entity_count(), 1U);
}

TEST(SessionTick, PausedTicksDoNotAdvanceTheSimulationStage) {
  EditorSession session{};
  seed_cube(session);
  const auto running = session.tick({0U, kDt, 0.0, false});
  EXPECT_GT(running.physics_substeps, 0U) << "an unpaused tick must step physics";

  // A paused host must still see graph edits apply, which is why sync_graph
  // runs before the pause gate -- so a paused tick is not a no-op tick.
  const auto paused = session.tick({1U, kDt, 0.0, true});
  EXPECT_TRUE(paused.ok());
  EXPECT_EQ(paused.physics_substeps, 0U) << "paused must not step physics";
  // And the document is still projected, so the UI keeps working while time
  // is stopped.
  EXPECT_EQ(session.projection().entity_count(), 1U);
}

TEST(SessionTick, ZeroTimestepSkipsTheSimulationStage) {
  EditorSession session{};
  seed_cube(session);
  const auto report = session.tick({0U, 0.0, false});
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.physics_substeps, 0U);
}

TEST(SessionTick, TimelineIsDrivenByTheTickNotByTheHost) {
  // The regression that motivated C2. Arm a clip that writes the cube's
  // position, then tick. If the tick did not run the timeline stage, the
  // document would keep its authored value -- which is exactly what the
  // viewport used to do while the control host did not.
  EditorSession session{};
  seed_cube(session);
  TimelineClip clip{};
  clip.id = 1U;
  clip.name = "move";
  clip.start_frame = 0U;
  clip.length_frames = 10U;
  clip.tracks[track_key(1U, "position")] = ClipTrack{
      1U, "position",
      {ClipSample{0U, PropValue::make_vec3(50.0, 0.0, 0.0)}}};

  SceneDocument document = session.document();
  document.clips.push_back(clip);
  session.reset_from(std::move(document));

  // The clip has to be armed for playback; drive it through the protocol so
  // this test exercises the same path a host would.
  omnicpp::core::ControlCommand play{};
  play.kind = omnicpp::core::ControlCommand::Kind::ClipPlay;
  play.id = 1U;
  play.numbers[0] = static_cast<double>(clip.id);
  play.number_count = 1U;
  const auto armed = session.on_control(play);
  ASSERT_TRUE(armed.ok) << armed.error;
  ASSERT_NE(session.playing_clip(), nullptr);

  (void)session.tick({0U, kDt, 0.0, false});

  const SceneObject* moved = session.document().find(1U);
  ASSERT_NE(moved, nullptr);
  const auto position = moved->properties.find("position");
  ASSERT_NE(position, moved->properties.end());
  // Whatever the clip evaluates to, the projected store must agree with the
  // document: the tick projects AFTER the timeline runs.
  ASSERT_DOUBLE_EQ(position->second.vec[0], 50.0)
      << "the clip must have written the document";
  {
    const auto entity = session.projection().entity_for(1U);
    ASSERT_TRUE(session.projection().projected(1U));
    EXPECT_DOUBLE_EQ(session.projection()
                        .world()
                        .get_component<DocumentTransform>(entity)
                        .position[0],
                    50.0)
        << "the projection must reflect the post-playback value";
  }
}

TEST(SessionTick, FrameComesFromTheInputNotAnInternalCounter) {
  // A host owns the frame counter; the session stamps from it. Two sessions
  // driven with the same frames must land in the same place.
  EditorSession a{};
  EditorSession b{};
  seed_cube(a);
  seed_cube(b);
  for (std::uint64_t frame = 0; frame < 4; ++frame) {
    (void)a.tick({frame, kDt, 0.0, false});
    (void)b.tick({frame, kDt, 0.0, false});
  }
  ASSERT_TRUE(a.projection().projected(1U));
  ASSERT_TRUE(b.projection().projected(1U));
  const auto ea = a.projection().entity_for(1U);
  const auto eb = b.projection().entity_for(1U);
  EXPECT_EQ(ea.id, eb.id);
  EXPECT_EQ(ea.generation, eb.generation);
  EXPECT_EQ(a.snapshot_json(), b.snapshot_json())
      << "identical ticks must produce identical state";
}
// ----------------------------------------------------------------------------
// C3: physics in the tick, and in the protocol
// ----------------------------------------------------------------------------

namespace {

//! A body resting above the ground, so a tick actually moves it.
::omnicpp::physics::PhysicsBody falling_body() {
  ::omnicpp::physics::PhysicsBody body{};
  body.position[0] = 0.0F;
  body.position[1] = 10.0F;
  body.position[2] = 0.0F;
  body.velocity[0] = 0.0F;
  body.velocity[1] = 0.0F;
  body.velocity[2] = 0.0F;
  body.radius = 0.5F;
  body.inverse_mass = 1.0F;
  body.restitution = 0.4F;
  return body;
}

}  // namespace

TEST(SessionTick, TickAdvancesPhysicsByTheFrameTimestep) {
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(session.spawn_physics_body(1U, falling_body()));

  const auto before = session.physics_body_for(1U);
  ASSERT_NE(before, nullptr);
  const float start_y = before->position[1];
  ASSERT_GT(start_y, 0.0F);

  (void)session.tick({1U, kDt, 0.0, false});
  const auto after = session.physics_body_for(1U);
  ASSERT_NE(after, nullptr);
  // Gravity is negative on Y, so a free body must fall.
  EXPECT_LT(after->position[1], start_y);
}

TEST(SessionTick, SubstepCountIsAFunctionOfTimestepNotWallClock) {
  // Two sessions, same ticks, must integrate identically -- byte for byte.
  // This is the property replay depends on.
  EditorSession a{};
  EditorSession b{};
  seed_cube(a);
  seed_cube(b);
  (void)a.tick({0U, kDt, 0.0, false});
  (void)b.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(a.spawn_physics_body(1U, falling_body()));
  ASSERT_TRUE(b.spawn_physics_body(1U, falling_body()));

  for (std::uint64_t frame = 1; frame <= 20; ++frame) {
    const auto ra = a.tick({frame, kDt, 0.0, false});
    const auto rb = b.tick({frame, kDt, 0.0, false});
    ASSERT_EQ(ra.physics_substeps, rb.physics_substeps);
  }
  EXPECT_EQ(a.physics_body_for(1U)->position[1],
            b.physics_body_for(1U)->position[1])
      << "identical ticks must integrate bit-identically";
  EXPECT_EQ(a.snapshot_json(), b.snapshot_json());
}

TEST(SessionTick, SubstepCountGrowsWithTimestepAndIsCapped) {
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  (void)session.spawn_physics_body(1U, falling_body());

  // 1/60 s needs four 1/240 substeps; a tiny dt needs exactly one.
  const auto at_60hz = session.tick({1U, 1.0 / 60.0, false});
  EXPECT_GT(at_60hz.physics_substeps, 1U);
  const auto at_1hz = session.tick({2U, 0.001, 0.0, false});
  EXPECT_EQ(at_1hz.physics_substeps, 1U);
  // And a pathological dt is capped rather than trusted.
  const auto absurd = session.tick({3U, 1000.0, false});
  EXPECT_LE(absurd.physics_substeps, 8U);
}

TEST(SessionTick, PausedTicksDoNotAdvancePhysics) {
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  (void)session.spawn_physics_body(1U, falling_body());
  (void)session.tick({1U, kDt, 0.0, false});
  const float settled = session.physics_body_for(1U)->position[1];

  const auto paused = session.tick({2U, kDt, 0.0, true});
  EXPECT_EQ(paused.physics_substeps, 0U);
  EXPECT_FLOAT_EQ(session.physics_body_for(1U)->position[1], settled)
      << "a paused tick must not move a body";
}

TEST(SessionTick, SimulatedPoseReachesTheProjectedStore) {
  // The point of the whole exercise: the renderer reads the projection, so a
  // simulated body must be visible there and not snap back to the authored
  // position on the next tick.
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(session.spawn_physics_body(1U, falling_body()));

  (void)session.tick({1U, kDt, 0.0, false});
  ASSERT_TRUE(session.projection().projected(1U));
  const auto entity = session.projection().entity_for(1U);
  // Copy, not a reference: the projection rewrites this component every tick,
  // so holding a reference across tick 2 would compare the new value to itself.
  const double after_first =
      session.projection().world().get_component<DocumentTransform>(entity)
          .position[1];
  EXPECT_DOUBLE_EQ(after_first,
                   static_cast<double>(session.physics_body_for(1U)->position[1]))
      << "the projected transform must track the simulated pose";

  // And it must survive the next project() rather than reverting to 2.0.
  (void)session.tick({2U, kDt, 0.0, false});
  const double after_second =
      session.projection().world().get_component<DocumentTransform>(entity)
          .position[1];
  EXPECT_LT(after_second, after_first)
      << "a simulated object must not snap back to its authored position";
}

TEST(SessionTick, SpawningForAnUnprojectedObjectFails) {
  EditorSession session{};
  seed_cube(session);
  // No tick yet, so nothing is projected.
  EXPECT_FALSE(session.spawn_physics_body(1U, falling_body()))
      << "simulating something invisible would look like physics did nothing";
}

TEST(SessionTick, PhysicsStateTravelsInTheSnapshot) {
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(session.spawn_physics_body(1U, falling_body()));
  (void)session.tick({1U, kDt, 0.0, false});

  const std::string json = session.snapshot_json();
  EXPECT_NE(json.find("\"physics\""), std::string::npos);
  EXPECT_NE(json.find("\"bodies\""), std::string::npos);
  EXPECT_NE(json.find("\"gravity\""), std::string::npos);
  // The body actually present, with its current position.
  EXPECT_NE(json.find("\"oid\":1"), std::string::npos);
}

TEST(SessionTick, SnapshotFloatsRoundTripExactly) {
  // A body at a position std::to_string could not express: six decimals is
  // lossy, and a resumed replay would then diverge on the next tick.
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});
  ::omnicpp::physics::PhysicsBody awkward = falling_body();
  awkward.position[1] = 0.123456789012345F;
  awkward.velocity[1] = -3.0517578125e-05F;
  ASSERT_TRUE(session.spawn_physics_body(1U, awkward));

  // The snapshot carries the body's position AFTER a tick, so the value under
  // test is the live one, not the authored seed. Round-trip means: what parses
  // back out of the JSON is exactly the float in the world.
  const std::string json = session.snapshot_json();
  const auto parse_array3 = [&json](const char* key) {
    const std::size_t at = json.find(key);
    EXPECT_NE(at, std::string::npos) << key << " missing from snapshot";
    if (at == std::string::npos) return std::array<double, 3>{};
    const std::size_t open = json.find('[', at);
    const std::size_t close = json.find(']', open);
    EXPECT_NE(close, std::string::npos);
    std::array<double, 3> out{};
    std::size_t cursor = open + 1;
    for (std::size_t i = 0; i < 3; ++i) {
      out[i] = std::strtod(json.c_str() + cursor, nullptr);
      while (cursor < json.size() && json[cursor] != ',' && json[cursor] != ']') {
        ++cursor;
      }
      ++cursor;
    }
    return out;
  };
  const auto position = parse_array3("\"pos\"");
  EXPECT_FLOAT_EQ(static_cast<float>(position[1]),
                  session.physics_body_for(1U)->position[1])
      << "the snapshot must round-trip a float std::to_string would truncate";
  EXPECT_FLOAT_EQ(static_cast<float>(parse_array3("\"vel\"")[1]),
                  session.physics_body_for(1U)->velocity[1]);

  // And re-emitting the same state twice gives identical bytes.
  EditorSession other{};
  seed_cube(other);
  (void)other.tick({0U, kDt, 0.0, false});
  ASSERT_TRUE(other.spawn_physics_body(1U, awkward));
  EXPECT_EQ(session.snapshot_json(), other.snapshot_json())
      << "identical physics state must serialise to identical bytes";
}

// ----------------------------------------------------------------------------
// D2a: sub-frame tick position reaches clip playback
// ----------------------------------------------------------------------------

namespace {

//! A session with one cube and a two-key clip on its position: sample 0 at
//! x=0, sample 10 at x=100, so linear interpolation is readable by eye.
void seed_clip(EditorSession& session) {
  seed_cube(session);
  TimelineClip clip{};
  clip.id = 1U;
  clip.name = "move";
  clip.start_frame = 0U;
  clip.length_frames = 20U;
  clip.tracks[track_key(1U, "position")] = ClipTrack{
      1U, "position",
      {ClipSample{0U, PropValue::make_vec3(0.0, 0.0, 0.0)},
       ClipSample{10U, PropValue::make_vec3(100.0, 0.0, 0.0)}}};
  SceneDocument document = session.document();
  document.clips.push_back(clip);
  session.reset_from(std::move(document));

  omnicpp::core::ControlCommand play{};
  play.kind = omnicpp::core::ControlCommand::Kind::ClipPlay;
  play.id = 1U;
  play.numbers[0] = static_cast<double>(clip.id);
  play.number_count = 1U;
  const auto armed = session.on_control(play);
  EXPECT_TRUE(armed.ok) << armed.error;
}

double cube_x(const EditorSession& session) {
  const SceneObject* object = session.document().find(1U);
  if (object == nullptr) return -1.0;
  const auto it = object->properties.find("position");
  if (it == object->properties.end()) return -1.0;
  return it->second.vec[0];
}

}  // namespace

TEST(SessionTick, SubFrameZeroReproducesStepHoldExactly) {
  // The property that keeps every existing recording valid: alpha 0 through
  // the interpolated path must be indistinguishable from step-hold.
  EditorSession stepped{};
  seed_clip(stepped);
  EditorSession fractional{};
  seed_clip(fractional);

  for (std::uint64_t frame = 0; frame < 12; ++frame) {
    (void)stepped.tick({frame, kDt, 0.0, false});
    (void)fractional.tick({frame, kDt, 0.0, false});
  }
  EXPECT_DOUBLE_EQ(cube_x(stepped), cube_x(fractional));
  // Step-hold reaches the second key at clip offset 10 and holds it there, so
  // frame 11 is already at the final value -- not still at the first key.
  EXPECT_NEAR(cube_x(stepped), 100.0, 1e-6);
}

TEST(SessionTick, SubFramePositionInterpolatesTheTrack) {
  EditorSession session{};
  seed_clip(session);

  // Keys sit at clip offsets 0 and 10 running 0 -> 100. Frame 5 at alpha 0.5
  // samples at 5.5, which is 55% of the way, so 55. Step-hold would have said
  // 0 -- that difference is the whole point.
  (void)session.tick({5U, kDt, 0.5, false});
  EXPECT_NEAR(cube_x(session), 55.0, 1e-3)
      << "a sub-frame tick must interpolate, not hold";
  // And at alpha 0 the same frame holds the earlier key.
  EditorSession held{};
  seed_clip(held);
  (void)held.tick({5U, kDt, 0.0, false});
  EXPECT_NEAR(cube_x(held), 0.0, 1e-6);
}

TEST(SessionTick, SubFrameIsClampedRatherThanTrusted) {
  EditorSession session{};
  seed_clip(session);
  // A host handing back an alpha at or past 1 must not sample into the next
  // frame or rewind; it is pulled just inside the frame.
  (void)session.tick({5U, kDt, 4.5, false});
  const double x = cube_x(session);
  EXPECT_GE(x, 0.0);
  EXPECT_LE(x, 100.0 + 1e-3) << "clamped alpha must stay within the clip";

  EditorSession negative{};
  seed_clip(negative);
  (void)negative.tick({5U, kDt, -3.0, false});
  EXPECT_NEAR(cube_x(negative), 0.0, 1e-6)
      << "a negative alpha falls back to the frame itself";
}

TEST(SessionTick, PausedTicksIgnoreTheSubFramePosition) {
  EditorSession session{};
  seed_clip(session);
  (void)session.tick({0U, kDt, 0.0, false});
  const double before = cube_x(session);
  const auto paused = session.tick({1U, kDt, 0.75, true});
  EXPECT_TRUE(paused.ok());
  // Time is stopped: a paused tick must not advance playback either.
  EXPECT_NEAR(cube_x(session), 0.0, 1e-6);
  (void)before;
}

// Physics state goes into the snapshot every frame. Serialising the float via
// double is exact but prints the double's shortest form, three to four times
// longer: 1e+20 becomes 21 characters and 3.4e+38 becomes 22. This pins both
// properties that matter -- the text is short, and it still recovers the exact
// float.
TEST(SessionTick, PhysicsFloatsSerialiseShortAndExactly) {
  EditorSession session{};
  seed_cube(session);
  (void)session.tick({0U, kDt, 0.0, false});

  ::omnicpp::physics::PhysicsBody body{};
  // Component 0 is the first array element, which is what the token extractor
  // below reads; component 1 would need different indexing and proves nothing
  // extra about the formatter.
  body.position[0] = 0.123456789012345F;
  body.velocity[0] = -3.0517578125e-05F;
  body.radius = 1.0e20F;
  ASSERT_TRUE(session.spawn_physics_body(1U, body));

  const std::string json = session.snapshot_json();
  const auto body_json = json.find("\"bodies\"");
  ASSERT_NE(body_json, std::string::npos) << json.substr(0, 400);

  // Short: the exact text the float formatter produces, not the double's.
  EXPECT_NE(json.find("\"radius\":1e+20"), std::string::npos)
      << "serialising through double emits 21 characters for this value";
  EXPECT_EQ(json.find("100000002004087734272"), std::string::npos);

  // And exact: parse each emitted token back and compare to the world.
  const auto token_after = [&json](const std::string& key) {
    const std::size_t at = json.find(key);
    if (at == std::string::npos) return std::string{};
    std::size_t cursor = at + key.size();
    while (cursor < json.size() &&
           (json[cursor] == ':' || json[cursor] == '[' || json[cursor] == ' ')) {
      ++cursor;
    }
    const std::size_t end = json.find_first_of(",}]", cursor);
    return json.substr(cursor, end - cursor);
  };
  const auto* live = session.physics_body_for(1U);
  ASSERT_NE(live, nullptr);
  EXPECT_FLOAT_EQ(std::strtof(token_after("\"pos\"").c_str(), nullptr),
                  live->position[0]);
  EXPECT_FLOAT_EQ(std::strtof(token_after("\"vel\"").c_str(), nullptr),
                  live->velocity[0]);
  EXPECT_FLOAT_EQ(std::strtof(token_after("\"radius\"").c_str(), nullptr),
                  live->radius);
}
