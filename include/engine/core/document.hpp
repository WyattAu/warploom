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

namespace omnicpp::editor {

//! Current document schema. Bump on breaking changes; `from_json` refuses
//! documents from the future.
inline constexpr std::uint32_t kDocumentSchemaVersion = 1;

//! The typed currency of the document: every property is one of these.
struct PropValue final {
  enum class Type : std::uint8_t { Number, Bool, String, Vec3 };

  Type type{Type::Number};
  double number{0.0};
  bool boolean{false};
  std::string text{};
  double vec[3]{0.0, 0.0, 0.0};

  [[nodiscard]] static PropValue make_number(double v) {
    PropValue p;
    p.type = Type::Number;
    p.number = v;
    return p;
  }
  [[nodiscard]] static PropValue make_bool(bool v) {
    PropValue p;
    p.type = Type::Bool;
    p.boolean = v;
    return p;
  }
  [[nodiscard]] static PropValue make_string(std::string v) {
    PropValue p;
    p.type = Type::String;
    p.text = std::move(v);
    return p;
  }
  [[nodiscard]] static PropValue make_vec3(double x, double y, double z) {
    PropValue p;
    p.type = Type::Vec3;
    p.vec[0] = x;
    p.vec[1] = y;
    p.vec[2] = z;
    return p;
  }

  [[nodiscard]] bool operator==(const PropValue&) const = default;
};

//! One editable scene object: a registry type plus a property bag.
struct SceneObject final {
  std::uint64_t id{0};
  std::uint32_t type_id{0};  // PropertyRegistry-assigned
  std::string name{};
  // std::map (sorted keys) is what makes serialization byte-deterministic.
  std::map<std::string, PropValue> properties{};
};

//! The whole editable scene.
struct SceneDocument final {
  std::uint32_t schema_version{kDocumentSchemaVersion};
  std::uint64_t next_object_id{1};  // monotonically increasing
  std::vector<SceneObject> objects{};

  [[nodiscard]] SceneObject* find(std::uint64_t id);
  [[nodiscard]] const SceneObject* find(std::uint64_t id) const;

  //! Byte-deterministic serialization. Contracts: all numbers finite.
  [[nodiscard]] std::string to_json() const;

  //! Strict parse. On failure returns false with `error` set (includes the
  //! byte offset) and leaves `out` untouched.
  [[nodiscard]] static bool from_json(std::string_view text,
                                      SceneDocument& out, std::string& error);
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
