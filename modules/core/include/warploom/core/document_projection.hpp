// SPDX-License-Identifier: MIT
//! Projects a SceneDocument into the ECS, so runtime systems read one store.
//!
//! The document stays the authored truth: it owns ids, undo/redo, and
//! serialization, and nothing here writes back to it. This is a *projection* --
//! document objects become entities carrying a stable back-reference, and the
//! ECS is what physics and rendering read. That replaces the previous
//! arrangement where the viewport re-derived a matrix per object per frame
//! while the ECS had no production consumer at all.
//!
//! Two properties matter for determinism, and both are testable:
//!   - Projection order follows document vector order, so entity creation
//!     order is reproducible.
//!   - The id -> entity mapping is rebuilt deterministically and an object
//!     that disappears destroys exactly its own entity.
//!
//! This header is render-agnostic on purpose: it describes where something is,
//! not how it is drawn, so core never depends on the renderer.

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "warploom/core/document.hpp"
#include "warploom/core/ecs.hpp"
#include "warploom/core/prop_value.hpp"

namespace warploom::editor {

using core::Entity;
using core::World;

//! Where an entity is, in the document's own convention (degrees, Euler XYZ
//! with yaw about Y applied first -- the convention mirror_document_objects
//! used, kept identical so saved scenes look the same).
struct DocumentTransform final {
  std::array<double, 3> position{{0.0, 0.0, 0.0}};
  std::array<double, 3> rotation_deg{{0.0, 0.0, 0.0}};
  std::array<double, 3> scale{{1.0, 1.0, 1.0}};
};

//! Back-reference to the authored object this entity was projected from.
//! Systems that need authored data (name, extra properties) look it up here
//! rather than duplicating it into the ECS.
struct DocumentRef final {
  std::uint64_t object_id{0};
  std::uint32_t type_id{0};
};

//! An object whose transform is animated by a timeline clip. Physics reads
//! this to treat a driven body as kinematic for the frame, which is what stops
//! a recorded clip from fighting the solver.
struct TimelineDriven final {
  bool active{false};
  std::uint64_t clip_id{0};
};

//! Statistics from the last project() call, so callers and tests can assert
//! what actually happened rather than inferring it from the world.
struct ProjectionReport final {
  std::uint32_t created{0};
  std::uint32_t updated{0};
  std::uint32_t destroyed{0};
  //! Objects present in the document but skipped for want of a transform.
  //! Reported rather than silently dropped: a malformed object is a data bug
  //! worth seeing, and a projection that quietly omits it looks like the
  //! renderer lost it.
  std::uint32_t skipped{0};
};

//! Owns the projected world and the document-id -> entity mapping.
class DocumentProjection final {
 public:
  DocumentProjection() = default;
  DocumentProjection(const DocumentProjection&) = delete;
  DocumentProjection& operator=(const DocumentProjection&) = delete;
  DocumentProjection(DocumentProjection&&) = delete;
  DocumentProjection& operator=(DocumentProjection&&) = delete;
  ~DocumentProjection() = default;

  [[nodiscard]] World& world() noexcept { return world_; }
  [[nodiscard]] const World& world() const noexcept { return world_; }

  //! Bring the world in line with `document`. Idempotent: calling it twice
  //! with an unchanged document creates nothing and destroys nothing, which is
  //! what lets the app call it every frame without bookkeeping.
  [[nodiscard]] ProjectionReport project(const SceneDocument& document);

  [[nodiscard]] Entity entity_for(std::uint64_t object_id) const;
  [[nodiscard]] bool projected(std::uint64_t object_id) const;

  //! Destroy every projected entity and forget the mapping. Used when the
  //! document is replaced wholesale (open/save-as) rather than edited.
  void clear() noexcept;

  [[nodiscard]] std::size_t entity_count() const noexcept {
    return entities_.size();
  }

 private:
  World world_{};
  //! document object id -> entity. A map, not a vector, because document ids
  //! are sparse and monotonic.
  std::map<std::uint64_t, Entity> entities_{};
  //! Scratch reused every project() so the steady-state frame allocates
  //! nothing. Held as a member rather than a local for the same reason the
  //! renderer keeps its barrier scratch in thread_local storage.
  std::vector<std::uint64_t> live_ids_{};
  //! Separate scratch for the clip pass, so the two passes cannot clobber
  //! each other's bookkeeping if one is ever reordered.
  std::vector<std::uint64_t> clip_ids_{};
};

//! Read a transform out of an object's property bag. Returns false when a
//! required property is missing or has the wrong type, which the caller
//! reports as skipped rather than substituting a default: a cube with no
//! position is a broken scene, and inventing one would hide it.
[[nodiscard]] bool read_document_transform(const SceneObject& object,
                                           DocumentTransform& out);

//! Timeline clips that drive an object's transform, if any. Fills `clip_ids`
//! with the ids of clips whose tracks write that object's transform, in clip
//! vector order so the result is deterministic.
void collect_driving_clips(const SceneDocument& document,
                           std::uint64_t object_id,
                           std::vector<std::uint64_t>& clip_ids);

}  // namespace warploom::editor