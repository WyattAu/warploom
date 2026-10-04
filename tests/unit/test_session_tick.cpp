//! @file test_session_tick.cpp
//! @brief One tick, one order, one call site. The two hosts had drifted: the
//!        control host ticked the timeline and the viewport did not, so the
//!        same document played differently depending on who drove it while a
//!        comment claimed they shared a contract. These tests pin the order,
//!        the frame stamping, and the pause semantics so that drift cannot
//!        come back unnoticed.

#include <gtest/gtest.h>

#include <cstdint>
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

  const auto report = session.tick({0U, kDt, false});
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
  (void)session.tick({0U, kDt, false});
  const auto entity = session.projection().entity_for(1U);

  for (std::uint64_t frame = 1; frame < 8; ++frame) {
    (void)session.tick({frame, kDt, false});
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
  const auto running = session.tick({0U, kDt, false});
  EXPECT_EQ(running.physics_substeps, 1U);

  // A paused host must still see graph edits apply, which is why sync_graph
  // runs before the pause gate -- so a paused tick is not a no-op tick.
  const auto paused = session.tick({1U, kDt, true});
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

  (void)session.tick({0U, kDt, false});

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
    (void)a.tick({frame, kDt, false});
    (void)b.tick({frame, kDt, false});
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