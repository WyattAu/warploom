#pragma once

//! @file document.hpp
//! @brief M1 editor foundation: serializable scene document + command stack.
//!
//! The document is the authoritative *editable* state the editor mirrors and
//! mutates; every mutation goes through a `Command` executed on the
//! `CommandStack`, which records the information needed to reverse it. That
//! single discipline yields undo/redo, deterministic replay (the same proof
//! machinery as the input-script and telemetry systems), and crash-safe
//! serialization for free.
//!
//! Design contracts (all machine-checked in test_scene_document.cpp):
//!   - `to_json` is byte-deterministic: identical documents serialize to
//!     identical bytes (objects in vector order, properties in sorted key
//!     order, round-trip-safe `%.17g` numbers).
//!   - `apply` then `undo` on any command restores a byte-identical
//!     serialization.
//!   - Parsing is strict: malformed input is rejected with a precise
//!     diagnostic (byte offset included) and never half-mutates.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "warploom/core/prop_value.hpp"
#include "warploom/core/node_graph.hpp"

#include <cfloat>

namespace omnicpp::editor {

//! Current document schema. Bump on breaking changes; `from_json` refuses
//! documents from the future.
//! v2: node_graph + node_layout. v3: timeline clips (G3).
inline constexpr std::uint32_t kDocumentSchemaVersion = 3;

//! One editable scene object: a registry type plus a property bag.
struct SceneObject final {
  std::uint64_t id{0};
  std::uint32_t type_id{0};  // PropertyRegistry-assigned
  std::string name{};
  // std::map (sorted keys) is what makes serialization byte-deterministic.
  std::map<std::string, PropValue> properties{};
};

//! G3: one property sample inside a clip track. `frame_offset` is relative
//! to the owning clip's start_frame (clips stay movable without rewriting
//! samples). Offsets within a track are strictly increasing.
struct ClipSample final {
  std::uint64_t frame_offset{0};
  PropValue value{};
};

//! G3: one recorded channel — object property values sampled over the clip's
//! span. Step-hold evaluation: the value of the last sample at or before the
//! query offset; nothing before the first sample.
struct ClipTrack final {
  std::uint64_t object_id{0};
  std::string property{};
  std::vector<ClipSample> samples{};  // sorted by frame_offset, unique
};

//! G3: one timeline clip — a named frame span holding recorded tracks.
struct TimelineClip final {
  std::uint64_t id{0};
  std::string name{};
  std::uint64_t start_frame{0};
  std::uint64_t length_frames{0};
  //! Key "<object_id>:<property>" (std::map: key-sorted, so serialization
  //! is byte-deterministic).
  std::map<std::string, ClipTrack> tracks{};

  //! Step-hold value of a track at an absolute frame; false when the clip
  //! does not cover the frame or the track does not exist / has no samples
  //! at or before the offset.
  [[nodiscard]] bool evaluate(std::uint64_t frame, std::uint64_t object_id,
                              const std::string& property,
                              PropValue& out) const;
};

//! G3: the canonical track key for one (object, property) channel. Shared
//! by the recorder, playback, and the on-disk format (writer + reader
//! validate the key against its payload).
[[nodiscard]] std::string track_key(std::uint64_t object_id,
                                    const std::string& property);

//! The whole editable scene.
struct SceneDocument final {
  std::uint32_t schema_version{kDocumentSchemaVersion};
  std::uint64_t next_object_id{1};  // monotonically increasing
  std::vector<SceneObject> objects{};
  //! M7: the authored node graph (positions live in node_layout so the
  //! graph core stays view-agnostic and object-free).
  NodeGraph node_graph{};
  std::map<std::uint64_t, std::pair<double, double>> node_layout{};
  //! G3: timeline clips (schema v3). Insertion-ordered; ids from
  //! next_clip_id. Absent from serialization when empty (v2 byte stability).
  std::vector<TimelineClip> clips{};
  std::uint64_t next_clip_id{1};  // monotonically increasing

  [[nodiscard]] TimelineClip* find_clip(std::uint64_t id);
  [[nodiscard]] const TimelineClip* find_clip(std::uint64_t id) const;

  [[nodiscard]] SceneObject* find(std::uint64_t id);
  [[nodiscard]] const SceneObject* find(std::uint64_t id) const;

