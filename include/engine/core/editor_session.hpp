#pragma once

//! @file editor_session.hpp
//! @brief M3 editor session: authoritative document behind the control
//!        protocol (query + edit + undo/redo).
//!
//! `EditorSession` implements `ControlHost` on top of the M1 document:
//!   - scene-editing commands run through the bridge (typed registry
//!     defaults, validation) and execute on the CommandStack — every edit
//!     is undoable and the document stays byte-serializable;
//!   - query commands (list_objects / get_object / schema) serialize
//!     document/registry state back to the client;
//!   - session commands (ping/pause/step/capture) pass through untouched.
//!
//! The viewport embeds this session and mirrors document state into its
//! render structures; tests use it headless. The session never touches
//! GPU/renderer state — that keeps the protocol fully headless-testable.

#include <string>
#include <vector>

#include "engine/core/command_recorder.hpp"
#include "engine/core/control_server.hpp"
#include "engine/core/document.hpp"
#include "engine/core/property_registry.hpp"
#include "engine/core/replay_scrubber.hpp"

namespace omnicpp::editor {

class EditorSession final : public omnicpp::core::ControlHost {
 public:
  EditorSession();

  [[nodiscard]] SceneDocument& document() { return doc_; }
  [[nodiscard]] const SceneDocument& document() const { return doc_; }
  [[nodiscard]] CommandStack& stack() { return stack_; }
  [[nodiscard]] const PropertyRegistry& registry() const {
    return default_registry();
  }

  //! Editor selection (mirrored by hosts for outline/highlight rendering;
  //! 0 = nothing selected). Set via the `select` protocol command.
  [[nodiscard]] std::uint64_t selected_id() const noexcept {
    return selected_id_;
  }

  // -- Graph -> scene bridge (M9) -----------------------------------------
  //! Binds one node output pin to one object property. When `sync_graph()`
  //! runs (once per editor tick), each binding writes the pin's CURRENT
  //! evaluated value into the object property — direct application, NOT a
  //! command (graphs drive state every frame; undoing a driven property is
  //! the graph's job, via node params). Rejected at bind time when object,
  //! property, or pin does not exist or the value type mismatches.
  struct PropertyBinding {
    std::uint64_t node_id{0};
    std::string out_pin{};
    std::uint64_t object_id{0};
    std::string property{};
  };
  //! Returns false + `error` when the binding is invalid (never half-bound).
  [[nodiscard]] bool bind_property(std::uint64_t node_id,
                                   std::string out_pin,
                                   std::uint64_t object_id,
                                   std::string property,
                                   std::string& error);
  //! Removes the binding on an object property (false when none).
  [[nodiscard]] bool unbind_property(std::uint64_t object_id,
                                     const std::string& property);
  [[nodiscard]] const std::vector<PropertyBinding>& bindings() const noexcept {
    return bindings_;
  }
  //! Replaces the document wholesale (file load): the loaded document becomes
  //! authoritative and history is cleared (undo across a load boundary is
  //! meaningless). Bindings and selection reset; borrowed graph pointers
  //! stay valid (the document object's address does not change).
  void reset_from(SceneDocument&& loaded) {
    doc_ = std::move(loaded);
    stack_.clear();
    bindings_.clear();
    selected_id_ = 0;
  }
  //! Runs the graph evaluation, then writes every binding's current pin
  //! value into its bound object property. Returns the number of bindings
  //! applied. Deterministic: bindings apply in insertion order.
  [[nodiscard]] std::size_t sync_graph(std::string& error);

  // -- Replay scrubbing (W1, protocol v1.6) --------------------------------
  //! Captures a checkpoint of the CURRENT document at sim frame `frame`.
  //! Checkpoints live in a bounded ring (see ReplayScrubber); restoring is
  //! hash-verified two ways (bytes -> capture hash, parse -> re-serialize
  //! byte equality) so a scrub is provably lossless.
  [[nodiscard]] bool scrub_start(std::uint64_t frame, std::string& error);
  //! Restores the checkpoint captured at sim frame `frame`. The document is
  //! replaced by the captured state and history CLEARS — undo cannot cross
  //! a time warp (same semantics as a document load). Selection survives
  //! only when the restored document still contains the object.
  [[nodiscard]] bool scrub_to(std::uint64_t frame, std::string& error);
  [[nodiscard]] const ReplayScrubber& scrubber() const noexcept {
    return scrubber_;
  }

  // -- Protocol record/replay (W2, protocol v1.7) ---------------------------
  //! Begins recording protocol commands into a warploom-replay-v1 file
  //! (docs/replay-format.md). Embeds the opening checkpoint at `frame`.
  [[nodiscard]] bool capture_start(std::uint64_t frame,
                                   const std::string& scene,
                                   std::string& error) {
    return recorder_.start(frame, doc_, scene, error);
  }
  //! Ends recording and writes the file atomically (tmp+rename, 0600).
  //! Embeds the closing checkpoint at the current logical frame.
  [[nodiscard]] bool capture_stop(const std::string& path,
                                  std::string& error) {
    return recorder_.stop(path, doc_, error);
  }
  [[nodiscard]] const CommandRecorder& recorder() const noexcept {
    return recorder_;
  }
  //! Records a command that was handled OUTSIDE the session (the viewport
  //! host owns pause/resume/step — the sim loop — so those never reach
  //! on_control). Call after the host handled the command successfully;
  //! recording semantics are identical (recorded-kinds set, frame stamps).
  void record_external(const omnicpp::core::ControlCommand& command) {
    if (!replaying_) recorder_.record(command);
  }
  //! Loads a warploom-replay-v1 file: hash-verified checkpoint hydration
  //! + command-log re-apply in seq order (spec "Load semantics").
  [[nodiscard]] bool load_replay(const std::string& path,
                                 std::string& error);

  // -- ControlHost ---------------------------------------------------------
  [[nodiscard]] omnicpp::core::ControlReply on_control(
      const omnicpp::core::ControlCommand& command) override;
  [[nodiscard]] std::string snapshot_json() const override;

 private:
  [[nodiscard]] omnicpp::core::ControlReply handle_session(
      const omnicpp::core::ControlCommand& command);
  //! Returns true when the kind was a query (reply filled).
  [[nodiscard]] bool handle_query(
      const omnicpp::core::ControlCommand& command,
      omnicpp::core::ControlReply& reply);
  //! Returns false when the kind is not a document edit.
  [[nodiscard]] bool handle_edit(
      const omnicpp::core::ControlCommand& command,
      omnicpp::core::ControlReply& reply);

  SceneDocument doc_{};
  CommandStack stack_{doc_};
  std::uint64_t selected_id_{0};
  CommandRecorder recorder_{};  //!< W2 capture (frame-thread-only)
  bool replaying_{false};       //!< suppresses re-recording during load_replay
  std::vector<PropertyBinding> bindings_{};
  //! W1: checkpoint ring for hash-verified scrubbing. Frame-thread-only
  //! (all access arrives via on_control), matching the session's model.
  ReplayScrubber scrubber_{};
};

}  // namespace omnicpp::editor
