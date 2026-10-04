//! @file test_document_projection.cpp
//! @brief Document -> ECS projection proofs: idempotence, stable identity
//!        across edits, exact destruction of removed objects, deterministic
//!        ordering, honest reporting of unprojectable objects, and the
//!        timeline-driven marker.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "warploom/core/document_projection.hpp"

namespace {

using namespace omnicpp::editor;
// The projection bridges warploom::editor (document) and warploom::core (ECS),
// so its own symbols live in omnicpp::editor with core types pulled in.
using omnicpp::editor::DocumentProjection;
using omnicpp::editor::DocumentRef;
using omnicpp::editor::DocumentTransform;
using omnicpp::editor::TimelineDriven;

//! A cube with a complete transform: the shape the projection accepts.
::omnicpp::editor::SceneObject make_cube(std::uint64_t id, double x, double y,
                                         double z) {
  SceneObject object{};
  object.id = id;
  object.type_id = 1U;
  object.name = "cube";
  object.properties["position"] = PropValue::make_vec3(x, y, z);
  object.properties["rotation"] =
      PropValue::make_vec3(0.0, 90.0, 0.0);  // yaw about Y
  object.properties["scale"] = PropValue::make_vec3(2.0, 2.0, 2.0);
  return object;
}

::omnicpp::editor::SceneDocument doc_with(
    std::vector<::omnicpp::editor::SceneObject> objects) {
  SceneDocument document{};
  document.objects = std::move(objects);
  for (const auto& o : document.objects) {
    if (o.id >= document.next_object_id) document.next_object_id = o.id + 1U;
  }
  return document;
}

}  // namespace

TEST(DocumentProjection, CreatesOneEntityPerTransformableObject) {
  DocumentProjection projection;
  const auto document = doc_with({make_cube(1U, 1.0, 2.0, 3.0),
                                  make_cube(2U, -1.0, 0.0, 5.0)});

  const auto report = projection.project(document);
  EXPECT_EQ(report.created, 2U);
  EXPECT_EQ(report.updated, 0U);
  EXPECT_EQ(report.destroyed, 0U);
  EXPECT_EQ(report.skipped, 0U);
  EXPECT_EQ(projection.entity_count(), 2U);

  const auto first = projection.entity_for(1U);
  ASSERT_TRUE(projection.projected(1U));
  ASSERT_TRUE(projection.world().is_alive(first));
  EXPECT_EQ(projection.world().get_component<DocumentRef>(first).object_id, 1U);
  EXPECT_EQ(projection.world().get_component<DocumentTransform>(first).position[0],
            1.0);
  EXPECT_EQ(projection.world().get_component<DocumentTransform>(first).scale[2],
            2.0);
}

TEST(DocumentProjection, ReProjectingAnUnchangedDocumentIsANoOp) {
  DocumentProjection projection;
  const auto document = doc_with({make_cube(1U, 1.0, 0.0, 0.0),
                                  make_cube(2U, 2.0, 0.0, 0.0)});
  (void)projection.project(document);
  const auto entity = projection.entity_for(1U);

  const auto second = projection.project(document);
  EXPECT_EQ(second.created, 0U) << "an unchanged document must create nothing";
  EXPECT_EQ(second.destroyed, 0U);
  EXPECT_EQ(second.updated, 2U);
  EXPECT_EQ(projection.entity_count(), 2U);
  // Identity is the whole point: systems holding an entity must keep it.
  EXPECT_EQ(projection.entity_for(1U).id, entity.id);
  EXPECT_EQ(projection.entity_for(1U).generation, entity.generation);
}

TEST(DocumentProjection, EditsFlowThroughUndoWithoutRecreatingEntities) {
  DocumentProjection projection;
  auto document = doc_with({make_cube(1U, 1.0, 0.0, 0.0)});
  (void)projection.project(document);
  const auto entity = projection.entity_for(1U);

  // The document changes; the ECS transform must follow.
  document.objects[0].properties["position"] =
      PropValue::make_vec3(9.0, 0.0, 0.0);
  const auto report = projection.project(document);
  EXPECT_EQ(report.updated, 1U);
  EXPECT_EQ(report.created, 0U);
  EXPECT_EQ(projection.entity_for(1U).id, entity.id);
  EXPECT_EQ(
      projection.world().get_component<DocumentTransform>(projection.entity_for(1U))
          .position[0],
      9.0);

  // And back again, which is what undo does.
  document.objects[0].properties["position"] =
      PropValue::make_vec3(1.0, 0.0, 0.0);
  (void)projection.project(document);
  EXPECT_EQ(projection.entity_for(1U).id, entity.id);
  EXPECT_EQ(
      projection.world().get_component<DocumentTransform>(projection.entity_for(1U))
          .position[0],
      1.0);
}