  //! Byte-deterministic serialization. Contracts: all numbers finite.
  [[nodiscard]] std::string to_json() const;

  //! Strict parse. On failure returns false with `error` set (includes the
  //! byte offset) and leaves `out` untouched.
  [[nodiscard]] static bool from_json(std::string_view text,
                                      SceneDocument& out, std::string& error);

  //! Saves byte-deterministic JSON to `path` (0600 perms). False + `error`
  //! on failure; the file is written atomically via a temp-file rename so a
  //! crash mid-write never corrupts an existing document.
  [[nodiscard]] bool save_to_file(const std::string& path,
                                  std::string& error) const;
  //! Loads a document from `path`. On parse failure `error` carries the
  //! parser diagnostic; on I/O failure a plain error. `out` untouched on
  //! any failure. NOTE: node types are validated against `out`'s registry —
  //! pre-register types before loading (the session does this for you).
  [[nodiscard]] static bool load_from_file(const std::string& path,
                                           SceneDocument& out,
                                           std::string& error);
};

// ============================================================================
// Commands
// ============================================================================

//! A reversible edit. `apply` runs through CommandStack::execute; `undo` must
//! restore byte-identical document state.
class Command {
 public:
  virtual ~Command() = default;
  //! Applies the edit. Returns false + `error` when the edit is invalid
  //! against the current document (e.g. unknown object); the stack treats a
  //! failed apply as if the command never ran.
  [[nodiscard]] virtual bool apply(SceneDocument& doc, std::string& error) = 0;
  //! Reverses the edit. Must not fail (the stack only stores applied
  //! commands); a contract violation aborts.
  virtual void undo(SceneDocument& doc) = 0;
  [[nodiscard]] virtual std::string describe() const = 0;
};

//! Set (or add) one property on one object.
class SetPropertyCommand final : public Command {
 public:
  SetPropertyCommand(std::uint64_t object_id, std::string key,
                     PropValue value);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t object_id_;
  std::string key_;
  PropValue value_;
  // Captured at apply time for the inverse.
  bool existed_{false};
  PropValue old_value_{};
};

//! Create an object with an explicit id (assigned by the caller from the
//! document's `next_object_id`).
class SpawnObjectCommand final : public Command {
 public:
  SpawnObjectCommand(std::uint64_t object_id, std::uint32_t type_id,
                     std::string name,
                     std::map<std::string, PropValue> properties);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t object_id_;
  std::uint32_t type_id_;
  std::string name_;
  std::map<std::string, PropValue> properties_;
  //! True when apply raised next_object_id; undo lowers it back so spawn+undo
  //! round-trips byte-identically.
  bool bumped_id_{false};
};

//! Set several properties on ONE object atomically (single undo step).
//! Used for multi-value payloads like the camera (eye + target + fov).
class SetPropertiesCommand final : public Command {
 public:
  SetPropertiesCommand(std::uint64_t object_id,
                       std::vector<std::pair<std::string, PropValue>> entries);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t object_id_;
  std::vector<std::pair<std::string, PropValue>> entries_;
  // Per-entry captured state, parallel to entries_.
  std::vector<bool> existed_{};
  std::vector<PropValue> old_values_{};
};

//! Remove an object; undo reinserts it at its original index with full state.
class DestroyObjectCommand final : public Command {
 public:
  explicit DestroyObjectCommand(std::uint64_t object_id);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t object_id_;
  // Captured at apply time for the inverse.
  std::size_t index_{0};
  bool applied_{false};
  SceneObject captured_{};
};

// ============================================================================
// Node-graph commands (M7) — same undo contract as the object commands:
// apply + undo restores a byte-identical document serialization.
// ============================================================================

//! Adds a node of `type` at `x,y`. On apply the id is claimed from the
//! document's graph (deterministic ascending); undo removes it and restores
//! the id cursor so spawn+undo round-trips byte-identically.
class AddNodeCommand final : public Command {
 public:
  AddNodeCommand(std::string type, double x, double y);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] std::uint64_t node_id() const noexcept { return node_id_; }

 private:
  std::string type_;
  double x_;
  double y_;
  std::uint64_t node_id_{0};  // claimed at apply
  bool bumped_{false};        // next-id cursor was advanced (for undo)
};

