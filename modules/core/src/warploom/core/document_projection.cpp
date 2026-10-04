// SPDX-License-Identifier: MIT
//! SceneDocument -> ECS projection. See document_projection.hpp.

#include "warploom/core/document_projection.hpp"

#include <algorithm>

namespace warploom::editor {

using core::Entity;
using core::World;
namespace {

constexpr std::array<const char*, 3> kTransformProperties = {
    "position", "rotation", "scale"};

//! Read one Vec3 property. Rotation is stored under "rotation" but the
//! document's own property name for it is exactly that, so no aliasing.
[[nodiscard]] bool read_vec3(const SceneObject& object, const char* name,
                             std::array<double, 3>& out) {
  const auto it = object.properties.find(name);
  if (it == object.properties.end()) return false;
  if (it->second.type != PropValue::Type::Vec3) return false;
  out[0] = it->second.vec[0];
  out[1] = it->second.vec[1];
  out[2] = it->second.vec[2];
  return true;
}

}  // namespace

bool read_document_transform(const SceneObject& object,
                             DocumentTransform& out) {
  if (!read_vec3(object, "position", out.position)) return false;
  if (!read_vec3(object, "rotation", out.rotation_deg)) return false;
  if (!read_vec3(object, "scale", out.scale)) return false;
  return true;
}

void collect_driving_clips(const SceneDocument& document,
                           std::uint64_t object_id,
                           std::vector<std::uint64_t>& clip_ids) {
  clip_ids.clear();
  const std::string prefix = std::to_string(object_id) + ":";
  for (const TimelineClip& clip : document.clips) {
    for (const auto& [key, track] : clip.tracks) {
      (void)track;
      // Only a transform channel makes a body timeline-driven. A clip
      // recording "color" must not mark the object's transform kinematic.
      if (key.rfind(prefix, 0) != 0) continue;
      const std::string property = key.substr(prefix.size());
      bool is_transform = false;
      for (const char* name : kTransformProperties) {
        if (property == name) {
          is_transform = true;
          break;
        }
      }
      if (!is_transform) continue;
      // One entry per clip however many transform tracks it holds: the
      // component records "this clip drives me", not per-channel detail.
      clip_ids.push_back(clip.id);
      break;
    }
  }
}

Entity DocumentProjection::entity_for(std::uint64_t object_id) const {
  const auto it = entities_.find(object_id);
  if (it == entities_.end()) return Entity{};
  return it->second;
}

bool DocumentProjection::projected(std::uint64_t object_id) const {
  return entities_.find(object_id) != entities_.end();
}

void DocumentProjection::clear() noexcept {
  // Destroy through the world so archetypes and component storage drop with
  // the entities, rather than leaving the mapping to dangle.
  for (const auto& [id, entity] : entities_) {
    (void)id;
    world_.destroy_entity(entity);
  }
  entities_.clear();
  live_ids_.clear();
  clip_ids_.clear();
}

ProjectionReport DocumentProjection::project(const SceneDocument& document) {
  ProjectionReport report{};

  // Pass 1: mark what the document currently contains and (re)build entities.
  // Document vector order, so entity creation order is reproducible.
  live_ids_.clear();
  live_ids_.reserve(document.objects.size());
  for (const SceneObject& object : document.objects) {
    live_ids_.push_back(object.id);

    const auto existing = entities_.find(object.id);
    Entity entity{};
    if (existing != entities_.end() && world_.is_alive(existing->second)) {
      entity = existing->second;
      ++report.updated;
    } else {
      DocumentTransform transform{};
      if (!read_document_transform(object, transform)) {
        // Not an entity-shaped object (or a broken one). Counted so the
        // caller can tell "nothing to project" from "everything vanished".
        ++report.skipped;
        live_ids_.pop_back();
        continue;
      }
      entity = world_.create_entity();
      world_.add_component<DocumentTransform>(entity, transform);
      (void)world_.add_component<DocumentRef>(
          entity, DocumentRef{object.id, object.type_id});
      entities_[object.id] = entity;
      ++report.created;
    }

    // The transform is refreshed unconditionally: the document may have been
    // edited through undo/redo, which never touches the ECS.
    DocumentTransform transform{};
    if (world_.has_component<DocumentTransform>(entity) &&
        read_document_transform(object, transform)) {
      world_.set_component<DocumentTransform>(entity, transform);
    }
  }

  // Pass 2: destroy entities whose object left the document. Iterating the
  // map (not the world) keeps this deterministic too.
  std::vector<std::uint64_t> orphaned;
  for (const auto& [id, entity] : entities_) {
    const bool still_live =
        std::find(live_ids_.begin(), live_ids_.end(), id) != live_ids_.end();
    if (!still_live) orphaned.push_back(id);
  }
  for (const std::uint64_t id : orphaned) {
    world_.destroy_entity(entities_[id]);
    entities_.erase(id);
    ++report.destroyed;
  }

  // Pass 3: annotate which entities a timeline clip drives. Kept separate from
  // the transform pass so a clip that starts driving an object does not force
  // the object to be recreated (which would drop its identity).
  for (const SceneObject& object : document.objects) {
    const auto it = entities_.find(object.id);
    if (it == entities_.end()) continue;
    collect_driving_clips(document, object.id, clip_ids_);
    TimelineDriven driven{};
    driven.active = !clip_ids_.empty();
    driven.clip_id = driven.active ? clip_ids_.front() : 0;
    // set_component reads before it writes, so the component has to exist
    // first. Entities projected before any clip existed never had it.
    if (world_.has_component<TimelineDriven>(it->second)) {
      (void)world_.set_component<TimelineDriven>(it->second, driven);
    } else {
      (void)world_.add_component<TimelineDriven>(it->second, driven);
    }
  }

  return report;
}

}  // namespace warploom::editor