TEST(DocumentProjection, RemovingAnObjectDestroysExactlyItsEntity) {
  DocumentProjection projection;
  auto document =
      doc_with({make_cube(1U, 0.0, 0.0, 0.0), make_cube(2U, 1.0, 0.0, 0.0),
                make_cube(3U, 2.0, 0.0, 0.0)});
  (void)projection.project(document);
  const auto doomed = projection.entity_for(2U);
  const auto first = projection.entity_for(1U);
  const auto third = projection.entity_for(3U);

  document.objects.erase(document.objects.begin() + 1);  // drop id 2
  const auto report = projection.project(document);

  EXPECT_EQ(report.destroyed, 1U);
  EXPECT_EQ(projection.entity_count(), 2U);
  EXPECT_FALSE(projection.projected(2U));
  // The destroyed entity is genuinely gone from the world, not merely
  // unmapped -- otherwise a stale handle would keep mutating its storage.
  EXPECT_FALSE(projection.world().is_alive(doomed));
  // And its neighbours are untouched, which is the part that matters: an
  // off-by-one in the orphan sweep would take a live object with it.
  ASSERT_TRUE(projection.projected(1U));
  ASSERT_TRUE(projection.projected(3U));
  EXPECT_EQ(projection.entity_for(1U).id, first.id);
  EXPECT_EQ(projection.entity_for(3U).id, third.id);
  EXPECT_TRUE(projection.world().is_alive(first));
  EXPECT_TRUE(projection.world().is_alive(third));
  EXPECT_NE(first.id, third.id);
}

TEST(DocumentProjection, ReportsObjectsItCannotProject) {
  DocumentProjection projection;
  SceneObject broken = make_cube(1U, 0.0, 0.0, 0.0);
  broken.properties.erase("position");  // no transform to read
  const auto document = doc_with({make_cube(2U, 0.0, 0.0, 0.0), broken});

  const auto report = projection.project(document);
  // Reported, not silently dropped: a broken object otherwise looks like the
  // renderer losing it.
  EXPECT_EQ(report.skipped, 1U);
  EXPECT_EQ(report.created, 1U);
  EXPECT_FALSE(projection.projected(1U));
  EXPECT_TRUE(projection.projected(2U));

  // Fixing the object later projects it rather than needing a rebuild.
  SceneDocument fixed = document;
  fixed.objects[1].properties["position"] =
      PropValue::make_vec3(1.0, 1.0, 1.0);
  const auto after = projection.project(fixed);
  EXPECT_EQ(after.created, 1U);
  EXPECT_EQ(after.skipped, 0U);
  EXPECT_TRUE(projection.projected(1U));
}

TEST(DocumentProjection, ProjectionOrderFollowsDocumentOrder) {
  // Two documents with the same objects in different vector order must
  // produce different creation order -- and each must be reproducible.
  DocumentProjection forward;
  const auto doc_forward =
      doc_with({make_cube(10U, 0.0, 0.0, 0.0), make_cube(20U, 1.0, 0.0, 0.0)});
  (void)forward.project(doc_forward);

  DocumentProjection again;
  (void)again.project(doc_forward);
  EXPECT_EQ(forward.entity_for(10U).id, again.entity_for(10U).id);
  EXPECT_EQ(forward.entity_for(20U).id, again.entity_for(20U).id);

  // Document order, not id order, drives creation.
  const auto doc_reversed =
      doc_with({make_cube(20U, 1.0, 0.0, 0.0), make_cube(10U, 0.0, 0.0, 0.0)});
  DocumentProjection reversed;
  (void)reversed.project(doc_reversed);
  EXPECT_LT(reversed.entity_for(20U).id, reversed.entity_for(10U).id);
}

TEST(DocumentProjection, ClipsMarkingTransformChannelsDriveTheBody) {
  DocumentProjection projection;
  auto document = doc_with({make_cube(1U, 0.0, 0.0, 0.0), make_cube(2U, 0.0, 0.0, 0.0)});

  TimelineClip position_clip{};
  position_clip.id = 7U;
  position_clip.start_frame = 0U;
  position_clip.length_frames = 10U;
  position_clip.tracks[track_key(1U, "position")] =
      ClipTrack{1U, "position", {ClipSample{0U, PropValue::make_vec3(1, 0, 0)}}};

  // A clip that records only "color" must not make the body kinematic.
  TimelineClip color_clip{};
  color_clip.id = 8U;
  color_clip.tracks[track_key(2U, "color")] =
      ClipTrack{2U, "color", {ClipSample{0U, PropValue::make_vec3(1, 0, 0)}}};

  document.clips = {position_clip, color_clip};
  (void)projection.project(document);

  const auto driven = projection.world().get_component<TimelineDriven>(
      projection.entity_for(1U));
  EXPECT_TRUE(driven.active);
  EXPECT_EQ(driven.clip_id, 7U);
  EXPECT_FALSE(projection.world()
                   .get_component<TimelineDriven>(projection.entity_for(2U))
                   .active);
}

TEST(DocumentProjection, ClearDropsEveryEntity) {
  DocumentProjection projection;
  (void)projection.project(doc_with({make_cube(1U, 0.0, 0.0, 0.0),
                                    make_cube(2U, 0.0, 0.0, 0.0)}));
  const auto entity = projection.entity_for(1U);
  projection.clear();
  EXPECT_EQ(projection.entity_count(), 0U);
  EXPECT_FALSE(projection.projected(1U));
  EXPECT_FALSE(projection.world().is_alive(entity));

  // Reprojecting after a wholesale replace starts from a clean world.
  const auto report = projection.project(doc_with({make_cube(1U, 0.0, 0.0, 0.0)}));
  EXPECT_EQ(report.created, 1U);
  EXPECT_TRUE(projection.projected(1U));
}

TEST(DocumentProjection, EmptyDocumentDestroysEverything) {
  DocumentProjection projection;
  (void)projection.project(doc_with({make_cube(1U, 0.0, 0.0, 0.0),
                                    make_cube(2U, 0.0, 0.0, 0.0)}));
  const auto report = projection.project(SceneDocument{});
  EXPECT_EQ(report.destroyed, 2U);
  EXPECT_EQ(report.created, 0U);
  EXPECT_EQ(projection.entity_count(), 0U);
}