//! Removes a node (and, via the graph, all links touching it); undo restores
//! node + links + id cursor exactly.
class RemoveNodeCommand final : public Command {
 public:
  explicit RemoveNodeCommand(std::uint64_t node_id);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t node_id_;
  GraphNode captured_{};
  std::vector<GraphLink> captured_links_{};
  bool applied_{false};
};

//! G3: adds an empty clip (undo removes it and restores the id cursor).
class AddClipCommand final : public Command {
 public:
  AddClipCommand(std::string name, std::uint64_t start_frame,
                 std::uint64_t length_frames);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] std::uint64_t clip_id() const noexcept { return clip_id_; }

 private:
  std::string name_;
  std::uint64_t start_frame_;
  std::uint64_t length_frames_;
  std::uint64_t clip_id_{0};  // claimed at apply
  bool bumped_{false};
};

//! G3: removes a clip (undo restores clip + id cursor exactly).
class RemoveClipCommand final : public Command {
 public:
  explicit RemoveClipCommand(std::uint64_t clip_id);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t clip_id_;
  TimelineClip captured_{};
  bool applied_{false};
};

//! G3: moves a clip's start frame (undo restores the previous start).
class MoveClipCommand final : public Command {
 public:
  MoveClipCommand(std::uint64_t clip_id, std::uint64_t new_start);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t clip_id_;
  std::uint64_t new_start_;
  std::uint64_t old_start_{0};
  bool applied_{false};
};

//! Links one output pin to one input pin (the graph validates type/cycle);
//! undo removes the link and restores any link the apply displaced on the
//! target input pin.
class LinkNodesCommand final : public Command {
 public:
  LinkNodesCommand(std::uint64_t from_node, std::string from_pin,
                   std::uint64_t to_node, std::string to_pin);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t from_node_;
  std::string from_pin_;
  std::uint64_t to_node_;
  std::string to_pin_;
  bool had_previous_{false};
  GraphLink previous_{};
  bool applied_{false};
};

//! Removes the link feeding an input pin; undo re-adds it.
class UnlinkNodeCommand final : public Command {
 public:
  UnlinkNodeCommand(std::uint64_t to_node, std::string to_pin);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t to_node_;
  std::string to_pin_;
  bool had_link_{false};
  GraphLink captured_{};
};

//! Sets a node parameter; undo restores the previous value (or removes the
//! key when it did not exist).
class SetNodeParamCommand final : public Command {
 public:
  SetNodeParamCommand(std::uint64_t node_id, std::string key, PropValue value);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t node_id_;
  std::string key_;
  PropValue value_;
  bool existed_{false};
  PropValue old_value_{};
};

//! Moves a node's view position; undo restores the previous position (or
//! removes the layout entry when it had none).
class SetNodePositionCommand final : public Command {
 public:
  SetNodePositionCommand(std::uint64_t node_id, double x, double y);

  [[nodiscard]] bool apply(SceneDocument& doc, std::string& error) override;
  void undo(SceneDocument& doc) override;
  [[nodiscard]] std::string describe() const override;

 private:
  std::uint64_t node_id_;
  double x_;
  double y_;
  bool had_previous_{false};
  std::pair<double, double> previous_{0.0, 0.0};
};

//! Undo/redo stack over a document. Every successful `execute` clears the
//! redo branch (standard linear history).
class CommandStack final {
 public:
  explicit CommandStack(SceneDocument& doc) : doc_(&doc) {}

  //! Applies `command`; on success it becomes the new top of the undo stack.
  [[nodiscard]] bool execute(std::unique_ptr<Command> command,
                             std::string& error);
  //! Undoes the most recent command. False when nothing to undo.
  [[nodiscard]] bool undo(std::string& error);
  //! Re-applies the most recently undone command. False when nothing to redo.
  [[nodiscard]] bool redo(std::string& error);

  [[nodiscard]] std::size_t undo_count() const noexcept {
    return undo_.size();
  }
  [[nodiscard]] std::size_t redo_count() const noexcept {
    return redo_.size();
  }
  [[nodiscard]] const Command* undo_top() const noexcept {
    return undo_.empty() ? nullptr : undo_.back().get();
  }
  void clear() {
    undo_.clear();
    redo_.clear();
  }

 private:
  SceneDocument* doc_;
  std::vector<std::unique_ptr<Command>> undo_{};
  std::vector<std::unique_ptr<Command>> redo_{};
};

}  // namespace omnicpp::editor
