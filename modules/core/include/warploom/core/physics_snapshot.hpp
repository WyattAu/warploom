#pragma once

//! @file physics_snapshot.hpp
//! @brief T4a: byte-deterministic physics state codec for scrub checkpoints.
//!
//! A scrub checkpoint captures the document only today, so a time-warp
//! scrub leaves physics simulating from its pre-warp state while the
//! document jumps back — a silent divergence. This codec serializes the
//! full solver state (configuration knobs + bodies + joints + the
//! session's object->body map) so checkpoints restore physics
//! bit-identically.
//!
//! Format (flat JSON, fixed key order — the parser requires it, and only
//! this codec writes it):
//! {"gravity":G,"substeps":N,"iterations":N,"compliance":C,
//!  "rest_thresh":T,"sleep_thresh":S,"vfp":0,
//!  "bodies":[{"bid":0,"oid":12,"pos":[x,y,z],"vel":[x,y,z],
//!             "ori":[x,y,z,w],"angvel":[x,y,z],
//!             "radius":R,"inv_mass":M,"restitution":E,
//!             "shape":0,"half_height":H},...],
//!  "joints":[{"a":0,"b":1,"rest":L,"compliance":C},...]}
//!
//! - Bodies in body-id order with explicit `bid` (0..n-1, checked on
//!   restore so a torn snapshot cannot remap bodies silently). `oid` is
//!   the document object id or -1 for a map-orphan body.
//! - Floats use std::to_chars' shortest round-trip form (the same
//!   discipline as EditorSession::snapshot_json): exact float32 recovery,
//!   deterministic bytes for a given value.
//! - Sleeping flags, contacts, and solver scratch are deliberately NOT
//!   carried: islands re-derive every step from bodies alone, so they are
//!   a pure function of the restored state (see PhysicsWorld docs).
//! - The determinism-check flag and violation count are diagnostics, not
//!   state, and are not carried either.
//!
//! Threading: none — frame-thread-only like the session. Determinism: same
//! state yields identical bytes on every machine (fixed key order, no
//! map iteration except the caller-supplied map, which the writer walks in
//! body-id order, never map order).

#include <cstdint>
#include <map>
#include <string>

#include "warploom/physics/physics_world.hpp"

namespace warploom::editor {

//! Serializes `world` plus the session's object->body map. Bodies walk in
//! body-id order; the map is consulted (never iterated) for the oid tag.
[[nodiscard]] std::string physics_snapshot_json(
    const ::warploom::physics::PhysicsWorld& world,
    const std::map<std::uint64_t, std::uint32_t>& object_bodies);

//! Restores `world` and `object_bodies` from `json`. All-or-nothing: on
//! any parse/validation failure returns false + error with both outputs
//! untouched. On success the world holds exactly the captured bodies in
//! id order (clear + re-add) and the map equals the captured mapping, so
//! restored body ids match the captured ones.
[[nodiscard]] bool physics_restore_json(
    ::warploom::physics::PhysicsWorld& world,
    std::map<std::uint64_t, std::uint32_t>& object_bodies,
    const std::string& json, std::string& error);

}  // namespace warploom::editor
